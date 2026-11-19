//! The bridge: carries Ethernet frames between the game on this PC (UDP on 127.0.0.1) and a lobby (WebSocket).
//!
//! The game sends one datagram per frame to the link port. Datagrams shorter than an Ethernet header are "hello"
//! messages: the game sends one when it starts and about once a second, so the bridge learns where to deliver
//! frames even while the game is only listening.

use crate::server::Event;
use futures_util::{SinkExt, StreamExt};
use std::net::SocketAddr;
use tokio::net::UdpSocket;
use tokio_tungstenite::tungstenite::Message;

pub struct JoinOptions {
    /// Server address as the player types it: `host:port`, `http(s)://...` or `ws(s)://...`.
    pub server: String,
    pub room: Option<String>,
    pub create: bool,
    pub title: Option<String>,
    pub name: String,
    pub password: Option<String>,
    pub link_port: u16,
}

fn encode(text: &str) -> String {
    let mut out = String::new();
    for byte in text.bytes() {
        if byte.is_ascii_alphanumeric() || b"-_.~".contains(&byte) {
            out.push(byte as char);
        } else {
            out.push_str(&format!("%{byte:02X}"));
        }
    }
    out
}

/// Turns what the player typed into the WebSocket base URL (no path). A bare `host:port` is a direct connection
/// (plain WebSocket); `https://` (a Cloudflare Tunnel address, for example) becomes `wss://`.
pub fn websocket_base(server: &str) -> Result<String, String> {
    let server = server.trim().trim_end_matches('/');
    if server.is_empty() || server.contains(char::is_whitespace) {
        return Err("the server address is empty or has spaces".into());
    }
    let (scheme, rest) = match server.split_once("://") {
        Some(("https" | "wss", rest)) => ("wss", rest),
        Some(("http" | "ws", rest)) => ("ws", rest),
        Some((other, _)) => return Err(format!("unknown address type '{other}://'")),
        None => ("ws", server),
    };
    let host = rest.split('/').next().unwrap_or_default();
    if host.is_empty() {
        return Err("the server address has no host name".into());
    }
    let needs_port = scheme == "ws" && !host.rsplit_once(':').is_some_and(|(_, p)| p.parse::<u16>().is_ok());
    Ok(if needs_port { format!("{scheme}://{host}:{}", crate::DEFAULT_SERVER_PORT) } else { format!("{scheme}://{host}") })
}

pub fn join_url(options: &JoinOptions) -> Result<String, String> {
    let mut url = format!("{}/ws?name={}", websocket_base(&options.server)?, encode(&options.name));
    if let Some(room) = &options.room {
        url.push_str(&format!("&room={}", encode(room)));
    }
    if options.create {
        url.push_str("&create=1");
    }
    if let Some(title) = &options.title {
        url.push_str(&format!("&title={}", encode(title)));
    }
    if let Some(password) = &options.password {
        url.push_str(&format!("&password={}", encode(password)));
    }
    Ok(url)
}

/// Asks a server for its open lobbies.
pub async fn list(server: &str) -> Result<Vec<crate::server::LobbyInfo>, String> {
    let url = format!("{}/lobbies/ws", websocket_base(server)?);
    let (mut socket, _) = tokio_tungstenite::connect_async(&url).await.map_err(|e| format!("cannot reach the server: {e}"))?;
    while let Some(message) = socket.next().await {
        if let Ok(Message::Text(text)) = message {
            return serde_json::from_str(&text).map_err(|e| format!("unexpected answer from the server: {e}"));
        }
    }
    Err("the server sent no lobby list".into())
}

/// Joins the lobby and relays until the connection ends. `on_event` sees every server event (welcome, roster).
pub async fn run(options: JoinOptions, mut on_event: impl FnMut(&Event)) -> Result<(), String> {
    let url = join_url(&options)?;
    let link = UdpSocket::bind(("127.0.0.1", options.link_port))
        .await
        .map_err(|e| format!("cannot open the game link on 127.0.0.1:{} ({e}); is another bridge running?", options.link_port))?;
    let (socket, _) = tokio_tungstenite::connect_async(&url).await.map_err(|e| match e {
        tokio_tungstenite::tungstenite::Error::Http(response) => {
            let body = response.body().as_ref().map(|b| String::from_utf8_lossy(b).into_owned()).unwrap_or_default();
            format!("the server refused: {} {}", response.status(), body.trim())
        }
        other => format!("cannot reach the server: {other}"),
    })?;
    let (mut sink, mut stream) = socket.split();
    let mut game: Option<SocketAddr> = None;
    let mut datagram = [0u8; 2048];
    loop {
        tokio::select! {
            received = link.recv_from(&mut datagram) => {
                let (len, from) = received.map_err(|e| format!("game link failed: {e}"))?;
                game = Some(from);
                if len >= crate::switch::MIN_FRAME && len <= crate::switch::MAX_FRAME {
                    sink.send(Message::Binary(datagram[..len].to_vec().into())).await.map_err(|e| format!("lost the server: {e}"))?;
                }
            }
            message = stream.next() => match message {
                Some(Ok(Message::Binary(frame))) => {
                    if let Some(game) = game {
                        let _ = link.send_to(&frame, game).await;
                    }
                }
                Some(Ok(Message::Text(text))) => {
                    if let Ok(event) = serde_json::from_str::<Event>(&text) {
                        on_event(&event);
                    }
                }
                Some(Ok(Message::Ping(data))) => { let _ = sink.send(Message::Pong(data)).await; }
                Some(Ok(Message::Close(_))) | None => return Err("the server closed the connection".into()),
                Some(Ok(_)) => {}
                Some(Err(e)) => return Err(format!("lost the server: {e}")),
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn addresses() {
        assert_eq!(websocket_base("203.0.113.9").unwrap(), "ws://203.0.113.9:47800");
        assert_eq!(websocket_base("203.0.113.9:5000/").unwrap(), "ws://203.0.113.9:5000");
        assert_eq!(websocket_base("http://lobby.example").unwrap(), "ws://lobby.example:47800");
        assert_eq!(websocket_base("https://x.trycloudflare.com/").unwrap(), "wss://x.trycloudflare.com");
        assert_eq!(websocket_base("wss://lobby.example:8443").unwrap(), "wss://lobby.example:8443");
        assert!(websocket_base("ftp://x").is_err());
        assert!(websocket_base("  ").is_err());
    }

    #[test]
    fn join_url_escapes() {
        let url = join_url(&JoinOptions {
            server: "https://x.example".into(),
            room: Some("AB12".into()),
            create: true,
            title: Some("Friday & friends".into()),
            name: "Ana Maria".into(),
            password: Some("p=1".into()),
            link_port: 0,
        })
        .unwrap();
        assert_eq!(url, "wss://x.example/ws?name=Ana%20Maria&room=AB12&create=1&title=Friday%20%26%20friends&password=p%3D1");
    }
}
