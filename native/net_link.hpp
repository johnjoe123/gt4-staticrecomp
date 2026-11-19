#pragma once
// The game's side of the frame link: one UDP socket to the gt4net bridge on this PC (one datagram per Ethernet frame;
// a one-byte datagram is a hello, so the bridge knows where the game listens). Never blocks.

#include "net_smap.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace gt4::net {

inline constexpr std::uint16_t kDefaultLinkPort = 47624;

class UdpLink final : public FrameLink {
public:
  // Opens the socket toward 127.0.0.1:`port`. ok() is false when the socket could not be made.
  explicit UdpLink(std::uint16_t port);
  ~UdpLink() override;
  UdpLink(const UdpLink &) = delete;
  UdpLink &operator=(const UdpLink &) = delete;

  bool ok() const;
  void send(std::span<const std::uint8_t> frame) override;
  bool receive(std::vector<std::uint8_t> &frame) override;
  void tick() override;

private:
  struct Socket;
  std::unique_ptr<Socket> socket_;
  unsigned ticks_ = 0;
};

// The player's choice, from the environment the launcher sets: GT4_NET=1 turns the adapter on, GT4_NET_LINK_PORT
// picks the bridge port, GT4_NET_MAC (aa:bb:cc:dd:ee:ff) overrides the address.
struct Settings {
  bool enabled = false;
  std::uint16_t link_port = kDefaultLinkPort;
  Mac mac{};
};
Settings settings_from_environment();
bool parse_mac(const std::string &text, Mac &mac);

} // namespace gt4::net
