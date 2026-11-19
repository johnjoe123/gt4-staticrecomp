// PS2 Network Adaptor model. See net_smap.hpp.
#include "net_smap.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gt4::net {

namespace {

// SPEED registers (offsets from kSpeedBase).
constexpr std::uint32_t kRev = 0x00, kRev1 = 0x02, kRev3 = 0x04, kRev8 = 0x0E;
constexpr std::uint32_t kIntrStat = 0x28, kIntrMask = 0x2A, kPioDir = 0x2C, kPioData = 0x2E;
constexpr std::uint16_t kCapsSmap = 0x0001;

// SMAP registers.
constexpr std::uint32_t kBdMode = 0x102, kIntrClr = 0x128, kIntrEnable = 0x12A;
constexpr std::uint32_t kTxCtrl = 0x1000, kTxWrPtr = 0x1004, kTxSize = 0x1008, kTxFrameCnt = 0x100C, kTxFrameInc = 0x1010;
constexpr std::uint32_t kRxCtrl = 0x1030, kRxRdPtr = 0x1034, kRxSize = 0x1038, kRxFrameCnt = 0x103C, kRxFrameDec = 0x1040;
constexpr std::uint32_t kTxData = 0x1100, kRxData = 0x1200;
constexpr std::uint32_t kEmacBase = 0x2000, kEmacEnd = 0x2070;
constexpr std::uint32_t kTxBdBase = 0x3000, kRxBdBase = 0x3200, kBdEnd = 0x3400;
constexpr std::uint8_t kFifoReset = 0x01, kFifoDmaEnable = 0x02;

// Interrupt bits (SPD_R_INTR_STAT).
constexpr std::uint16_t kIntrEmac3 = 1u << 6, kIntrRxEnd = 1u << 5, kIntrTxEnd = 1u << 4, kIntrRxDnv = 1u << 3, kIntrTxDnv = 1u << 2;
constexpr std::uint16_t kIntrAll = kIntrEmac3 | kIntrRxEnd | kIntrTxEnd | kIntrRxDnv | kIntrTxDnv;

// EMAC3 registers (index = offset / 4).
constexpr std::uint32_t kMode0 = 0, kTxMode0 = 2, kEmacIntrStat = 5, kStaCtrl = 0x17;
constexpr std::uint32_t kRxMacIdle = 1u << 31, kTxMacIdle = 1u << 30, kSoftReset = 1u << 29, kRxMacEnable = 1u << 27;
constexpr std::uint32_t kGetNewPacket = 1u << 31;
constexpr std::uint32_t kPhyOpComplete = 1u << 15, kPhyRead = 1u << 12, kPhyWrite = 2u << 12, kPhyCommandMask = 3u << 12;

// Buffer descriptors.
constexpr std::uint16_t kBdTxReady = 0x8000, kBdRxEmpty = 0x8000;
constexpr std::uint16_t kTxFifoPointerBase = 0x1000, kRxFifoPointerBase = 0x4000;

// MII registers.
constexpr unsigned kBmcr = 0, kBmsr = 1, kPhyId1 = 2, kPhyId2 = 3, kAnar = 4, kAnlpar = 5, kAner = 6, kPhySts = 0x10;

bool env_flag(const char *name) {
  const char *value = std::getenv(name);
  return value && *value && *value != '0';
}

} // namespace

Mac default_mac(std::uint16_t link_port) {
  std::uint32_t hash = 2166136261u; // FNV-1a over the computer and user names and the link port
  const auto mix = [&hash](const char *text) {
    for (; text && *text; ++text) hash = (hash ^ static_cast<std::uint8_t>(*text)) * 16777619u;
  };
  mix(std::getenv("COMPUTERNAME"));
  mix(std::getenv("HOSTNAME"));
  mix(std::getenv("USERNAME"));
  mix(std::getenv("USER"));
  hash = (hash ^ link_port) * 16777619u;
  hash ^= hash >> 15;
  return {0x02, 0x47, 0x54, static_cast<std::uint8_t>(hash >> 16), static_cast<std::uint8_t>(hash >> 8),
          static_cast<std::uint8_t>(hash)};
}

Smap::Smap(Bus bus, FrameLink *link, Mac mac) : bus_(std::move(bus)), link_(link), mac_(mac) {
  trace_ = env_flag("GT4_NET_TRACE");
  // The EEPROM holds the address as three little-endian words, then their 16-bit sum.
  std::uint16_t sum = 0;
  for (unsigned i = 0; i < 3; ++i) {
    eeprom_[i] = static_cast<std::uint16_t>(mac_[2 * i] | (mac_[2 * i + 1] << 8));
    sum = static_cast<std::uint16_t>(sum + eeprom_[i]);
  }
  eeprom_[3] = sum;
  reset();
}

void Smap::reset() {
  dev9_.fill(0);
  speed_.fill(0);
  interrupt_status_ = interrupt_mask_ = 0;
  pio_direction_ = pio_data_ = 0;
  eeprom_state_ = EepromState::Idle;
  eeprom_bits_ = eeprom_command_ = eeprom_address_ = eeprom_bit_ = 0;
  eeprom_out_ = false;
  bd_mode_ = 0;
  reset_tx_fifo();
  reset_rx_fifo();
  tx_bd_.fill({});
  rx_bd_.fill({});
  backlog_.clear();
  emac_reset();
  phy_.fill(0);
  phy_[kBmcr] = 0x3100;  // auto-negotiation on, 100 Mb/s, full duplex
  phy_[kAnar] = 0x01E1;  // 10/100, half and full duplex
}

void Smap::reset_tx_fifo() {
  tx_control_ = 0;
  tx_write_ = tx_size_ = 0;
  tx_frames_ = 0;
  tx_index_ = 0;
}

void Smap::reset_rx_fifo() {
  rx_control_ = 0;
  rx_read_ = rx_size_ = rx_write_ = 0;
  rx_used_ = 0;
  rx_sizes_.clear();
  rx_index_ = 0;
}

void Smap::emac_reset() {
  emac_.fill(0);
}

bool Smap::receiver_enabled() const { return (emac_[kMode0] & kRxMacEnable) != 0; }

void Smap::trace(const char *what, std::uint32_t offset, std::uint32_t value, unsigned size) {
  if (trace_) std::fprintf(stderr, "[net] %s 0x%04X = 0x%X (%u)\n", what, offset, value, size);
}

void Smap::update_interrupt() {
  if (interrupt_status_ & interrupt_mask_) bus_.raise_interrupt();
}

void Smap::set_interrupt(std::uint16_t bits) {
  interrupt_status_ |= bits;
  update_interrupt();
}

// ---------------------------------------------------------------------------------------------------------------------
// DEV9

std::uint32_t Smap::dev9_read(std::uint32_t address) {
  const unsigned reg = (address - kDev9Base) >> 1;
  if (reg >= dev9_.size()) return 0;
  switch (address & ~1u) {
  case 0x1F80146Eu: return 0x0030; // expansion bay, revision 0
  case 0x1F801462u: return dev9_[reg] & ~1u; // bit 0 clear: a device is connected
  default: return dev9_[reg];
  }
}

void Smap::dev9_write(std::uint32_t address, std::uint32_t value) {
  const unsigned reg = (address - kDev9Base) >> 1;
  if (reg < dev9_.size()) dev9_[reg] = static_cast<std::uint16_t>(value);
}

// ---------------------------------------------------------------------------------------------------------------------
// MAC address EEPROM (93C46, 64 words of 16 bits): chip select = bit 7, clock = bit 6, data in = bit 5, data out = bit 4.

void Smap::eeprom_write(std::uint8_t value) {
  const bool select = value & 0x80, clock = value & 0x40, data = value & 0x20;
  const bool rising = clock && !(pio_data_ & 0x40);
  pio_data_ = value;
  if (!select) {
    eeprom_state_ = EepromState::Idle;
    eeprom_bits_ = 0;
    eeprom_out_ = false;
    return;
  }
  if (!rising) return;
  switch (eeprom_state_) {
  case EepromState::Idle:
    if (data) { // start bit
      eeprom_state_ = EepromState::Command;
      eeprom_bits_ = 0;
      eeprom_command_ = 0;
    }
    break;
  case EepromState::Command:
    eeprom_command_ = (eeprom_command_ << 1) | (data ? 1u : 0u);
    if (++eeprom_bits_ == 8) { // two opcode bits and six address bits
      if ((eeprom_command_ >> 6) == 2) { // READ
        eeprom_address_ = eeprom_command_ & 0x3F;
        eeprom_bit_ = 0;
        eeprom_out_ = false; // the dummy zero before the first data bit
        eeprom_state_ = EepromState::Read;
        ++counters_.eeprom_reads;
        if (trace_) std::fprintf(stderr, "[net] EEPROM read from word %u\n", eeprom_address_);
      } else {
        eeprom_state_ = EepromState::Idle; // writes and erases are ignored: the address is fixed
        eeprom_bits_ = 0;
      }
    }
    break;
  case EepromState::Read:
    eeprom_out_ = (eeprom_[eeprom_address_ & 0x3F] >> (15 - eeprom_bit_)) & 1u;
    if (++eeprom_bit_ == 16) { // sequential read continues with the next word
      eeprom_bit_ = 0;
      ++eeprom_address_;
    }
    break;
  }
}

// ---------------------------------------------------------------------------------------------------------------------
// PHY (a generic 10/100 transceiver with the link up at 100 Mb/s full duplex)

std::uint16_t Smap::phy_read(unsigned reg) {
  switch (reg) {
  case kBmcr: return phy_[kBmcr] & ~0x8200u; // reset and restart-negotiation complete at once
  case kBmsr: return 0x786D; // 10/100 half/full, negotiation complete, link up
  case kPhyId1: return 0x2000;
  case kPhyId2: return 0x5C90;
  case kAnlpar: return 0x45E1; // the partner offers the same and acknowledges
  case kAner: return 0x0001;
  case kPhySts: return 0x0015; // link, 100 Mb/s, full duplex, negotiation complete
  default: return phy_[reg & 31];
  }
}

void Smap::phy_write(unsigned reg, std::uint16_t value) { phy_[reg & 31] = value; }

void Smap::phy_operation() {
  std::uint32_t &sta = emac_[kStaCtrl];
  const unsigned reg = sta & 0x1F;
  ++counters_.phy_operations;
  if ((sta & kPhyCommandMask) == kPhyRead) {
    sta = (sta & 0x0000FFFFu) | (static_cast<std::uint32_t>(phy_read(reg)) << 16);
  } else if ((sta & kPhyCommandMask) == kPhyWrite) {
    phy_write(reg, static_cast<std::uint16_t>(sta >> 16));
  }
  sta |= kPhyOpComplete;
  if (trace_) std::fprintf(stderr, "[net] PHY reg %u sta 0x%08X\n", reg, sta);
}

// ---------------------------------------------------------------------------------------------------------------------
// EMAC3: 32-bit registers written and read as two 16-bit halves, high half at the lower address.

std::uint32_t Smap::emac_read(std::uint32_t reg) const {
  std::uint32_t value = emac_[reg];
  if (reg == kMode0) value |= kRxMacIdle | kTxMacIdle; // the MAC stops as soon as it is told to
  return value;
}

void Smap::emac_write(std::uint32_t reg, std::uint32_t value) {
  switch (reg) {
  case kMode0:
    if (value & kSoftReset) {
      emac_reset();
      return;
    }
    emac_[reg] = value;
    return;
  case kTxMode0:
    emac_[reg] = value & ~kGetNewPacket; // the packets go out before the write returns
    if (value & kGetNewPacket) transmit();
    return;
  case kEmacIntrStat:
    emac_[reg] &= ~value; // write one to clear
    return;
  case kStaCtrl:
    emac_[reg] = value & ~kPhyOpComplete;
    phy_operation();
    return;
  default:
    emac_[reg] = value;
  }
}

// ---------------------------------------------------------------------------------------------------------------------
// Frames

void Smap::transmit() {
  unsigned sent = 0;
  for (unsigned n = 0; n < kDescriptors; ++n) {
    Descriptor &bd = tx_bd_[tx_index_];
    if (!(bd.control & kBdTxReady)) break;
    const std::size_t length = bd.length;
    if (length >= kMinFrame && length <= kMaxFrame + 4) {
      std::array<std::uint8_t, kMaxFrame + 4> frame;
      const std::size_t start = static_cast<std::uint16_t>(bd.pointer - kTxFifoPointerBase);
      for (std::size_t i = 0; i < length; ++i) frame[i] = tx_fifo_[(start + i) & (kTxFifo - 1)];
      if (link_) link_->send(std::span<const std::uint8_t>(frame.data(), std::min(length, kMaxFrame)));
      ++counters_.tx_frames;
    }
    bd.control &= 0x7C00u & ~kBdTxReady; // done, no error bits
    tx_index_ = (tx_index_ + 1) % kDescriptors;
    if (tx_frames_) --tx_frames_;
    ++sent;
  }
  if (trace_) std::fprintf(stderr, "[net] transmit: %u frame(s), total %llu\n", sent, static_cast<unsigned long long>(counters_.tx_frames));
  if (sent) set_interrupt(kIntrTxDnv);
}

bool Smap::deliver(std::span<const std::uint8_t> frame) {
  // The driver sees the frame with its four check bytes and reads it in whole words.
  const std::size_t length = std::max<std::size_t>(frame.size(), 60) + 4;
  const std::size_t stored = (length + 3) & ~std::size_t{3};
  Descriptor &bd = rx_bd_[rx_index_];
  if (!(bd.control & kBdRxEmpty) || rx_used_ + stored > kRxFifo || rx_sizes_.size() >= 255) return false;
  for (std::size_t i = 0; i < stored; ++i)
    rx_fifo_[(rx_write_ + i) & (kRxFifo - 1)] = i < frame.size() ? frame[i] : 0;
  bd.pointer = static_cast<std::uint16_t>(kRxFifoPointerBase + rx_write_);
  bd.length = static_cast<std::uint16_t>(length);
  bd.control &= 0x7C00u & ~kBdRxEmpty; // filled, no error bits
  rx_write_ = static_cast<std::uint16_t>((rx_write_ + stored) & (kRxFifo - 1));
  rx_used_ += stored;
  rx_sizes_.push_back(static_cast<std::uint16_t>(stored));
  rx_index_ = (rx_index_ + 1) % kDescriptors;
  ++counters_.rx_frames;
  return true;
}

void Smap::poll() {
  if (!link_) return;
  link_->tick();
  std::vector<std::uint8_t> frame;
  for (unsigned n = 0; n < 32 && link_->receive(frame); ++n) {
    if (frame.size() < kMinFrame || frame.size() > kMaxFrame) continue;
    if (!receiver_enabled()) continue; // nobody is listening yet
    if (backlog_.size() >= 128) {
      ++counters_.rx_dropped;
      continue;
    }
    backlog_.push_back(std::move(frame));
    frame.clear();
  }
  if (!receiver_enabled()) {
    backlog_.clear();
    return;
  }
  bool delivered = false;
  while (!backlog_.empty() && deliver(backlog_.front())) {
    backlog_.pop_front();
    delivered = true;
  }
  if (delivered) set_interrupt(kIntrRxEnd);
}

bool Smap::dma(std::uint32_t address, std::uint32_t bytes, bool to_device) {
  if (to_device && (tx_control_ & kFifoDmaEnable)) {
    for (std::uint32_t i = 0; i < bytes; ++i) tx_fifo_[(tx_write_ + i) & (kTxFifo - 1)] = bus_.read8(address + i);
    tx_write_ = static_cast<std::uint16_t>((tx_write_ + bytes) & (kTxFifo - 1));
    tx_control_ &= ~kFifoDmaEnable;
    return true;
  }
  if (!to_device && (rx_control_ & kFifoDmaEnable)) {
    for (std::uint32_t i = 0; i < bytes; ++i) bus_.write8(address + i, rx_fifo_[(rx_read_ + i) & (kRxFifo - 1)]);
    rx_read_ = static_cast<std::uint16_t>((rx_read_ + bytes) & (kRxFifo - 1));
    rx_control_ &= ~kFifoDmaEnable;
    return true;
  }
  if (trace_) std::fprintf(stderr, "[net] DMA of %u bytes with no FIFO waiting (to_device %d)\n", bytes, to_device ? 1 : 0);
  return false;
}

// ---------------------------------------------------------------------------------------------------------------------
// Register access

std::uint16_t &Smap::descriptor_word(std::uint32_t offset) {
  auto &ring = offset < kRxBdBase ? tx_bd_ : rx_bd_;
  Descriptor &bd = ring[((offset - (offset < kRxBdBase ? kTxBdBase : kRxBdBase)) >> 3) % kDescriptors];
  switch ((offset >> 1) & 3) {
  case 0: return bd.control;
  case 1: return bd.reserved;
  case 2: return bd.length;
  default: return bd.pointer;
  }
}

std::uint32_t Smap::read(std::uint32_t offset, unsigned size) {
  if (offset >= kTxBdBase && offset < kBdEnd) return descriptor_word(offset);
  if (offset >= kEmacBase && offset < kEmacEnd) {
    const std::uint32_t value = emac_read((offset - kEmacBase) >> 2);
    if (size == 4) return (value >> 16) | (value << 16); // memory order: high half first
    return (offset & 2) ? (value & 0xFFFFu) : (value >> 16);
  }
  switch (offset) {
  case kRev: return 0x0011;
  case kRev1: return 0x0012;
  case kRev3: return kCapsSmap;
  case kRev8: return 0;
  case kIntrStat: return interrupt_status_;
  case kIntrMask: return interrupt_mask_;
  case kPioDir: return pio_direction_;
  case kPioData: return (pio_data_ & ~0x10u) | (eeprom_out_ ? 0x10u : 0u);
  case kBdMode: return bd_mode_;
  case kIntrClr: return interrupt_status_;
  case kIntrEnable: return interrupt_mask_;
  case kTxCtrl: return tx_control_;
  case kTxWrPtr: return tx_write_;
  case kTxSize: return tx_size_;
  case kTxFrameCnt: return tx_frames_;
  case kRxCtrl: return rx_control_;
  case kRxRdPtr: return rx_read_;
  case kRxSize: return rx_size_;
  case kRxFrameCnt: return static_cast<std::uint32_t>(rx_sizes_.size());
  case kRxData: {
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(rx_fifo_[(rx_read_ + i) & (kRxFifo - 1)]) << (8 * i);
    rx_read_ = static_cast<std::uint16_t>((rx_read_ + 4) & (kRxFifo - 1));
    return value;
  }
  default:
    if (offset < 0x100) return speed_[offset >> 1];
    trace("unhandled read", offset, 0, size);
    return 0;
  }
}

void Smap::write(std::uint32_t offset, std::uint32_t value, unsigned size) {
  if (offset >= kTxBdBase && offset < kBdEnd) {
    descriptor_word(offset) = static_cast<std::uint16_t>(value);
    return;
  }
  if (offset >= kEmacBase && offset < kEmacEnd) {
    const std::uint32_t reg = (offset - kEmacBase) >> 2;
    if (size == 4) {
      emac_write(reg, (value >> 16) | (value << 16));
    } else if (offset & 2) {
      emac_write(reg, (emac_[reg] & 0xFFFF0000u) | (value & 0xFFFFu)); // the low half completes the write
    } else {
      emac_[reg] = (emac_[reg] & 0x0000FFFFu) | (value << 16);
    }
    return;
  }
  switch (offset) {
  case kIntrMask:
  case kIntrEnable:
    interrupt_mask_ = static_cast<std::uint16_t>(value);
    update_interrupt(); // a driver re-arms by rewriting the mask while a cause is pending
    return;
  case kIntrClr:
    interrupt_status_ &= ~static_cast<std::uint16_t>(value & kIntrAll);
    return;
  case kPioDir: pio_direction_ = static_cast<std::uint8_t>(value); return;
  case kPioData: eeprom_write(static_cast<std::uint8_t>(value)); return;
  case kBdMode: bd_mode_ = static_cast<std::uint8_t>(value); return;
  case kTxCtrl:
    if (value & kFifoReset) reset_tx_fifo();
    else tx_control_ = static_cast<std::uint8_t>(value);
    return;
  case kTxWrPtr: tx_write_ = static_cast<std::uint16_t>(value & (kTxFifo - 1)); return;
  case kTxSize: tx_size_ = static_cast<std::uint16_t>(value); return;
  case kTxFrameInc: ++tx_frames_; return;
  case kTxData:
    for (unsigned i = 0; i < 4; ++i) tx_fifo_[(tx_write_ + i) & (kTxFifo - 1)] = static_cast<std::uint8_t>(value >> (8 * i));
    tx_write_ = static_cast<std::uint16_t>((tx_write_ + 4) & (kTxFifo - 1));
    return;
  case kRxCtrl:
    if (value & kFifoReset) reset_rx_fifo();
    else rx_control_ = static_cast<std::uint8_t>(value);
    return;
  case kRxRdPtr: rx_read_ = static_cast<std::uint16_t>(value & (kRxFifo - 1)); return;
  case kRxSize: rx_size_ = static_cast<std::uint16_t>(value); return;
  case kRxFrameDec:
    if (!rx_sizes_.empty()) {
      rx_used_ -= rx_sizes_.front();
      rx_sizes_.pop_front();
    }
    return;
  default:
    if (offset < 0x100) {
      speed_[offset >> 1] = static_cast<std::uint16_t>(value);
      return;
    }
    trace("unhandled write", offset, value, size);
  }
}

} // namespace gt4::net
