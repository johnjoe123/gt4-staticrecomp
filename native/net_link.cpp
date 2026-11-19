// UDP frame link to the gt4net bridge. See net_link.hpp.
#include "net_link.hpp"

#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_handle = SOCKET;
constexpr socket_handle kNoSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_handle = int;
constexpr socket_handle kNoSocket = -1;
#endif

namespace gt4::net {

struct UdpLink::Socket {
  socket_handle handle = kNoSocket;
};

UdpLink::UdpLink(std::uint16_t port) : socket_(std::make_unique<Socket>()) {
#ifdef _WIN32
  WSADATA data;
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return;
#endif
  const socket_handle handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (handle == kNoSocket) return;
  sockaddr_in peer{};
  peer.sin_family = AF_INET;
  peer.sin_port = htons(port);
  peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  bool ready = ::connect(handle, reinterpret_cast<const sockaddr *>(&peer), sizeof peer) == 0;
#ifdef _WIN32
  u_long nonblocking = 1;
  ready = ready && ioctlsocket(handle, FIONBIO, &nonblocking) == 0;
  // Windows reports an ICMP "port unreachable" (no bridge yet) as an error on the next call; turn that off.
  DWORD off = 0, returned = 0;
  WSAIoctl(handle, _WSAIOW(IOC_VENDOR, 12) /* SIO_UDP_CONNRESET */, &off, sizeof off, nullptr, 0, &returned, nullptr, nullptr);
#else
  ready = ready && fcntl(handle, F_SETFL, fcntl(handle, F_GETFL, 0) | O_NONBLOCK) == 0;
#endif
  if (!ready) {
#ifdef _WIN32
    closesocket(handle);
#else
    ::close(handle);
#endif
    return;
  }
  socket_->handle = handle;
  tick(); // first hello
}

UdpLink::~UdpLink() {
  if (socket_->handle == kNoSocket) return;
#ifdef _WIN32
  closesocket(socket_->handle);
#else
  ::close(socket_->handle);
#endif
}

bool UdpLink::ok() const { return socket_->handle != kNoSocket; }

void UdpLink::send(std::span<const std::uint8_t> frame) {
  if (!ok()) return;
  ::send(socket_->handle, reinterpret_cast<const char *>(frame.data()), static_cast<int>(frame.size()), 0);
}

bool UdpLink::receive(std::vector<std::uint8_t> &frame) {
  if (!ok()) return false;
  frame.resize(2048);
  const auto got = ::recv(socket_->handle, reinterpret_cast<char *>(frame.data()), static_cast<int>(frame.size()), 0);
  if (got <= 0) {
    frame.clear();
    return false;
  }
  frame.resize(static_cast<std::size_t>(got));
  return true;
}

void UdpLink::tick() {
  if (ticks_++ % 1000 != 0 || !ok()) return; // about once a second
  const char hello = 0;
  ::send(socket_->handle, &hello, 1, 0);
}

bool parse_mac(const std::string &text, Mac &mac) {
  unsigned parts[6];
  char extra;
  if (std::sscanf(text.c_str(), "%2x:%2x:%2x:%2x:%2x:%2x%c", &parts[0], &parts[1], &parts[2], &parts[3], &parts[4],
                  &parts[5], &extra) != 6)
    return false;
  if (parts[0] & 1u) return false; // a group address cannot be a station address
  for (unsigned i = 0; i < 6; ++i) mac[i] = static_cast<std::uint8_t>(parts[i]);
  return true;
}

Settings settings_from_environment() {
  Settings settings;
  const char *enabled = std::getenv("GT4_NET");
  settings.enabled = enabled && *enabled && *enabled != '0';
  if (const char *port = std::getenv("GT4_NET_LINK_PORT")) {
    const long value = std::strtol(port, nullptr, 10);
    if (value > 0 && value < 65536) settings.link_port = static_cast<std::uint16_t>(value);
  }
  settings.mac = default_mac(settings.link_port);
  if (const char *mac = std::getenv("GT4_NET_MAC")) {
    if (!parse_mac(mac, settings.mac)) std::fprintf(stderr, "[net] GT4_NET_MAC is not aa:bb:cc:dd:ee:ff; using the default\n");
  }
  return settings;
}

} // namespace gt4::net
