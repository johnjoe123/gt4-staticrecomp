// The machine's Network Adaptor instance. See net_runtime.hpp.
#include "net_runtime.hpp"

#include "net_link.hpp"

#include <cstdio>
#include <memory>

namespace gt4::net {

namespace {

struct Machine {
  Settings settings = settings_from_environment();
  std::unique_ptr<UdpLink> link;
  std::unique_ptr<Smap> adapter;
  bool active = false;

  Machine() {
    if (!settings.enabled) return;
    link = std::make_unique<UdpLink>(settings.link_port);
    if (!link->ok()) {
      std::fprintf(stderr, "[net] cannot open the link to the bridge on 127.0.0.1:%u; the network adapter stays off\n",
                   settings.link_port);
      return;
    }
    active = true;
    const Mac &m = settings.mac;
    std::fprintf(stderr, "[net] network adapter on, bridge link 127.0.0.1:%u, address %02X:%02X:%02X:%02X:%02X:%02X\n",
                 settings.link_port, m[0], m[1], m[2], m[3], m[4], m[5]);
  }
};

Machine &machine() {
  static Machine instance;
  return instance;
}

} // namespace

bool adapter_active() { return machine().active; }

Smap &machine_adapter(const Bus &bus) {
  Machine &m = machine();
  if (!m.adapter) m.adapter = std::make_unique<Smap>(bus, m.link.get(), m.settings.mac);
  return *m.adapter;
}

} // namespace gt4::net
