# Online multiplayer: handoff notes for the next agent

2026-11-19. Everything learned while starting the LAN-over-internet work, so the next session does not rediscover it.
Design and usage are in [ONLINE-LAN](ONLINE-LAN.md); this file is the state of knowledge and the open work.
Labels: Confirmed / High confidence / Hypothesis / Unknown, as in `AGENTS.md`.

## What the person wants

- Online multiplayer for **Gran Turismo 4 Spec II 1.10** (SCUS-97436 lineage) running in GT4Recomp.
- **Their own standalone server, or player-hosted lobbies.** They do not want to depend on the existing community
  server. They asked for **Cloudflare Tunnel** where possible and will put the server on a **VPS** later.
- They use the portable release build (1.3.0-rc16 Beta) on Windows; they are a player, not a developer. Give them
  links and single steps.

## Decisions taken

1. **Transport = the game's own LAN battle mode**, not Sony's 2006 online service. Reason: LAN mode needs no game
   server (one console hosts), so "own server" reduces to carrying Ethernet frames between players. Reviving the
   online protocol would mean reverse-engineering a private server protocol.
2. **Hardware-level adapter (DEV9/SPEED/SMAP), not socket-level HLE.** The game's IOP modules (DEV9, SMAP, INET,
   NETCNF, INETCTL) are already loaded and interpreted; giving them a device keeps the game's IP stack untouched and
   needs no decompilation of the network classes.
3. **WebSocket transport** so one HTTP port works through Cloudflare Tunnel and proxies. Cost: TCP head-of-line
   blocking. A direct UDP path is a later optimisation.
4. **The lobby is an Ethernet switch with DHCP**, so no per-player network setup is needed.
5. **`net/` is its own Cargo workspace**, so the launcher's `Cargo.lock` and release checks are untouched.

## Facts about the recomp (Confirmed by reading `main` at `b0008e8`)

- The runtime is the frozen MIT donor `snuri00/ps2-web-recomp` at `1b4808793a12c45125262fd2d5f50a9ec9f7c020`, expected
  at `private/references/ps2-web-recomp`. Behaviour changes are **capsules**: a script derives a donor file with exact
  text edits and writes a `runtime.json` hash pin; CMake swaps the file in (`GT4_*_RUNTIME_SOURCE`).
- All IOP hardware access that is not RAM goes through `ps2::iop::hw::read/write(phys, size)` in the donor's
  `runtime/src/iop/hw.cpp`. Unknown registers are stored in a map and logged once (`[iop-io]`). DMA channels other
  than 4, 7, 11, 12 are completed at once and logged "not emulated". IOP interrupts: `raise_interrupt(line)`.
  Timers: `kernel::schedule(kernel::now() + cycles, fn)`; 36,864 cycles = 1 ms.
- **The USB capsule (`scripts/prepare_usb_runtime.py`, TASK-018) is the precedent** and also replaces `hw.cpp`, so the
  net capsule is derived from the USB capsule's output (`--usb-capsule`) and takes the `hw.cpp` slot in CMake.
- `docs/IOP-MAP.md`: both games load INET, NETCNF, INETCTL, DEV9, SMAP, RTINETB; Spec II adds PPP, PPPOE. With no
  device, the DEV9 open fails cleanly. DNAS modules under `\NET` are never loaded offline.
- Spec II's Options screen lists "LAN Settings" under Arcade Mode (`docs/SPEC2-PLAN.md`), so LAN code is present.
- **The game executable cannot be built from a clean checkout.** It links a pinned private EE library
  (`ps2game.lib`) generated from the disc with private entry seeds, and the release builds use several private
  capsule folders (`docs/BUILD.md`, Spec II S4). Only the upstream maintainer (moesuito) has that tree today.
  **This is the main blocker for an in-game test**: see "Getting a test build" below.
- Repository rules that matter here: no GPL code in the game executable and PCSX2 is reference only; code and docs in
  en-US; every behaviour needs CTest/Python controls; never weaken checks; switches default off; claims are labelled.

## Facts about the hardware model (sources: homebrew headers and data sheets; in-game behaviour Unknown)

- DEV9 registers `0x1F801460..0x1F80147F` (16-bit). Revision at `0x1F80146E`: high nibble 3 = expansion bay.
  Bit 0 of `0x1F801462` clear = device connected.
- SPEED at IOP physical `0x10000000`: revision words at `0x00/0x02/0x04/0x0E` (`0x04` bit 0 = SMAP present),
  interrupt status `0x28`, mask `0x2A`, EEPROM pins on `0x2E` (CS bit 7, clock bit 6, data in bit 5, data out bit 4).
  IOP interrupt line 13, IOP DMA channel 8 (blocks of 0x20 words = 128 bytes).
- SMAP: clear interrupts at `0x128`; TX FIFO control/pointer/size/count at `0x1000..0x1010`, data `0x1100`;
  RX FIFO at `0x1030..0x1040`, data `0x1200`; EMAC3 at `0x2000` (32-bit registers as two 16-bit halves, high half
  first); descriptors TX `0x3000`, RX `0x3200` (64 entries of control, reserved, length, pointer; bit 15 = ready/empty;
  TX pointers are based at `0x1000`, RX at `0x4000`). Interrupt bits: EMAC3 6, RXEND 5, TXEND 4, RXDNV 3, TXDNV 2.
- Guesses made in the model, first suspects if the game's driver refuses it [Hypothesis]: SPEED revision values
  (`0x0011`, `0x0012`); PHY identity (`0x2000`/`0x5C90`) and that every PHY address answers; received length includes
  four check bytes; transmit completes inside the "get new packet" write and raises TXDNV; the descriptor index
  restarts at 0 on a FIFO reset; DEV9 power registers are plain storage.

## What exists on the branch `task/online-lan-tunnel`

| Path | What | Checked |
|---|---|---|
| `native/net_smap.*` | adapter model | `net_smap_fixture` (g++ 20, ASan + UBSan) |
| `native/net_link.*`, `native/net_runtime.*` | UDP link, settings, the one instance | same fixture (loopback sockets) |
| `scripts/prepare_net_runtime.py`, CMake `GT4_NET_RUNTIME_SOURCE`, lib `gt4_net`, CTest `native_net_smap` | wiring | `tests/python/test_net_runtime.py`; derived `hw.cpp` passes `g++ -fsyntax-only` against the donor headers |
| `net/` | `gt4net` server, switch, bridge, CLI | `cargo test` (7 unit, 3 end-to-end), clippy clean |
| `docs/ONLINE-LAN.md` | design, switches, build, usage, limits, test plan | |

**Not done, in order of need:**

1. **Never compiled with clang-cl / MSVC headers** (Winsock path in `net_link.cpp`, `/W4`). Expect small fixes.
2. **Never run in the game.** First run: `GT4_NET=1 GT4_NET_TRACE=1` and read `[net]` and `[iop-io]`/`[iop-dma]`
   lines; fix the model until Sony's SMAP module initialises (EEPROM read, PHY operations, then transmits).
3. Check that Spec II's LAN battle starts at all (try it in PCSX2 with two instances first: cheap, no build needed).
4. Launcher: Host and Join screens (Tauri UI under `launcher/crates/gt4l-tauri`), starting the bridge and setting
   `GT4_NET` for the game. Note the launcher scrubs inherited `GT4_*` variables, so the launcher must pass them itself.
5. Machine-state stream for the adapter; a UDP fast path; a lobby browser in the launcher against `GET /lobbies`.

## Getting a test build (open problem)

Options, best first:
- **Ask upstream (moesuito) to build the branch** or to accept the PR behind its default-off switch and ship it in a
  Beta rc. The capsule recipe is three commands (ONLINE-LAN "Build"). An issue or PR on `moesuito/gt4-staticrecomp`
  needs the person's go-ahead first (it is a message outside the project).
- Reproduce the private build tree on the person's PC (VS Build Tools, LLVM clang-cl, the donor checkout, the EE
  producer and Spec II seeds from `scripts/spec2_*.py`). Documented only in pieces in `docs/BUILD.md`; expect days and
  a library that may not match the pins. Not attempted.
- There is no way to add the adapter to the shipped executable without rebuilding it.

## Community state (web research 2026-11-19, not verified by running anything)

- The GT4 Online Public Beta still plays online in PCSX2 through a community server (PSRewired; players set a custom
  primary DNS in the game's network settings; status page `psrewired.com/servers/10782`; GTPlanet thread "GT4 Online
  In 2026?"). Reported working: profile, friends, chat, races; hosting needs UDP 6000 forwarded; it needs a
  DNAS-disabled image. Its server code was not found public.
- "GT4 Online Public Beta Enhanced" (GTPlanet) lists 2-Player Battle and Multi-LAN Race as enabled on the same base
  as Spec II, which is weak evidence that LAN code in this lineage works.
- GT4 holds six cars per race; that is the lobby size.

## Working environment notes

- The work was done in a Linux cloud container with no disc image and no Windows toolchain: host-side tests only.
- The person plays the portable 1.3.0-rc16 Beta build with Spec II 1.10 on Windows (an older Stable install refuses
  Spec II). Local paths are in the project's private notes, not here.
- Fork: `github.com/johnjoe123/gt4-staticrecomp` (the person's). Upstream: `moesuito/gt4-staticrecomp`.
