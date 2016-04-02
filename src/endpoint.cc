#include <caf/actor_system.hpp>
#include <caf/send.hpp>
#include <caf/io/middleman.hpp>

#include "broker/broker.hh"

#include "endpoint_impl.hh"

namespace broker {

endpoint::endpoint(std::string name, int flags)
  : pimpl(new impl(this, std::move(name), flags)) {
}

endpoint::~endpoint() = default;

endpoint::endpoint(endpoint&& other) = default;

endpoint& endpoint::operator=(endpoint&& other) = default;

const std::string& endpoint::name() const {
  return pimpl->name;
}

int endpoint::flags() const {
  return pimpl->flags;
}

void endpoint::set_flags(int flags) {
  pimpl->flags = flags;
  caf::anon_send(pimpl->actor, flags_atom::value, flags);
}

int endpoint::last_errno() const {
  return pimpl->last_errno;
}

const std::string& endpoint::last_error() const {
  return pimpl->last_error;
}

bool endpoint::listen(uint16_t port, const char* addr, bool reuse_addr) {
  try {
    broker_system->middleman().publish(pimpl->actor, port, addr, reuse_addr);
  } catch (const std::exception& e) {
    pimpl->last_errno = 0;
    pimpl->last_error = e.what();
    return false;
  }
  return true;
}

peering endpoint::peer(std::string addr, uint16_t port,
                       std::chrono::duration<double> retry) {
  auto port_addr = std::pair<std::string, uint16_t>(addr, port);
  peering rval;
  for (const auto& peer : pimpl->peers)
    if (peer.remote() && port_addr == peer.remote_tuple()) {
      rval = peer;
      break;
    }
  if (rval)
    caf::anon_send(rval.pimpl->peer_actor, peerstat_atom::value);
  else {
    auto h = handle_to_actor(pimpl->outgoing_conns.handle());
    auto a = broker_system->spawn<endpoint_proxy_actor>(
      pimpl->actor, pimpl->name, addr, port, retry, h);
    a->link_to(pimpl->self);
    rval = peering(std::unique_ptr<peering::impl>(
      new peering::impl(pimpl->actor, std::move(a), true, port_addr)));
    pimpl->peers.insert(rval);
  }
  return rval;
}

peering endpoint::peer(const endpoint& e) {
  if (this == &e)
    return {};
  peering p(std::unique_ptr<peering::impl>(
    new peering::impl(pimpl->actor, e.pimpl->actor)));
  pimpl->peers.insert(p);
  caf::anon_send(pimpl->actor, peer_atom::value, e.pimpl->actor,
                 *p.pimpl.get());
  return p;
}

bool endpoint::unpeer(peering p) {
  if (!p)
    return false;
  auto it = pimpl->peers.find(p);
  if (it == pimpl->peers.end())
    return false;
  pimpl->peers.erase(it);
  if (p.remote())
    // The proxy actor initiates unpeer messages.
    caf::anon_send(p.pimpl->peer_actor, quit_atom::value);
  else {
    caf::anon_send(pimpl->actor, unpeer_atom::value, p.pimpl->peer_actor);
    caf::anon_send(p.pimpl->peer_actor, unpeer_atom::value, pimpl->actor);
  }
  return true;
}

const outgoing_connection_status_queue&
endpoint::outgoing_connection_status() const {
  return pimpl->outgoing_conns;
}

const incoming_connection_status_queue&
endpoint::incoming_connection_status() const {
  return pimpl->incoming_conns;
}

void endpoint::send(topic t, message msg, int flags) const {
  caf::anon_send(pimpl->actor, std::move(t), std::move(msg), flags);
}

void endpoint::publish(topic t) {
  caf::anon_send(pimpl->actor, acl_pub_atom::value, t);
}

void endpoint::unpublish(topic t) {
  caf::anon_send(pimpl->actor, acl_unpub_atom::value, t);
}

void endpoint::advertise(topic t) {
  caf::anon_send(pimpl->actor, advert_atom::value, t);
}

void endpoint::unadvertise(topic t) {
  caf::anon_send(pimpl->actor, unadvert_atom::value, t);
}

void* endpoint::handle() const {
  return &pimpl->actor;
}

} // namespace broker

// Begin C API
#include "broker/broker.h"
using std::nothrow;

void broker_deque_of_incoming_connection_status_delete(
  broker_deque_of_incoming_connection_status* d) {
  delete reinterpret_cast<std::deque<broker::incoming_connection_status>*>(d);
}

size_t broker_deque_of_incoming_connection_status_size(
  const broker_deque_of_incoming_connection_status* d) {
  using deque_ptr = const std::deque<broker::incoming_connection_status>*;
  auto dd = reinterpret_cast<deque_ptr>(d);
  return dd->size();
}

broker_incoming_connection_status*
broker_deque_of_incoming_connection_status_at(
  broker_deque_of_incoming_connection_status* d, size_t idx) {
  using deque_ptr = std::deque<broker::incoming_connection_status>*;
  auto dd = reinterpret_cast<deque_ptr>(d);
  return reinterpret_cast<broker_incoming_connection_status*>(&(*dd)[idx]);
}

void broker_deque_of_incoming_connection_status_erase(
  broker_deque_of_incoming_connection_status* d, size_t idx) {
  using deque_ptr = std::deque<broker::incoming_connection_status>*;
  auto dd = reinterpret_cast<deque_ptr>(d);
  dd->erase(dd->begin() + idx);
}

int broker_incoming_connection_status_queue_fd(
  const broker_incoming_connection_status_queue* q) {
  using queue_ptr = const broker::incoming_connection_status_queue*;
  auto qq = reinterpret_cast<queue_ptr>(q);
  return qq->fd();
}

broker_deque_of_incoming_connection_status*
broker_incoming_connection_status_queue_want_pop(
  const broker_incoming_connection_status_queue* q) {
  auto rval = new (nothrow) std::deque<broker::incoming_connection_status>;
  if (!rval)
    return nullptr;
  using queue_ptr = const broker::incoming_connection_status_queue*;
  auto qq = reinterpret_cast<queue_ptr>(q);
  *rval = qq->want_pop();
  return reinterpret_cast<broker_deque_of_incoming_connection_status*>(rval);
}

broker_deque_of_incoming_connection_status*
broker_incoming_connection_status_queue_need_pop(
  const broker_incoming_connection_status_queue* q) {
  auto rval = new (nothrow) std::deque<broker::incoming_connection_status>;
  if (!rval)
    return nullptr;
  using queue_ptr = const broker::incoming_connection_status_queue*;
  auto qq = reinterpret_cast<queue_ptr>(q);
  *rval = qq->need_pop();
  return reinterpret_cast<broker_deque_of_incoming_connection_status*>(rval);
}

void broker_deque_of_outgoing_connection_status_delete(
  broker_deque_of_outgoing_connection_status* d) {
  delete reinterpret_cast<std::deque<broker::outgoing_connection_status>*>(d);
}

size_t broker_deque_of_outgoing_connection_status_size(
  const broker_deque_of_outgoing_connection_status* d) {
  using deque_ptr = const std::deque<broker::outgoing_connection_status>*;
  auto dd = reinterpret_cast<deque_ptr>(d);
  return dd->size();
}

broker_outgoing_connection_status*
broker_deque_of_outgoing_connection_status_at(
  broker_deque_of_outgoing_connection_status* d, size_t idx) {
  using deque_ptr = std::deque<broker::outgoing_connection_status>*;
  auto dd = reinterpret_cast<deque_ptr>(d);
  return reinterpret_cast<broker_outgoing_connection_status*>(&(*dd)[idx]);
}

void broker_deque_of_outgoing_connection_status_erase(
  broker_deque_of_outgoing_connection_status* d, size_t idx) {
  using deque_ptr = std::deque<broker::outgoing_connection_status>*;
  auto dd = reinterpret_cast<deque_ptr>(d);
  dd->erase(dd->begin() + idx);
}

int broker_outgoing_connection_status_queue_fd(
  const broker_outgoing_connection_status_queue* q) {
  using queue_ptr = const broker::outgoing_connection_status_queue*;
  auto qq = reinterpret_cast<queue_ptr>(q);
  return qq->fd();
}

broker_deque_of_outgoing_connection_status*
broker_outgoing_connection_status_queue_want_pop(
  const broker_outgoing_connection_status_queue* q) {
  auto rval = new (nothrow) std::deque<broker::outgoing_connection_status>;
  if (!rval)
    return nullptr;
  using queue_ptr = const broker::outgoing_connection_status_queue*;
  auto qq = reinterpret_cast<queue_ptr>(q);
  *rval = qq->want_pop();
  return reinterpret_cast<broker_deque_of_outgoing_connection_status*>(rval);
}

broker_deque_of_outgoing_connection_status*
broker_outgoing_connection_status_queue_need_pop(
  const broker_outgoing_connection_status_queue* q) {
  auto rval = new (nothrow) std::deque<broker::outgoing_connection_status>;
  if (!rval)
    return nullptr;
  using queue_ptr = const broker::outgoing_connection_status_queue*;
  auto qq = reinterpret_cast<queue_ptr>(q);
  *rval = qq->need_pop();
  return reinterpret_cast<broker_deque_of_outgoing_connection_status*>(rval);
}

broker_endpoint* broker_endpoint_create(const char* name) {
  try {
    auto rval = new broker::endpoint(name);
    return reinterpret_cast<broker_endpoint*>(rval);
  } catch (std::bad_alloc&) {
    return nullptr;
  }
}

broker_endpoint* broker_endpoint_create_with_flags(const char* name,
                                                   int flags) {
  try {
    auto rval = new broker::endpoint(name, flags);
    return reinterpret_cast<broker_endpoint*>(rval);
  } catch (std::bad_alloc&) {
    return nullptr;
  }
}

void broker_endpoint_delete(broker_endpoint* e) {
  delete reinterpret_cast<broker::endpoint*>(e);
}

const char* broker_endpoint_name(const broker_endpoint* e) {
  auto ee = reinterpret_cast<const broker::endpoint*>(e);
  return ee->name().data();
}

int broker_endpoint_flags(const broker_endpoint* e) {
  auto ee = reinterpret_cast<const broker::endpoint*>(e);
  return ee->flags();
}

void broker_endpoint_set_flags(broker_endpoint* e, int flags) {
  auto ee = reinterpret_cast<broker::endpoint*>(e);
  return ee->set_flags(flags);
}

int broker_endpoint_last_errno(const broker_endpoint* e) {
  auto ee = reinterpret_cast<const broker::endpoint*>(e);
  return ee->last_errno();
}

const char* broker_endpoint_last_error(const broker_endpoint* e) {
  auto ee = reinterpret_cast<const broker::endpoint*>(e);
  return ee->last_error().data();
}

int broker_endpoint_listen(broker_endpoint* e, uint16_t port, const char* addr,
                           int reuse_addr) {
  auto ee = reinterpret_cast<broker::endpoint*>(e);
  return ee->listen(port, addr, reuse_addr);
}

broker_peering* broker_endpoint_peer_remotely(broker_endpoint* e,
                                              const char* addr, uint16_t port,
                                              double retry_interval) {
  auto rval = new (nothrow) broker::peering;
  if (!rval)
    return nullptr;
  auto ee = reinterpret_cast<broker::endpoint*>(e);
  auto retry = std::chrono::duration<double>(retry_interval);
  try {
    *rval = ee->peer(addr, port, retry);
  } catch (std::bad_alloc&) {
    delete rval;
    return nullptr;
  }
  return reinterpret_cast<broker_peering*>(rval);
}

broker_peering* broker_endpoint_peer_locally(broker_endpoint* self,
                                             const broker_endpoint* other) {
  auto rval = new (nothrow) broker::peering;
  if (!rval)
    return nullptr;
  auto s = reinterpret_cast<broker::endpoint*>(self);
  auto o = reinterpret_cast<const broker::endpoint*>(other);
  *rval = s->peer(*o);
  return reinterpret_cast<broker_peering*>(rval);
}

int broker_endpoint_unpeer(broker_endpoint* e, const broker_peering* p) {
  auto ee = reinterpret_cast<broker::endpoint*>(e);
  auto pp = reinterpret_cast<const broker::peering*>(p);
  return ee->unpeer(*pp);
}

const broker_outgoing_connection_status_queue*
broker_endpoint_outgoing_connection_status(const broker_endpoint* e) {
  auto ee = reinterpret_cast<const broker::endpoint*>(e);
  return reinterpret_cast<const broker_outgoing_connection_status_queue*>(
    &ee->outgoing_connection_status());
}

const broker_incoming_connection_status_queue*
broker_endpoint_incoming_connection_status(const broker_endpoint* e) {
  auto ee = reinterpret_cast<const broker::endpoint*>(e);
  return reinterpret_cast<const broker_incoming_connection_status_queue*>(
    &ee->incoming_connection_status());
}

int broker_endpoint_send(broker_endpoint* e, const broker_string* topic,
                         const broker_message* msg) {
  auto ee = reinterpret_cast<broker::endpoint*>(e);
  auto tt = reinterpret_cast<const std::string*>(topic);
  auto mm = reinterpret_cast<const broker::message*>(msg);
  try {
    ee->send(*tt, *mm);
  } catch (std::bad_alloc&) {
    return 0;
  }
  return 1;
}

int broker_endpoint_send_with_flags(broker_endpoint* e,
                                    const broker_string* topic,
                                    const broker_message* msg, int flags) {
  auto ee = reinterpret_cast<broker::endpoint*>(e);
  auto tt = reinterpret_cast<const std::string*>(topic);
  auto mm = reinterpret_cast<const broker::message*>(msg);
  try {
    ee->send(*tt, *mm, flags);
  } catch (std::bad_alloc&) {
    return 0;
  }
  return 1;
}

int broker_endpoint_publish(broker_endpoint* e, const broker_string* topic) {
  auto ee = reinterpret_cast<broker::endpoint*>(e);
  auto tt = reinterpret_cast<const std::string*>(topic);
  try {
    ee->publish(*tt);
  } catch (std::bad_alloc&) {
    return 0;
  }
  return 1;
}

int broker_endpoint_unpublish(broker_endpoint* e, const broker_string* topic) {
  auto ee = reinterpret_cast<broker::endpoint*>(e);
  auto tt = reinterpret_cast<const std::string*>(topic);
  try {
    ee->unpublish(*tt);
  } catch (std::bad_alloc&) {
    return 0;
  }
  return 1;
}

int broker_endpoint_advertise(broker_endpoint* e, const broker_string* topic) {
  auto ee = reinterpret_cast<broker::endpoint*>(e);
  auto tt = reinterpret_cast<const std::string*>(topic);
  try {
    ee->advertise(*tt);
  } catch (std::bad_alloc&) {
    return 0;
  }
  return 1;
}

int broker_endpoint_unadvertise(broker_endpoint* e,
                                const broker_string* topic) {
  auto ee = reinterpret_cast<broker::endpoint*>(e);
  auto tt = reinterpret_cast<const std::string*>(topic);
  try {
    ee->unadvertise(*tt);
  } catch (std::bad_alloc&) {
    return 0;
  }
  return 1;
}
