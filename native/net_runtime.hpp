#pragma once
// The one Network Adaptor of the machine, for the net capsule (scripts/prepare_net_runtime.py), which routes the
// IOP's DEV9/SPEED register accesses and DMA channel 8 here. Off unless the player asked for it (GT4_NET=1): then
// the derived iop/hw.cpp answers exactly as the donor's does.

#include "net_smap.hpp"

namespace gt4::net {

// True when GT4_NET=1 and the link to the bridge opened. Decided once per process.
bool adapter_active();

// The adapter, created on first use with the machine's bus. Only call when adapter_active().
Smap &machine_adapter(const Bus &bus);

} // namespace gt4::net
