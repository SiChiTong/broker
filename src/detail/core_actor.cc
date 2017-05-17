#include "broker/logger.hh" // Must come before any CAF include.

#include <caf/all.hpp>
#include <caf/io/middleman.hpp>

#include "broker/atoms.hh"
#include "broker/backend.hh"
#include "broker/backend_options.hh"
#include "broker/convert.hh"
#include "broker/error.hh"
#include "broker/peer_status.hh"
#include "broker/status.hh"
#include "broker/timeout.hh"
#include "broker/topic.hh"
#include "broker/version.hh"

#include "broker/detail/assert.hh"
#include "broker/detail/clone_actor.hh"
#include "broker/detail/core_actor.hh"
#include "broker/detail/die.hh"
#include "broker/detail/make_backend.hh"
#include "broker/detail/make_unique.hh"
#include "broker/detail/master_actor.hh"
#include "broker/detail/master_resolver.hh"

using namespace caf;

namespace broker {
namespace detail {

const char* core_state::name = "core";

namespace {

// Creates endpoint information from a core actor.
endpoint_info make_info(const caf::actor& a, optional<network_info> net = {}) {
  return {a->node(), a->id(), std::move(net)};
}

endpoint_info make_info(network_info net) {
  return {{}, caf::invalid_actor_id, std::move(net)};
}

// Supervises the connection to an IP address and TCP port.
caf::behavior supervisor(caf::event_based_actor* self, caf::actor core,
                         network_info net, timeout::seconds retry) {
  self->send(self, atom::connect::value);
  self->set_down_handler(
    [=](const caf::down_msg&) {
      BROKER_DEBUG("lost connection to" << to_string(net));
      self->send(core, atom::peer::value, net, peer_status::disconnected);
      self->send(self, atom::connect::value);
    }
  );
  return {
    [=](atom::connect) {
      BROKER_DEBUG("attempting to connect to" << to_string(net));
      auto& mm = self->home_system().middleman();
      auto other = mm.remote_actor(net.address, net.port);
      if (!other && retry != timeout::seconds(0)) {
        // Try again on failure.
        self->delayed_send(self, retry, atom::connect::value);
      } else {
        self->monitor(*other);
        self->send(core, atom::peer::value, net, peer_status::connected,
                   *other);
      }
    }
  };
}

optional<caf::actor> find_remote_master(caf::stateful_actor<core_state>* self,
                                        const std::string& name) {
  // If we don't have a master recorded locally, we could still have a
  // propagated subscription to a remote core hosting a master.
  auto t = name / topics::reserved / topics::master;
  auto s = self->state.subscriptions.find(t.string());
  if (s != self->state.subscriptions.end()) {
    // Only the master subscribes to its inbound topic, so there can be at most
    // a single subscriber.
    BROKER_ASSERT(s->second.subscribers.size() == 1);
    auto& master = *s->second.subscribers.begin();
    return master;
  }
  return nil;
}

} // namespace <anonymous>

void core_state::init(caf::event_based_actor* s, filter_type initial_filter) {
  self = s;
  filter = std::move(initial_filter);
  governor = caf::make_counted<stream_governor>(this);
  auto lsid = governor->local_subscribers().sid();
  local_relay = caf::make_counted<stream_relay>(governor, lsid);
  self->streams().emplace(std::move(lsid), local_relay);
}

caf::strong_actor_ptr core_state::prev_peer_from_handshake() {
  auto& xs = self->current_mailbox_element()->content();
  CAF_ASSERT(xs.match_elements<caf::stream_msg>());
  auto& x = xs.get_as<caf::stream_msg>(0);
  if (caf::holds_alternative<caf::stream_msg::open>(x.content))
    return get<caf::stream_msg::open>(x.content).prev_stage;
  return nullptr;
}

void core_state::update_filter_on_peers() {
  CAF_LOG_TRACE("");
  for (auto& kvp : governor->peers())
    self->send(kvp.first, atom::update::value, filter);
}

void core_state::add_to_filter(filter_type xs) {
  CAF_LOG_TRACE(CAF_ARG(xs));
  // Get initial size of our filter.
  auto s0 = filter.size();
  // Insert new elements then remove duplicates with sort and unique.
  filter.insert(filter.end(), std::make_move_iterator(xs.begin()),
                std::make_move_iterator(xs.end()));
  std::sort(filter.begin(), filter.end());
  auto e = std::unique(filter.begin(), filter.end());
  if (e != filter.end())
    filter.erase(e, filter.end());
  // Update our peers if we have actually changed our filter.
  if (s0 != filter.size()) {
    CAF_LOG_DEBUG("Changed filter to " << filter);
    update_filter_on_peers();
  }
}

bool core_state::has_peer(const caf::actor& x) {
  return pending_peers.count(x) > 0 || connected_peers.count(x) > 0;
}

caf::behavior core_actor(caf::stateful_actor<core_state>* self,
                         filter_type initial_filter) {
  self->state.init(self, std::move(initial_filter));
  self->state.info = make_info(self);
  // We monitor remote inbound peerings and local outbound peerings.
  self->set_down_handler(
    [=](const caf::down_msg& down) {
      /* TODO: still needed? Already tracked by governor.
      BROKER_INFO("got DOWN from peer" << to_string(down.source));
      auto peers = &self->state.peers;
      auto pred = [&](const peer_state& p) {
        return p.actor && p.actor->address() == down.source;
      };
      auto i = std::find_if(peers->begin(), peers->end(), pred);
      BROKER_ASSERT(i != self->state.peers.end());
      const char* desc;
      if (is_outbound(i->info.flags)) {
        BROKER_ASSERT(is_local(i->info.flags));
        desc = "lost local outbound peer";
      } else {
        BROKER_ASSERT(is_inbound(i->info.flags));
        BROKER_ASSERT(is_remote(i->info.flags));
        desc = "lost remote inbound peer";
      }
      BROKER_DEBUG(desc);
      self->send(subscriber, make_status<sc::peer_removed>(i->info.peer, desc));
      peers->erase(i);
      */
    }
  );
  return {
    // --- filter manipulation -------------------------------------------------
    [=](atom::subscribe, filter_type& f) {
      CAF_LOG_TRACE(CAF_ARG(f));
      self->state.add_to_filter(std::move(f));
    },
    // --- peering requests from local actors, i.e., "step 0" ------------------
    [=](atom::peer, actor remote_core) -> result<void> {
      CAF_LOG_TRACE(CAF_ARG(remote_core));
      auto& st = self->state;
      // Sanity checking.
      if (remote_core == nullptr)
        return sec::invalid_argument;
      // Create necessary state and send message to remote core if not already
      // peering with B.
      if (!st.has_peer(remote_core))
        self->send(self * remote_core, atom::peer::value, st.filter, self);
      return unit;
    },
    // --- 3-way handshake for establishing peering streams between A and B ----
    // --- A (this node) performs steps #1 and #3; B performs #2 and #4 --------
    // Step #1: - A demands B shall establish a stream back to A
    //          - A has subscribers to the topics `ts`
    [=](atom::peer, filter_type& peer_ts,
        caf::actor& remote_core) -> stream_type {
      CAF_LOG_TRACE(CAF_ARG(peer_ts) << CAF_ARG(remote_core));
      auto& st = self->state;
      // Reject anonymous peering requests.
      auto p = self->current_sender();
      if (p == nullptr) {
        CAF_LOG_DEBUG("Drop anonymous peering request.");
        return invalid_stream;
      }
      CAF_LOG_DEBUG("received handshake step #1 from" << remote_core
                    << "via" << p << CAF_ARG(actor{self}));
      // Ignore unexpected handshakes as well as handshakes that collide
      // with an already pending handshake.
      if (st.pending_peers.count(remote_core) > 0) {
        CAF_LOG_DEBUG("Drop repeated peering request.");
        return invalid_stream;
      }
      // Especially ignore handshakes from already connected peers.
      if (st.connected_peers.count(remote_core) > 0) {
        CAF_LOG_WARNING("Drop peering request from already connected peer.");
        return invalid_stream;
      }
      // Start streaming.
      auto sid = self->make_stream_id();
      auto peer_ptr = st.governor->add_peer(p, remote_core,
                                            sid, std::move(peer_ts));
      if (peer_ptr == nullptr) {
        CAF_LOG_DEBUG("Drop peering request of already known peer.");
        return invalid_stream;
      }
      st.pending_peers.emplace(remote_core, sid);
      auto& next = self->current_mailbox_element()->stages.back();
      CAF_ASSERT(next != nullptr);
      auto token = std::make_tuple(st.filter, caf::actor{self});
      self->fwd_stream_handshake<stream_type::value_type>(sid, token);
      return {sid, peer_ptr->relay};
    },
    // Step #2: B establishes a stream to A and sends its own filter
    [=](const stream_type& in, filter_type& filter, caf::actor& remote_core) {
      CAF_LOG_TRACE(CAF_ARG(in) << CAF_ARG(filter) << remote_core);
      auto& st = self->state;
      // Reject anonymous peering requests and unrequested handshakes.
      auto p = st.prev_peer_from_handshake();
      if (p == nullptr) {
        CAF_LOG_DEBUG("Drop anonymous peering request.");
        return;
      }
      CAF_LOG_DEBUG("received handshake step #2 from" << remote_core
                    << "via" << p << CAF_ARG(actor{self}));
      // Ignore duplicates.
      if (st.governor->has_peer(remote_core)) {
        CAF_LOG_DEBUG("Drop repeated handshake phase #2.");
        return;
      }
      // Start streaming in opposite direction.
      auto sid = self->make_stream_id();
      auto peer_ptr = st.governor->add_peer(p, std::move(remote_core),
                                            sid, std::move(filter));
      peer_ptr->incoming_sid = in.id();
      self->streams().emplace(sid, peer_ptr->relay);
      self->streams().emplace(in.id(), peer_ptr->relay);
      peer_ptr->send_stream_handshake();
    },
    // Step #3: - A establishes a stream to B
    //          - B has a stream to A and vice versa now
    [=](const stream_type& in, ok_atom, caf::actor& remote_core) {
      CAF_LOG_TRACE(CAF_ARG(in));
      auto& st = self->state;
      // Reject anonymous peering requests and unrequested handshakes.
      auto p = st.prev_peer_from_handshake();
      if (p == nullptr) {
        CAF_LOG_DEBUG("Ignored anonymous peering request.");
        return;
      }
      CAF_LOG_DEBUG("received handshake step #3 from" << remote_core
                    << "via" << p << CAF_ARG(actor{self}));
      // Reject step #3 handshake if this actor didn't receive a step #1
      // handshake previously.
      auto i = st.pending_peers.find(remote_core);
      if (i == st.pending_peers.end()) {
        CAF_LOG_WARNING("Received a step #3 handshake, but no #1 previously.");
        return;
      }
      st.pending_peers.erase(i);
      // Get peer data and install stream handler.
      auto peer_ptr = st.governor->peer(remote_core);
      if (!peer_ptr) {
        CAF_LOG_WARNING("could not get peer data for " << remote_core);
        return;
      }
      auto res = self->streams().emplace(in.id(), peer_ptr->relay);
      if (!res.second) {
        CAF_LOG_WARNING("Stream already existed.");
      }
    },
    // --- asynchronous communication to peers ---------------------------------
    [=](atom::update, filter_type f) {
      CAF_LOG_TRACE(CAF_ARG(f));
      auto& st = self->state;
      auto p = caf::actor_cast<caf::actor>(self->current_sender());
      if (p == nullptr) {
        CAF_LOG_DEBUG("Received anonymous filter update.");
        return;
      }
      if (!st.governor->update_peer(p, std::move(f)))
        CAF_LOG_DEBUG("Cannot update filter of unknown peer:" << to_string(p));
    },
    // --- communication to local actors: incoming streams and subscriptions ---
    [=](atom::join, filter_type& filter) -> expected<stream_type> {
      CAF_LOG_TRACE(CAF_ARG(filter));
      // Check if the message is not anonymous and does contain a next stage.
      auto& st = self->state;
      auto cs = self->current_sender();
      if (cs == nullptr)
        return sec::cannot_add_downstream;
      auto& stages = self->current_mailbox_element()->stages;
      if (stages.empty()) {
        CAF_LOG_ERROR("Cannot join a data stream without downstream.");
        auto rp = self->make_response_promise();
        rp.deliver(sec::no_downstream_stages_defined);
        return stream_type{stream_id{nullptr, 0}, nullptr};
      }
      auto next = stages.back();
      CAF_ASSERT(next != nullptr);
      // Initiate stream handshake and add subscriber to the governor.
      std::tuple<> token;
      auto sid = st.governor->local_subscribers().sid();
      self->fwd_stream_handshake<stream_type::value_type>(sid, token);
      st.governor->local_subscribers().add_path(cs);
      st.governor->local_subscribers().set_filter(cs, filter);
      CAF_LOG_DEBUG("updates lanes: "
                    << st.governor->local_subscribers().lanes());
      // Update our filter to receive updates on all subscribed topics.
      st.add_to_filter(std::move(filter));
      return stream_type{sid, st.local_relay};
    },
    [=](const stream_type& in) {
      CAF_LOG_TRACE(CAF_ARG(in));
      auto& st = self->state;
      auto& cs = self->current_sender();
      if (cs == nullptr) {
        return;
      }
      self->streams().emplace(in.id(), st.local_relay);
    },
    [=](atom::publish, topic& t, data& x) {
      CAF_LOG_TRACE(CAF_ARG(t) << CAF_ARG(x));
      self->state.governor->push(std::move(t), std::move(x));
    },
    // --- data store management -----------------------------------------------
    [=](atom::store, atom::master, atom::attach, const std::string& name,
        backend backend_type,
        backend_options& opts) -> caf::result<caf::actor> {
      CAF_LOG_TRACE(CAF_ARG(name) << CAF_ARG(backend_type) << CAF_ARG(opts));
      BROKER_DEBUG("attaching master:" << name);
      // Sanity check: this message must be a point-to-point message.
      auto& cme = *self->current_mailbox_element();
      if (!cme.stages.empty())
        return ec::unspecified;
      auto& st = self->state;
      auto i = st.masters.find(name);
      if (i != st.masters.end()) {
        BROKER_DEBUG("found local master");
        return i->second;
      }
      if (find_remote_master(self, name)) {
        BROKER_WARNING("remote master with same name exists already");
        return ec::master_exists;
      }
      BROKER_DEBUG("instantiating backend");
      auto ptr = make_backend(backend_type, std::move(opts));
      BROKER_ASSERT(ptr);
      BROKER_DEBUG("spawn new master");
      auto ms = self->spawn<caf::linked + caf::lazy_init>(master_actor, self,
                                                          name, std::move(ptr));
      st.masters.emplace(name, ms);
      // Subscribe to messages directly targeted at the master.
      std::tuple<> token;
      // fwd_stream_handshake expects the next stage in cme.stages
      auto ms_ptr = actor_cast<strong_actor_ptr>(ms);
      cme.stages.emplace_back(ms_ptr);
      auto sid = st.governor->local_subscribers().sid();
      self->fwd_stream_handshake<stream_type::value_type>(sid, token, true);
      // Update governor and filter.
      st.governor->local_subscribers().add_path(ms_ptr);
      filter_type filter{name / topics::reserved / topics::master};
      st.governor->local_subscribers().set_filter(std::move(ms_ptr), filter);
      st.add_to_filter(std::move(filter));
      return ms;
    },
    [=](atom::store, atom::clone, atom::attach,
        std::string& name) -> caf::result<caf::actor> {
      BROKER_DEBUG("attaching clone:" << name);
      // Sanity check: this message must be a point-to-point message.
      auto& cme = *self->current_mailbox_element();
      if (!cme.stages.empty())
        return ec::unspecified;
      auto spawn_clone = [=](const caf::actor& master) -> caf::actor {
        BROKER_DEBUG("spawn new clone");
        auto clone = self->spawn<linked + lazy_init>(clone_actor, self,
                                                     master, name);
        auto& st = self->state;
        st.clones.emplace(name, clone);
        // Subscribe to updates.
        filter_type f{name / topics::reserved / topics::clone};
        std::tuple<> token;
        auto sid = st.governor->local_subscribers().sid();
        auto cptr = actor_cast<strong_actor_ptr>(clone);
        self->current_mailbox_element()->stages.emplace_back(cptr);
        st.governor->local_subscribers().add_path(cptr);
        st.governor->local_subscribers().set_filter(cptr, f);
        self->fwd_stream_handshake<stream_type::value_type>(sid, token, true);
        st.add_to_filter(std::move(f));
        // Instruct master to generate a snapshot.
        self->state.governor->push(
          name / topics::reserved / topics::master,
          make_internal_command<snapshot_command>(self));
        return clone;
      };
      auto& peers = self->state.governor->peers();
      auto i = self->state.masters.find(name);
      if (i != self->state.masters.end()) {
        BROKER_DEBUG("found local master, using direct link");
        return spawn_clone(i->second);
      } else if (peers.empty()) {
        BROKER_DEBUG("no peers to ask for the master");
        return ec::no_such_master;
      }
      auto resolv = self->spawn<caf::lazy_init>(master_resolver);
      auto rp = self->make_response_promise<caf::actor>();
      std::vector<caf::actor> tmp;
      for (auto& kvp : peers)
        tmp.emplace_back(kvp.first);
      self->request(resolv, caf::infinite, std::move(tmp), std::move(name))
      .then(
        [=](actor& master) mutable {
          BROKER_DEBUG("received result from resolver:" << master);
          self->state.masters.emplace(name, master);
          rp.deliver(spawn_clone(std::move(master)));
        },
        [=](caf::error& err) mutable {
          BROKER_DEBUG("received error from resolver:" << err);
          rp.deliver(std::move(err));
        }
      );
      return rp;
    },
    [=](atom::store, atom::master, atom::get,
        const std::string& name) -> result<actor> {
      auto i = self->state.masters.find(name);
      if (i != self->state.masters.end())
        return i->second;
      return ec::no_such_master;
    },
    [=](atom::store, atom::master, atom::resolve,
        const std::string& name) -> result<actor> {
      auto i = self->state.masters.find(name);
      if (i != self->state.masters.end()) {
        BROKER_DEBUG("found local master, using direct link");
        return i->second;
      }
      auto resolv = self->spawn<caf::lazy_init>(master_resolver);
      auto rp = self->make_response_promise<caf::actor>();
      std::vector<caf::actor> tmp;
      for (auto& kvp : self->state.governor->peers())
        tmp.emplace_back(kvp.first);
      self->request(resolv, caf::infinite, std::move(tmp), std::move(name))
      .then(
        [=](actor& master) mutable {
          BROKER_DEBUG("received result from resolver:" << master);
          self->state.masters.emplace(name, master);
          rp.deliver(master);
        },
        [=](caf::error& err) mutable {
          BROKER_DEBUG("received error from resolver:" << err);
          rp.deliver(std::move(err));
        }
      );
      return rp;
    }
  };
}

} // namespace detail
} // namespace broker
