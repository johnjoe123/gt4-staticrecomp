#pragma once
// PS2 Network Adaptor model for the IOP: the DEV9 expansion-bay registers (0x1F801460..0x1F80147F), the SPEED chip
// (0x10000000, IOP interrupt line 13, IOP DMA channel 8) and its SMAP Ethernet function (FIFOs, buffer descriptor
// rings, EMAC3 register block, MII PHY, MAC address EEPROM). The game's own DEV9/SMAP/INET modules run unchanged on
// top of it; whole Ethernet frames leave and arrive through a FrameLink.
//
// Written from the public register layout documented by the open-source PS2 homebrew headers (speedregs.h,
// smapregs.h) and the 93C46 and IEEE 802.3 clause 22 data sheets. No PCSX2 source was read for this file.
//
// docs/ONLINE-LAN.md describes the whole path (adapter, link, bridge, lobby server).

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <span>
#include <vector>

namespace gt4::net {

inline constexpr std::uint32_t kSpeedBase = 0x10000000u;
inline constexpr std::uint32_t kSpeedSize = 0x00010000u;
inline constexpr std::uint32_t kDev9Base = 0x1F801460u;
inline constexpr std::uint32_t kDev9End = 0x1F801480u;
inline constexpr unsigned kDev9InterruptLine = 13;
inline constexpr unsigned kDev9DmaChannel = 8;
inline constexpr std::size_t kMinFrame = 14;
inline constexpr std::size_t kMaxFrame = 1514; // without the frame check sequence

using Mac = std::array<std::uint8_t, 6>;

// Where frames go. send() never blocks; receive() returns false when nothing is waiting.
class FrameLink {
public:
  virtual ~FrameLink() = default;
  virtual void send(std::span<const std::uint8_t> frame) = 0;
  virtual bool receive(std::vector<std::uint8_t> &frame) = 0;
  // Called about once a millisecond of IOP time, for keep-alives.
  virtual void tick() {}
};

// What the adapter needs from the machine: guest memory for DMA and the interrupt line.
struct Bus {
  std::function<std::uint8_t(std::uint32_t)> read8;
  std::function<void(std::uint32_t, std::uint8_t)> write8;
  std::function<void()> raise_interrupt;
};

struct Counters {
  std::uint64_t tx_frames = 0, rx_frames = 0, rx_dropped = 0, eeprom_reads = 0, phy_operations = 0;
};

class Smap {
public:
  Smap(Bus bus, FrameLink *link, Mac mac);

  void reset();

  // SPEED/SMAP registers: `offset` is the address minus kSpeedBase, `size` is 1, 2 or 4 bytes.
  std::uint32_t read(std::uint32_t offset, unsigned size);
  void write(std::uint32_t offset, std::uint32_t value, unsigned size);

  // DEV9 registers: `address` in kDev9Base..kDev9End.
  std::uint32_t dev9_read(std::uint32_t address);
  void dev9_write(std::uint32_t address, std::uint32_t value);

  // IOP DMA channel 8. `to_device` is CHCR bit 0 (memory to device). Returns false when no FIFO asked for DMA.
  bool dma(std::uint32_t address, std::uint32_t bytes, bool to_device);

  // Once per millisecond of IOP time: moves waiting frames from the link into the receive FIFO.
  void poll();

  const Mac &mac() const { return mac_; }
  const Counters &counters() const { return counters_; }
  bool receiver_enabled() const;

private:
  static constexpr std::size_t kTxFifo = 0x1000, kRxFifo = 0x4000, kDescriptors = 64;

  struct Descriptor {
    std::uint16_t control = 0, reserved = 0, length = 0, pointer = 0;
  };

  void update_interrupt();
  void set_interrupt(std::uint16_t bits);
  void eeprom_write(std::uint8_t value);
  std::uint32_t emac_read(std::uint32_t reg) const;
  void emac_write(std::uint32_t reg, std::uint32_t value);
  void emac_reset();
  void phy_operation();
  std::uint16_t phy_read(unsigned reg);
  void phy_write(unsigned reg, std::uint16_t value);
  void transmit();
  bool deliver(std::span<const std::uint8_t> frame);
  void reset_tx_fifo();
  void reset_rx_fifo();
  std::uint16_t &descriptor_word(std::uint32_t offset);
  void trace(const char *what, std::uint32_t offset, std::uint32_t value, unsigned size);

  Bus bus_;
  FrameLink *link_;
  Mac mac_;
  bool trace_ = false;
  Counters counters_;

  std::array<std::uint16_t, 16> dev9_{};
  std::array<std::uint16_t, 0x80> speed_{}; // plain storage for SPEED registers without behaviour
  std::uint16_t interrupt_status_ = 0, interrupt_mask_ = 0;

  // 93C46 serial EEPROM behind SPD_R_PIO_DATA.
  std::array<std::uint16_t, 64> eeprom_{};
  std::uint8_t pio_direction_ = 0, pio_data_ = 0;
  enum class EepromState { Idle, Command, Read } eeprom_state_ = EepromState::Idle;
  unsigned eeprom_bits_ = 0, eeprom_command_ = 0, eeprom_address_ = 0, eeprom_bit_ = 0;
  bool eeprom_out_ = false;

  std::uint8_t bd_mode_ = 0;
  std::uint8_t tx_control_ = 0, rx_control_ = 0;
  std::uint16_t tx_write_ = 0, tx_size_ = 0, rx_read_ = 0, rx_size_ = 0;
  std::uint8_t tx_frames_ = 0;
  std::array<std::uint8_t, kTxFifo> tx_fifo_{};
  std::array<std::uint8_t, kRxFifo> rx_fifo_{};
  std::uint16_t rx_write_ = 0;
  std::size_t rx_used_ = 0;
  std::deque<std::uint16_t> rx_sizes_; // FIFO bytes of each frame the driver has not released yet
  std::deque<std::vector<std::uint8_t>> backlog_; // frames waiting for FIFO room or a free descriptor
  std::array<Descriptor, kDescriptors> tx_bd_{}, rx_bd_{};
  unsigned tx_index_ = 0, rx_index_ = 0;

  std::array<std::uint32_t, 0x1C> emac_{};
  std::array<std::uint16_t, 32> phy_{};
};

// Stable locally administered address for this PC and link port: 02:47:54:xx:xx:xx.
Mac default_mac(std::uint16_t link_port);

} // namespace gt4::net
