# LAN battle over the internet

2026-11-19. Status: **code and host-side tests only; not yet run in the game.** Nothing here is on unless the player
asks for it (`GT4_NET=1`). Background: [ONLINE-NOTES](ONLINE-NOTES.md), [IOP-MAP](IOP-MAP.md).

## Goal

Players host their own lobbies and race each other with the game's own LAN battle mode (up to six consoles), over the
internet, with no dependency on any third-party game server. Sony's 2006 online service is a different protocol and
is out of scope here.

## Path of one Ethernet frame

```
game (EE) -> INET/SMAP IRX modules (IOP, interpreted, unchanged)
          -> Network Adaptor model            native/net_smap.cpp    (DEV9, SPEED, SMAP registers)
          -> UDP datagram on 127.0.0.1        native/net_link.cpp
          -> gt4net bridge                    net/src/bridge.rs
          -> one WebSocket                    (plain, or TLS through Cloudflare Tunnel / a reverse proxy)
          -> gt4net lobby server              net/src/server.rs, net/src/switch.rs
          -> the other members' bridges -> their games
```

The game believes it is on a home network. The lobby is that network: a learning Ethernet switch for six members that
also answers DHCP (each seat gets `192.168.47.11` .. `192.168.47.16`, router `192.168.47.1`) and ARP for the router.
Lobbies are isolated from each other and from the server's own network: frames are only ever switched between the
members of one lobby.

## Parts

| Part | Where | Checked by |
|---|---|---|
| Network Adaptor model (DEV9 probe, MAC EEPROM, PHY, EMAC3, FIFOs, descriptor rings, PIO and DMA, interrupts) | `native/net_smap.*` | CTest `native_net_smap` |
| UDP link to the bridge, settings from the environment | `native/net_link.*`, `native/net_runtime.*` | CTest `native_net_smap` |
| Net capsule: routes the IOP's DEV9/SPEED accesses and DMA channel 8 to the model | `scripts/prepare_net_runtime.py`, `GT4_NET_RUNTIME_SOURCE` | `tests/python/test_net_runtime.py` |
| Lobby server, switch, bridge, command line | `net/` (Rust, its own workspace) | `cargo test` in `net/` (unit + two bridges through a real server) |

Sources for the adapter model [Confirmed as sources, behaviour in the game Unknown]: the register layout published in
the open-source PS2 homebrew headers (`speedregs.h`, `smapregs.h`), the 93C46 EEPROM and IEEE 802.3 clause 22 (MII)
data sheets. **No PCSX2 source was read**, and none is linked.

## Switches

| Variable | Meaning |
|---|---|
| `GT4_NET=1` | Turn the adapter on. Off (the default): the derived `hw.cpp` answers exactly as its input does. |
| `GT4_NET_LINK_PORT` | UDP port of the bridge on 127.0.0.1 (default 47624). Two games on one PC need two ports. |
| `GT4_NET_MAC` | Override the adapter address (`aa:bb:cc:dd:ee:ff`). Default: `02:47:54:xx:xx:xx` from the PC, user and port. |
| `GT4_NET_TRACE=1` | Log EEPROM reads, PHY operations, transmits and any register the model does not know to stderr. |

## Build

The adapter enters the game through the net capsule, like the USB capsule (both own `iop/hw.cpp`, so the net capsule
is derived from the USB one):

```
python scripts/prepare_usb_runtime.py private/sliceNNN-usb-runtime
python scripts/prepare_net_runtime.py private/sliceNNN-net-runtime --usb-capsule private/sliceNNN-usb-runtime
cmake ... -DGT4_USB_RUNTIME_SOURCE=private/sliceNNN-usb-runtime -DGT4_NET_RUNTIME_SOURCE=private/sliceNNN-net-runtime
```

The game executable needs the pinned private EE library like every other build (`docs/BUILD.md`); a clean checkout
cannot produce it. `gt4net` builds anywhere: `cargo build --release` in `net/`.

## Running a lobby

```
gt4net server                       # a public lobby server, port 47800 (VPS, or behind Cloudflare Tunnel)
gt4net host --cloudflare            # host on this PC; prints an https address friends can join (needs cloudflared)
gt4net host                         # host on this PC; friends join by IP (forward TCP 47800) or on the same network
gt4net create https://lobby.example --title "Friday night"
gt4net list   https://lobby.example
gt4net join   https://lobby.example --room AB12C
```

Then each player starts the game with `GT4_NET=1` and enters the game's LAN mode. Network settings in the game:
automatic (DHCP), or manual with the seat address the bridge prints.

Everything is HTTP and WebSocket on one port, so Cloudflare Tunnel works without extra settings:
`cloudflared tunnel --url http://127.0.0.1:47800` for a throwaway address, or a named tunnel with a hostname for a
permanent one. Server endpoints: `GET /lobbies` (JSON), `GET /ws` (join or create), `GET /` (a text line).
`net/deploy/gt4net.service` is a systemd unit for a VPS.

## Limits and unknowns

- **Never run in the game.** [Unknown] whether Sony's DEV9/SMAP modules accept the model as written; the first runs
  will need `GT4_NET_TRACE=1` and fixes. [Unknown] whether Spec II 1.10's LAN battle still works at all (its Options
  screen lists LAN settings). [Unknown] how the LAN mode tolerates internet latency; it was written for a local network.
- Traffic rides TCP (WebSocket). A lost packet delays the ones behind it; a direct UDP path between players is a
  later improvement once the mode is proven.
- A lobby password is a convenience, sent in the join request; use an `https` address when it matters. The server
  keeps no accounts and stores nothing.
- The adapter's state is not part of the machine-state stream (`ps2::iop::hw::state`).
- No launcher interface yet: the bridge is a command-line program.

## Test plan (owner PCs)

1. Baseline: build with the capsule, run **without** `GT4_NET`: the game must behave as before (same capture hashes).
2. `GT4_NET=1 GT4_NET_TRACE=1`, bridge running (`gt4net host`): boot to the menus. Expect `[net] network adapter on`,
   an EEPROM read and PHY operations in the log, and no halt.
3. Open the game's network settings and LAN mode; expect DHCP to give `192.168.47.11`.
4. Second PC (or second instance with `GT4_NET_LINK_PORT=47625` and `gt4net join 127.0.0.1 --link-port 47625`):
   both games see each other in the LAN lobby; start a race.
5. The same through `--cloudflare` and through a VPS.
