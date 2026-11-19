"""Net capsule (LAN over the internet, docs/ONLINE-LAN.md): derives iop/hw.cpp so the DEV9 registers
(0x1F801460..0x1F80147F), the SPEED/SMAP registers (0x10000000..0x1000FFFF) and IOP DMA channel 8 reach the Network
Adaptor model in native/net_smap.cpp. Without GT4_NET=1 the derived file answers exactly as its input does.

The input is the USB capsule's hw.cpp when --usb-capsule is given (so the wheel and the network adapter live in one
file), else the donor's iop/hw.cpp.

usage: prepare_net_runtime.py <fresh output folder> [--usb-capsule <folder>] [--assets-root <checkout>]
Writes hw.cpp and runtime.json (with the derived file's SHA-256, which CMake checks).
"""
import argparse
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DONOR_FILE = 'private/references/ps2-web-recomp/runtime/src/iop/hw.cpp'

INCLUDES_OLD = '#include <cstdio>\n'
INCLUDES_NEW = '#include "net_runtime.hpp"\n\n#include <cstdio>\n'

# The helpers go just before start_dma (inside the file's anonymous namespace), which is their first user.
HELPERS_OLD = 'void start_dma(unsigned ch) {\n'
HELPERS_NEW = '''// Net capsule: the Network Adaptor model when the player turned it on, else nothing changes.
std::uint64_t g_net_generation = 0;
bool g_net_timer = false;

gt4::net::Smap &net_adapter() {
    static const gt4::net::Bus bus{[](std::uint32_t a) { return ps2::iop::read8(a); },
                                   [](std::uint32_t a, std::uint8_t v) { ps2::iop::write8(a, v); },
                                   [] { raise_interrupt(gt4::net::kDev9InterruptLine); }};
    return gt4::net::machine_adapter(bus);
}

// The adapter looks for arrived frames once per millisecond of IOP time (36,864 cycles), from its first access on.
void net_tick(std::uint64_t generation) {
    if (generation != g_net_generation) return;
    net_adapter().poll();
    kernel::schedule(kernel::now() + 36864u, [generation] { net_tick(generation); });
}

void net_start_timer() {
    if (g_net_timer) return;
    g_net_timer = true;
    const std::uint64_t generation = ++g_net_generation;
    kernel::schedule(kernel::now() + 36864u, [generation] { net_tick(generation); });
}

bool net_address(std::uint32_t a) {
    return (a >= gt4::net::kDev9Base && a < gt4::net::kDev9End) ||
           (a >= gt4::net::kSpeedBase && a < gt4::net::kSpeedBase + gt4::net::kSpeedSize);
}

std::uint32_t net_read(std::uint32_t a, unsigned size) {
    net_start_timer();
    if (a < gt4::net::kDev9Base) return net_adapter().read(a - gt4::net::kSpeedBase, size);
    return net_adapter().dev9_read(a);
}

void net_write(std::uint32_t a, std::uint32_t v, unsigned size) {
    net_start_timer();
    if (a < gt4::net::kDev9Base) net_adapter().write(a - gt4::net::kSpeedBase, v, size);
    else net_adapter().dev9_write(a, v);
}

void net_reset() {
    g_net_timer = false;
    ++g_net_generation;
    if (gt4::net::adapter_active()) net_adapter().reset();
}

void start_dma(unsigned ch) {
'''

DMA_OLD = '    std::fprintf(stderr, "[iop-dma] channel %u start madr'
DMA_NEW = '''    if (ch == gt4::net::kDev9DmaChannel && gt4::net::adapter_active()) {
        net_adapter().dma(d.madr, words * 4u, (d.chcr & 1u) != 0);
        dma_done(ch);
        return;
    }
    std::fprintf(stderr, "[iop-dma] channel %u start madr'''

RESET_OLD = '    spu2::reset();\n    sio2::reset();\n'
RESET_NEW = '    spu2::reset();\n    sio2::reset();\n    net_reset();\n'

READ_OLD = '    if (a >= 0x1F900000u && a < 0x1F900800u) return spu2::read(a, size);\n'
READ_NEW = '    if (net_address(a) && gt4::net::adapter_active()) return net_read(a, size);\n' + READ_OLD

WRITE_OLD = '    if (a >= 0x1F900000u && a < 0x1F900800u) { spu2::write(a, v, size); return; }\n'
WRITE_NEW = '    if (net_address(a) && gt4::net::adapter_active()) { net_write(a, v, size); return; }\n' + WRITE_OLD

EDITS = ((INCLUDES_OLD, INCLUDES_NEW), (HELPERS_OLD, HELPERS_NEW), (DMA_OLD, DMA_NEW), (RESET_OLD, RESET_NEW),
         (READ_OLD, READ_NEW), (WRITE_OLD, WRITE_NEW))


def derive(source: str) -> str:
    if 'net_runtime.hpp' in source:
        raise SystemExit('the input iop/hw.cpp already has the net capsule edits')
    for old, _ in EDITS:
        if source.count(old) != 1:
            raise SystemExit(f'edit point not found exactly once in the input iop/hw.cpp: {old[:60]!r}')
    for old, new in EDITS:
        source = source.replace(old, new)
    return source


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('output', type=Path)
    parser.add_argument('--usb-capsule', type=Path, help='folder of the USB capsule (hw.cpp, runtime.json) to build on')
    parser.add_argument('--assets-root', type=Path, default=ROOT)
    args = parser.parse_args(argv)
    receipt = {'schema_version': 1, 'tool': 'scripts/prepare_net_runtime.py'}
    if args.usb_capsule:
        usb = json.loads((args.usb_capsule / 'runtime.json').read_text(encoding='utf-8'))
        source = (args.usb_capsule / 'hw.cpp').read_bytes()
        if hashlib.sha256(source).hexdigest() != usb['hw_sha256']:
            raise SystemExit('the USB capsule hw.cpp differs from its runtime.json pin')
        receipt.update(input='usb capsule', donor_file=usb['donor_file'], donor_sha256=usb['donor_sha256'],
                       usb_hw_sha256=usb['hw_sha256'])
    else:
        source = (args.assets_root / DONOR_FILE).read_bytes()
        receipt.update(input='donor', donor_file=DONOR_FILE, donor_sha256=hashlib.sha256(source).hexdigest())
    text = source.decode('utf-8')
    crlf = '\r\n' in text  # a Windows checkout has CRLF line endings; edits are made on LF
    derived_text = derive(text.replace('\r\n', '\n'))
    derived = (derived_text.replace('\n', '\r\n') if crlf else derived_text).encode('utf-8')
    args.output.mkdir(parents=True, exist_ok=False)
    (args.output / 'hw.cpp').write_bytes(derived)
    receipt['hw_sha256'] = hashlib.sha256(derived).hexdigest()
    (args.output / 'runtime.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8', newline='\n')
    print(json.dumps(receipt, indent=2))


if __name__ == '__main__':
    main()
