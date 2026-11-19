//! Two bridges join one lobby on a real server over loopback; frames from one "game" socket reach the other, the
//! switch's DHCP answers, and the server's refusals arrive as readable errors.

use gt4net::bridge::{self, JoinOptions};
use gt4net::server::Event;
use std::time::Duration;
use tokio::net::UdpSocket;
use tokio::sync::mpsc;

async fn start_server() -> String {
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let address = listener.local_addr().unwrap().to_string();
    tokio::spawn(gt4net::server::serve(listener));
    address
}

async fn free_udp_port() -> u16 {
    UdpSocket::bind("127.0.0.1:0").await.unwrap().local_addr().unwrap().port()
}

fn options(server: &str, room: Option<&str>, create: bool, name: &str, link_port: u16) -> JoinOptions {
    JoinOptions {
        server: server.to_owned(),
        room: room.map(str::to_owned),
        create,
        title: None,
        name: name.to_owned(),
        password: None,
        link_port,
    }
}

/// Starts a bridge and returns its event stream.
fn start_bridge(options: JoinOptions) -> mpsc::UnboundedReceiver<Event> {
    let (tx, rx) = mpsc::unbounded_channel();
    tokio::spawn(async move {
        let _ = bridge::run(options, move |event| {
            let _ = tx.send(event.clone());
        })
        .await;
    });
    rx
}

async fn next<T>(rx: &mut mpsc::UnboundedReceiver<T>) -> T {
    tokio::time::timeout(Duration::from_secs(5), rx.recv()).await.expect("event in time").expect("bridge alive")
}

async fn receive(socket: &UdpSocket) -> Vec<u8> {
    let mut buffer = [0u8; 2048];
    let len = tokio::time::timeout(Duration::from_secs(5), socket.recv(&mut buffer)).await.expect("frame in time").unwrap();
    buffer[..len].to_vec()
}

fn frame(dst: [u8; 6], src: [u8; 6], fill: u8) -> Vec<u8> {
    let mut f = Vec::new();
    f.extend_from_slice(&dst);
    f.extend_from_slice(&src);
    f.extend_from_slice(&[0x08, 0x00]);
    f.extend_from_slice(&[fill; 100]);
    f
}

#[tokio::test(flavor = "multi_thread")]
async fn two_games_exchange_frames_through_a_lobby() {
    let server = start_server().await;
    let (port_a, port_b) = (free_udp_port().await, free_udp_port().await);

    let mut events_a = start_bridge(options(&server, Some("RACE1"), true, "Ana", port_a));
    let Event::Welcome { room, slot, ip } = next(&mut events_a).await else { panic!("welcome first") };
    assert_eq!((room.as_str(), slot, ip.as_str()), ("RACE1", 0, "192.168.47.11"));

    let lobbies = bridge::list(&server).await.unwrap();
    assert_eq!(lobbies.len(), 1);
    assert_eq!((lobbies[0].id.as_str(), lobbies[0].players, lobbies[0].title.as_str()), ("RACE1", 1, "Ana's lobby"));

    let mut events_b = start_bridge(options(&server, Some("race1"), false, "Bruno", port_b));
    let Event::Welcome { slot, .. } = next(&mut events_b).await else { panic!("welcome first") };
    assert_eq!(slot, 1);

    let game_a = UdpSocket::bind("127.0.0.1:0").await.unwrap();
    let game_b = UdpSocket::bind("127.0.0.1:0").await.unwrap();
    game_a.connect(("127.0.0.1", port_a)).await.unwrap();
    game_b.connect(("127.0.0.1", port_b)).await.unwrap();
    // The hello tells each bridge where its game listens.
    game_a.send(&[0]).await.unwrap();
    game_b.send(&[0]).await.unwrap();
    tokio::time::sleep(Duration::from_millis(100)).await;

    let (mac_a, mac_b) = ([2, 0, 0, 0, 0, 0xA], [2, 0, 0, 0, 0, 0xB]);
    let broadcast = frame([0xFF; 6], mac_a, 1);
    game_a.send(&broadcast).await.unwrap();
    assert_eq!(receive(&game_b).await, broadcast);
    let unicast = frame(mac_a, mac_b, 2);
    game_b.send(&unicast).await.unwrap();
    assert_eq!(receive(&game_a).await, unicast);

    // A third player cannot join a lobby that does not exist; the refusal is readable.
    let error = bridge::run(options(&server, Some("NOPE"), false, "Caio", free_udp_port().await), |_| {}).await.unwrap_err();
    assert!(error.contains("no such lobby"), "{error}");
}

#[tokio::test(flavor = "multi_thread")]
async fn lobby_fills_at_six_and_closes_when_empty() {
    let server = start_server().await;
    let mut bridges = Vec::new();
    for player in 0..6 {
        let mut events = start_bridge(options(&server, Some("FULL"), player == 0, &format!("P{player}"), free_udp_port().await));
        assert!(matches!(next(&mut events).await, Event::Welcome { slot, .. } if slot == player));
        bridges.push(events);
    }
    let error = bridge::run(options(&server, Some("FULL"), false, "Late", free_udp_port().await), |_| {}).await.unwrap_err();
    assert!(error.contains("the lobby is full"), "{error}");
    assert_eq!(bridge::list(&server).await.unwrap()[0].players, 6);
}

#[tokio::test(flavor = "multi_thread")]
async fn password_is_checked() {
    let server = start_server().await;
    let mut host = options(&server, Some("LOCK"), true, "Ana", free_udp_port().await);
    host.password = Some("drift".into());
    let mut events = start_bridge(host);
    next(&mut events).await;
    assert!(bridge::list(&server).await.unwrap()[0].locked);
    let error = bridge::run(options(&server, Some("LOCK"), false, "Eve", free_udp_port().await), |_| {}).await.unwrap_err();
    assert!(error.contains("wrong lobby password"), "{error}");
    let mut guest = options(&server, Some("LOCK"), false, "Bruno", free_udp_port().await);
    guest.password = Some("drift".into());
    let mut events = start_bridge(guest);
    assert!(matches!(next(&mut events).await, Event::Welcome { slot: 1, .. }));
}
