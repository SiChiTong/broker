#ifndef BROKER_PEERING_IMPL_HH
#define BROKER_PEERING_IMPL_HH

#include <cstdint>
#include <string>
#include <utility>

#include <caf/actor.hpp>

#include "broker/peering.hh"

namespace broker {

class peering::impl {
public:
  impl() : endpoint_actor(), peer_actor(), remote(), remote_tuple() {
  }

  impl(caf::actor ea, caf::actor pa, bool r = false,
       std::pair<std::string, uint16_t> rt = std::make_pair("", 0))
    : endpoint_actor(std::move(ea)),
      peer_actor(std::move(pa)),
      remote(r),
      remote_tuple(std::move(rt)) {
  }

  bool operator==(const peering::impl& other) const;

  caf::actor endpoint_actor;
  caf::actor peer_actor;
  bool remote;
  std::pair<std::string, uint16_t> remote_tuple;
};

inline bool peering::impl::operator==(const peering::impl& other) const {
  return endpoint_actor == other.endpoint_actor
         && peer_actor == other.peer_actor && remote == other.remote
         && remote_tuple == other.remote_tuple;
}

template <class Processor>
void serialize(Processor& proc, peering::impl& impl, const unsigned) {
  proc& impl.endpoint_actor;
  proc& impl.peer_actor;
  proc& impl.remote;
  proc& impl.remote_tuple;
}

template <class Processor>
void serialize(Processor& proc, peering& p, const unsigned) {
  proc&* p.pimpl;
}

} // namespace broker

#endif // BROKER_PEERING_IMPL_HH
