//! GT4Recomp LAN-over-internet.
//!
//! The game's own LAN battle mode runs unchanged on a virtual network adapter (native/net_smap.cpp). That adapter
//! hands every Ethernet frame to the bridge over a UDP socket on this PC; the bridge carries the frames to a lobby
//! on a relay server over one WebSocket (so it passes through Cloudflare Tunnel and ordinary HTTPS proxies), and the
//! server switches them between the members of the lobby like an Ethernet switch.

pub mod bridge;
pub mod server;
pub mod switch;

/// Default TCP port of the lobby and relay server.
pub const DEFAULT_SERVER_PORT: u16 = 47800;
/// Default UDP port on 127.0.0.1 where the bridge waits for the game's frames.
pub const DEFAULT_LINK_PORT: u16 = 47624;
/// GT4 holds six cars in a race, so a lobby holds six players.
pub const MAX_PLAYERS: usize = 6;
