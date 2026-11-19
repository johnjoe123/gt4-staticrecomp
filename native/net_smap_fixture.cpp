// net_smap_fixture: the Network Adaptor model driven the way a guest driver drives it: DEV9 probe, MAC address from
// the serial EEPROM, PHY status over the station management register, transmit by PIO and by DMA, receive with the
// descriptor ring, interrupts, and the UDP link to the bridge over loopback.
#include "net_link.hpp"
#include "net_smap.hpp"

#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

using namespace gt4::net;

namespace {

int failures = 0;
void require(bool condition, const char *what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

struct FakeLink final : FrameLink {
  std::vector<std::vector<std::uint8_t>> sent;
  std::deque<std::vector<std::uint8_t>> incoming;
  void send(std::span<const std::uint8_t> frame) override { sent.emplace_back(frame.begin(), frame.end()); }
  bool receive(std::vector<std::uint8_t> &frame) override {
    if (incoming.empty()) return false;
    frame = std::move(incoming.front());
    incoming.pop_front();
    return true;
  }
};

struct Machine {
  std::vector<std::uint8_t> ram = std::vector<std::uint8_t>(0x10000);
  int interrupts = 0;
  FakeLink link;
  Mac mac{0x02, 0x47, 0x54, 0xAA, 0xBB, 0xCC};
  Smap smap{Bus{[this](std::uint32_t a) { return ram[a]; }, [this](std::uint32_t a, std::uint8_t v) { ram[a] = v; },
                [this] { ++interrupts; }},
            &link, mac};

  std::uint16_t r16(std::uint32_t o) { return static_cast<std::uint16_t>(smap.read(o, 2)); }
  void w16(std::uint32_t o, std::uint16_t v) { smap.write(o, v, 2); }
  void w8(std::uint32_t o, std::uint8_t v) { smap.write(o, v, 1); }
  std::uint32_t emac_get(std::uint32_t o) { return (static_cast<std::uint32_t>(r16(o)) << 16) | r16(o + 2); }
  void emac_set(std::uint32_t o, std::uint32_t v) {
    w16(o, static_cast<std::uint16_t>(v >> 16));
    w16(o + 2, static_cast<std::uint16_t>(v));
  }
};

std::vector<std::uint8_t> test_frame(std::size_t size, std::uint8_t seed) {
  std::vector<std::uint8_t> frame(size);
  for (std::size_t i = 0; i < size; ++i) frame[i] = static_cast<std::uint8_t>(seed + i * 7);
  frame[0] = 0xFF; // broadcast destination
  return frame;
}

void test_dev9_probe() {
  Machine m;
  require((m.smap.dev9_read(0x1F80146E) & 0xF0) == 0x30, "DEV9 reports an expansion bay");
  m.smap.dev9_write(0x1F80146C, 0x0005);
  require(m.smap.dev9_read(0x1F80146C) == 0x0005, "DEV9 power register keeps its value");
  m.smap.dev9_write(0x1F801462, 0xFFFF);
  require((m.smap.dev9_read(0x1F801462) & 1) == 0, "a device is connected");
  require(m.r16(0x04) & 0x0001, "SPEED capabilities include SMAP");
  require(!(m.r16(0x04) & 0x0002), "no hard disk");
  require(m.r16(0x02) >= 0x0011, "SPEED revision allows DMA");
}

// Reads `count` words the way a driver bit-bangs the 93C46: start bit, READ opcode, six address bits, then 16 clocks
// per word with the data bit sampled while the clock is high.
std::vector<std::uint16_t> eeprom_read(Machine &m, unsigned address, unsigned count) {
  const auto bit_out = [&m](bool bit) {
    const std::uint8_t base = static_cast<std::uint8_t>(0x80 | (bit ? 0x20 : 0));
    m.w8(0x2E, base);
    m.w8(0x2E, base | 0x40);
    m.w8(0x2E, base);
  };
  m.w8(0x2C, 0xE1);
  m.w8(0x2E, 0x80);
  bit_out(true);
  bit_out(true);
  bit_out(false);
  for (int bit = 5; bit >= 0; --bit) bit_out((address >> bit) & 1);
  require(!(m.smap.read(0x2E, 1) & 0x10), "EEPROM dummy bit is zero");
  std::vector<std::uint16_t> words;
  for (unsigned w = 0; w < count; ++w) {
    std::uint16_t word = 0;
    for (int bit = 0; bit < 16; ++bit) {
      m.w8(0x2E, 0xC0);
      word = static_cast<std::uint16_t>((word << 1) | ((m.smap.read(0x2E, 1) >> 4) & 1));
      m.w8(0x2E, 0x80);
    }
    words.push_back(word);
  }
  m.w8(0x2E, 0x00);
  m.w8(0x2C, 0x01);
  return words;
}

void test_eeprom() {
  Machine m;
  const auto words = eeprom_read(m, 0, 4);
  Mac read{};
  std::memcpy(read.data(), words.data(), 6); // the driver copies the words as they lie in memory
  require(read == m.mac, "EEPROM holds the MAC address");
  require(static_cast<std::uint16_t>(words[0] + words[1] + words[2]) == words[3], "EEPROM checksum");
  require(eeprom_read(m, 1, 1)[0] == words[1], "EEPROM read from another address");
  require(m.smap.counters().eeprom_reads == 2, "EEPROM reads counted");
}

void test_phy_and_emac() {
  Machine m;
  // Soft reset completes at once.
  m.emac_set(0x2000, 1u << 29);
  require(!(m.emac_get(0x2000) & (1u << 29)), "EMAC soft reset clears itself");
  // Read PHY register 1 (status) at address 1.
  m.emac_set(0x205C, (1u << 12) | (1u << 5) | 1u);
  const std::uint32_t sta = m.emac_get(0x205C);
  require(sta & (1u << 15), "PHY operation completes");
  require((sta >> 16) & 0x0004, "link is up");
  require((sta >> 16) & 0x0020, "auto-negotiation complete");
  // Write then read back register 4.
  m.emac_set(0x205C, (0x05E1u << 16) | (2u << 12) | (1u << 5) | 4u);
  m.emac_set(0x205C, (1u << 12) | (1u << 5) | 4u);
  require((m.emac_get(0x205C) >> 16) == 0x05E1, "PHY register write is kept");
  // PHY reset bit reads back clear.
  m.emac_set(0x205C, (0x8000u << 16) | (2u << 12) | (1u << 5) | 0u);
  m.emac_set(0x205C, (1u << 12) | (1u << 5) | 0u);
  require(!((m.emac_get(0x205C) >> 16) & 0x8000), "PHY reset completes");
  // Station address registers keep their values; MODE0 reports the MACs idle.
  m.emac_set(0x201C, 0x00000247);
  m.emac_set(0x2020, 0x54AABBCC);
  require(m.emac_get(0x201C) == 0x0247 && m.emac_get(0x2020) == 0x54AABBCC, "EMAC address registers");
  require((m.emac_get(0x2000) >> 30) == 3, "MACs report idle");
}

void queue_tx(Machine &m, unsigned index, std::uint16_t pointer, std::uint16_t length) {
  const std::uint32_t bd = 0x3000 + index * 8;
  m.w16(bd + 4, length);
  m.w16(bd + 6, static_cast<std::uint16_t>(0x1000 + pointer));
  m.w16(bd + 0, 0x8000 | 0x4000);
}

void test_transmit() {
  Machine m;
  m.w8(0x1000, 0x01); // reset the transmit FIFO
  require(!(m.smap.read(0x1000, 1) & 1), "FIFO reset completes");
  m.w16(0x2A, 0x007C); // enable all SMAP interrupts
  // Frame 1 by PIO, 61 bytes written as 16 words.
  const auto first = test_frame(61, 3);
  for (std::size_t i = 0; i < 64; i += 4) {
    std::uint32_t word = 0;
    for (unsigned b = 0; b < 4; ++b) word |= static_cast<std::uint32_t>(i + b < first.size() ? first[i + b] : 0) << (8 * b);
    m.smap.write(0x1100, word, 4);
  }
  queue_tx(m, 0, 0, 61);
  m.w8(0x1010, 0);
  // Frame 2 by DMA, 128 bytes from guest memory.
  const auto second = test_frame(128, 9);
  std::memcpy(&m.ram[0x4000], second.data(), second.size());
  m.w16(0x1008, 1);
  m.w8(0x1000, 0x02);
  require(m.smap.dma(0x4000, 128, true), "transmit DMA accepted");
  require(!(m.smap.read(0x1000, 1) & 2), "DMA request clears");
  queue_tx(m, 1, 64, 128);
  m.w8(0x1010, 0);
  require(m.smap.read(0x100C, 1) == 2, "two frames counted in the FIFO");
  m.emac_set(0x2008, 1u << 31); // get new packet
  require(m.link.sent.size() == 2, "both frames left");
  require(m.link.sent.size() == 2 && m.link.sent[0] == first, "PIO frame is intact");
  require(m.link.sent.size() == 2 && m.link.sent[1] == second, "DMA frame is intact");
  require(!(m.emac_get(0x2008) & (1u << 31)), "get-new-packet clears");
  require(!(m.r16(0x3000) & 0x8000) && !(m.r16(0x3008) & 0x8000), "descriptors handed back");
  require(m.r16(0x28) & 0x0004, "transmit-done interrupt cause");
  require(m.interrupts > 0, "interrupt line raised");
  m.w16(0x128, 0x0004);
  require(!(m.r16(0x28) & 0x0004), "interrupt cause cleared");
  // The ring continues at descriptor 2, and the FIFO wraps.
  m.w16(0x1004, 0x0FF0);
  const auto third = test_frame(60, 21);
  for (std::size_t i = 0; i < 60; i += 4) {
    std::uint32_t word;
    std::memcpy(&word, &third[i], 4);
    m.smap.write(0x1100, word, 4);
  }
  queue_tx(m, 2, 0x0FF0, 60);
  m.emac_set(0x2008, 1u << 31);
  require(m.link.sent.size() == 3 && m.link.sent[2] == third, "frame across the FIFO wrap");
}

void test_receive() {
  Machine m;
  m.w8(0x1030, 0x01);
  for (unsigned i = 0; i < 64; ++i) m.w16(0x3200 + i * 8, 0x8000); // every receive descriptor empty
  m.w16(0x2A, 0x007C);
  // Frames that arrive while the receiver is off are not kept.
  m.link.incoming.push_back(test_frame(100, 1));
  m.smap.poll();
  require(m.smap.read(0x103C, 1) == 0 && m.interrupts == 0, "receiver off: nothing delivered");
  m.emac_set(0x2000, (1u << 28) | (1u << 27));
  const auto a = test_frame(100, 5), b = test_frame(1514, 6), tiny = test_frame(20, 7);
  m.link.incoming.push_back(a);
  m.link.incoming.push_back(b);
  m.link.incoming.push_back(test_frame(13, 0));   // too short for Ethernet: dropped
  m.link.incoming.push_back(test_frame(1600, 0)); // too long: dropped
  m.link.incoming.push_back(tiny);
  m.smap.poll();
  require(m.smap.read(0x103C, 1) == 3, "three frames wait");
  require(m.r16(0x28) & 0x0020, "receive interrupt cause");
  require(m.interrupts > 0, "interrupt line raised");
  // Driver side: descriptor 0 by PIO.
  require(!(m.r16(0x3200) & 0x8000), "descriptor 0 filled");
  require(m.r16(0x3204) == 104, "length includes the four check bytes");
  std::uint16_t pointer = static_cast<std::uint16_t>(m.r16(0x3206) - 0x4000);
  m.w16(0x1034, pointer);
  std::vector<std::uint8_t> got;
  for (unsigned i = 0; i < 104; i += 4) {
    const std::uint32_t word = m.smap.read(0x1200, 4);
    for (unsigned k = 0; k < 4; ++k) got.push_back(static_cast<std::uint8_t>(word >> (8 * k)));
  }
  got.resize(100);
  require(got == a, "PIO frame is intact");
  m.w8(0x1040, 0);
  m.w16(0x3200, 0x8000);
  // Descriptor 1 by DMA.
  require(m.r16(0x320C) == 1518, "full-size frame length");
  pointer = static_cast<std::uint16_t>(m.r16(0x320E) - 0x4000);
  m.w16(0x1034, pointer);
  m.w16(0x1038, 1520 / 128);
  m.w8(0x1030, 0x02);
  require(m.smap.dma(0x8000, 1520, false), "receive DMA accepted");
  require(std::memcmp(&m.ram[0x8000], b.data(), b.size()) == 0, "DMA frame is intact");
  m.w8(0x1040, 0);
  m.w16(0x3208, 0x8000);
  // Descriptor 2: a short frame is padded to the Ethernet minimum.
  require(m.r16(0x3214) == 64, "short frame padded to 60 bytes plus check bytes");
  m.w8(0x1040, 0);
  m.w16(0x3210, 0x8000);
  require(m.smap.read(0x103C, 1) == 0, "all frames released");
  require(m.smap.counters().rx_frames == 3, "receive counter");
  // With no empty descriptor, frames wait and arrive once the driver frees one.
  for (unsigned i = 0; i < 64; ++i) m.w16(0x3200 + i * 8, 0x0000);
  m.w16(0x128, 0x0020);
  m.link.incoming.push_back(a);
  m.smap.poll();
  require(!(m.r16(0x28) & 0x0020), "no descriptor: nothing delivered");
  m.w16(0x3200 + 3 * 8, 0x8000);
  m.smap.poll();
  require((m.r16(0x28) & 0x0020) && m.r16(0x3200 + 3 * 8 + 4) == 104, "delivered when a descriptor is free");
  // Many frames: the FIFO never overruns and nothing is lost while the driver keeps up.
  Machine n;
  n.w8(0x1030, 0x01);
  for (unsigned i = 0; i < 64; ++i) n.w16(0x3200 + i * 8, 0x8000);
  n.emac_set(0x2000, 1u << 27);
  unsigned index = 0, received = 0;
  for (unsigned round = 0; round < 40; ++round) {
    for (unsigned k = 0; k < 20; ++k) n.link.incoming.push_back(test_frame(1514, static_cast<std::uint8_t>(round)));
    n.smap.poll();
    while (!(n.r16(0x3200 + index * 8) & 0x8000)) {
      const std::uint16_t at = static_cast<std::uint16_t>(n.r16(0x3200 + index * 8 + 6) - 0x4000);
      n.w16(0x1034, at);
      const std::uint32_t word = n.smap.read(0x1200, 4);
      require((word & 0xFF) == 0xFF, "frame start is where the descriptor says");
      n.w8(0x1040, 0);
      n.w16(0x3200 + index * 8, 0x8000);
      index = (index + 1) % 64;
      ++received;
      n.smap.poll();
    }
  }
  require(received == 800, "800 full-size frames received in order");
}

void test_mac_helpers() {
  Mac mac{};
  require(parse_mac("02:47:54:0a:0B:ff", mac) && mac == Mac{0x02, 0x47, 0x54, 0x0A, 0x0B, 0xFF}, "MAC text parses");
  require(!parse_mac("02:47:54:0a:0b", mac), "short MAC text refused");
  require(!parse_mac("01:00:5e:00:00:01", mac), "group address refused");
  require(!parse_mac("02:47:54:0a:0b:ff:00", mac), "long MAC text refused");
  const Mac one = default_mac(47624), two = default_mac(47625);
  require(one[0] == 0x02 && one[1] == 0x47 && one[2] == 0x54, "default MAC is locally administered");
  require(one != two, "two games on one PC get different addresses");
  require(one == default_mac(47624), "default MAC is stable");
}

#ifndef _WIN32
// The bridge's side of the link, over real loopback sockets.
void test_udp_link() {
  const int bridge = ::socket(AF_INET, SOCK_DGRAM, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  require(::bind(bridge, reinterpret_cast<sockaddr *>(&address), sizeof address) == 0, "bridge socket binds");
  socklen_t size = sizeof address;
  getsockname(bridge, reinterpret_cast<sockaddr *>(&address), &size);
  timeval timeout{2, 0};
  setsockopt(bridge, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);

  UdpLink link(ntohs(address.sin_port));
  require(link.ok(), "link opens");
  std::uint8_t buffer[2048];
  sockaddr_in game{};
  socklen_t game_size = sizeof game;
  auto got = recvfrom(bridge, buffer, sizeof buffer, 0, reinterpret_cast<sockaddr *>(&game), &game_size);
  require(got == 1, "the game says hello when the link opens");
  const auto frame = test_frame(300, 4);
  link.send(frame);
  got = recv(bridge, buffer, sizeof buffer, 0);
  require(got == 300 && std::memcmp(buffer, frame.data(), 300) == 0, "frame reaches the bridge");
  std::vector<std::uint8_t> received;
  require(!link.receive(received), "nothing waiting: receive does not block");
  sendto(bridge, frame.data(), frame.size(), 0, reinterpret_cast<sockaddr *>(&game), game_size);
  bool arrived = false;
  for (int tries = 0; tries < 200 && !arrived; ++tries) {
    arrived = link.receive(received);
    if (!arrived) usleep(1000);
  }
  require(arrived && received == frame, "frame reaches the game");
  for (int i = 0; i < 1000; ++i) link.tick();
  got = recv(bridge, buffer, sizeof buffer, 0);
  require(got == 1, "hello repeats about once a second of ticks");
  ::close(bridge);
}
#endif

} // namespace

int main() {
  test_dev9_probe();
  test_eeprom();
  test_phy_and_emac();
  test_transmit();
  test_receive();
  test_mac_helpers();
#ifndef _WIN32
  test_udp_link();
#endif
  if (failures) {
    std::fprintf(stderr, "net_smap_fixture: %d failure(s)\n", failures);
    return 1;
  }
  std::puts("net_smap_fixture: ok");
  return 0;
}
