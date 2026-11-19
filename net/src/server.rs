//! The lobby and relay server. One HTTP port:
//!
//! - `GET /`              a line of text (so a browser shows the server is up)
//! - `GET /lobbies`       the open lobbies as JSON
//! - `GET /lobbies/ws`    the same list as one WebSocket text message
//! - `GET /ws`            WebSocket: join or create a lobby (query: `room`, `create`, `title`, `name`, `password`)
//!
//! Inside a lobby, binary messages are Ethernet frames and text messages are JSON events from the server. Everything
//! is plain HTTP and WebSocket so the server can sit behind Cloudflare Tunnel, a reverse proxy, or nothing at all.

use crate::switch::{slot_ip, Switch, MAX_FRAME};
use crate::MAX_PLAYERS;
use axum::extract::ws::{Message, WebSocket, WebSocketUpgrade};
use axum::extract::{Query, State};
use axum::http::StatusCode;
use axum::response::{IntoResponse, Response};
use axum::routing::get;
use axum::{Json, Router};
use futures_util::{SinkExt, StreamExt};
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::sync::{Arc, Mutex};
use std::time::Duration;
use tokio::sync::mpsc;

const MAX_ROOMS: usize = 256;
/// Frames queued for one slow member before the server drops frames for it (a race sends a few dozen per second).
const QUEUE: usize = 512;

struct Member {
    name: String,
    tx: mpsc::Sender<Message>,
}

struct Room {
    title: String,
    password: Option<String>,
    switch: Switch,
    members: [Option<Member>; MAX_PLAYERS],
}

impl Room {
    fn names(&self) -> Vec<String> {
        self.members.iter().flatten().map(|m| m.name.clone()).collect()
    }

    fn broadcast_roster(&self, room: &str) {
        let players: Vec<_> = self
            .members
            .iter()
            .enumerate()
            .filter_map(|(slot, m)| m.as_ref().map(|m| Player { slot, name: m.name.clone(), ip: ip_text(slot) }))
            .collect();
        let text = serde_json::to_string(&Event::Roster { room: room.to_owned(), title: self.title.clone(), players })
            .expect("roster serializes");
        for member in self.members.iter().flatten() {
            let _ = member.tx.try_send(Message::Text(text.clone().into()));
        }
    }
}

#[derive(Default)]
pub struct Lobbies {
    rooms: Mutex<HashMap<String, Room>>,
}

pub type Shared = Arc<Lobbies>;

#[derive(Serialize, Deserialize, Debug, Clone, PartialEq)]
pub struct LobbyInfo {
    pub id: String,
    pub title: String,
    pub players: usize,
    pub max: usize,
    pub locked: bool,
    pub names: Vec<String>,
}

#[derive(Serialize, Deserialize, Debug, Clone, PartialEq)]
pub struct Player {
    pub slot: usize,
    pub name: String,
    pub ip: String,
}

#[derive(Serialize, Deserialize, Debug, Clone, PartialEq)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum Event {
    /// First message after joining: which lobby, which seat, and the address the game gets from DHCP.
    Welcome { room: String, slot: usize, ip: String },
    Roster { room: String, title: String, players: Vec<Player> },
}

#[derive(Deserialize, Default)]
pub struct JoinQuery {
    room: Option<String>,
    create: Option<String>,
    title: Option<String>,
    name: Option<String>,
    password: Option<String>,
}

fn ip_text(slot: usize) -> String {
    let ip = slot_ip(slot);
    format!("{}.{}.{}.{}", ip[0], ip[1], ip[2], ip[3])
}

/// Keeps printable text, trims it and caps its length, so names and titles are safe to list.
fn clean(text: Option<String>, fallback: &str, max: usize) -> String {
    let cleaned: String = text.unwrap_or_default().chars().filter(|c| !c.is_control()).take(max).collect();
    let cleaned = cleaned.trim();
    if cleaned.is_empty() { fallback.to_owned() } else { cleaned.to_owned() }
}

fn new_room_id(existing: &HashMap<String, Room>) -> String {
    use std::hash::{BuildHasher, Hasher};
    const LETTERS: &[u8] = b"ABCDEFGHJKLMNPQRSTUVWXYZ23456789"; // no look-alikes
    loop {
        let mut bits = std::collections::hash_map::RandomState::new().build_hasher().finish();
        let id: String = (0..5)
            .map(|_| {
                let c = LETTERS[(bits & 31) as usize] as char;
                bits >>= 5;
                c
            })
            .collect();
        if !existing.contains_key(&id) {
            return id;
        }
    }
}

pub fn router(state: Shared) -> Router {
    Router::new()
        .route("/", get(|| async { "GT4Recomp lobby server. Lobbies: /lobbies\n" }))
        .route("/lobbies", get(lobbies))
        .route("/lobbies/ws", get(lobbies_socket))
        .route("/ws", get(join))
        .with_state(state)
}

pub fn list(state: &Lobbies) -> Vec<LobbyInfo> {
    let rooms = state.rooms.lock().unwrap();
    let mut out: Vec<_> = rooms
        .iter()
        .map(|(id, room)| LobbyInfo {
            id: id.clone(),
            title: room.title.clone(),
            players: room.switch.count(),
            max: MAX_PLAYERS,
            locked: room.password.is_some(),
            names: room.names(),
        })
        .collect();
    out.sort_by(|a, b| a.id.cmp(&b.id));
    out
}

async fn lobbies(State(state): State<Shared>) -> Json<Vec<LobbyInfo>> {
    Json(list(&state))
}

/// The same list over a WebSocket (one text message, then close), for clients that only speak WebSocket.
async fn lobbies_socket(State(state): State<Shared>, upgrade: WebSocketUpgrade) -> Response {
    upgrade.on_upgrade(move |mut socket| async move {
        let text = serde_json::to_string(&list(&state)).expect("list serializes");
        let _ = socket.send(Message::Text(text.into())).await;
        let _ = socket.send(Message::Close(None)).await;
    })
}

fn refuse(code: StatusCode, why: &str) -> Response {
    (code, why.to_owned()).into_response()
}

async fn join(State(state): State<Shared>, Query(query): Query<JoinQuery>, upgrade: WebSocketUpgrade) -> Response {
    let name = clean(query.name, "Player", 24);
    let password = query.password.filter(|p| !p.is_empty());
    let creating = query.create.is_some_and(|c| c != "0" && c != "false");
    let (tx, rx) = mpsc::channel::<Message>(QUEUE);
    // Seat the player before the upgrade, so "full", "wrong password" and "no such lobby" are plain HTTP answers.
    let (room_id, slot) = {
        let mut rooms = state.rooms.lock().unwrap();
        let room_id = match (query.room.map(|r| r.trim().to_uppercase()).filter(|r| !r.is_empty()), creating) {
            (Some(id), _) if rooms.contains_key(&id) => id,
            (Some(_), false) | (None, false) => return refuse(StatusCode::NOT_FOUND, "no such lobby"),
            (requested, true) => {
                if rooms.len() >= MAX_ROOMS {
                    return refuse(StatusCode::SERVICE_UNAVAILABLE, "the server has no room for another lobby");
                }
                let id = match requested {
                    Some(id) if id.len() <= 16 && id.chars().all(|c| c.is_ascii_alphanumeric() || c == '-') => id,
                    Some(_) => return refuse(StatusCode::BAD_REQUEST, "lobby codes are letters, digits and '-'"),
                    None => new_room_id(&rooms),
                };
                let title = clean(query.title, &format!("{name}'s lobby"), 48);
                rooms.insert(
                    id.clone(),
                    Room { title, password: password.clone(), switch: Switch::new(), members: Default::default() },
                );
                id
            }
        };
        let room = rooms.get_mut(&room_id).expect("room exists");
        if room.password.is_some() && room.password != password {
            return refuse(StatusCode::FORBIDDEN, "wrong lobby password");
        }
        let Some(slot) = room.switch.join() else {
            return refuse(StatusCode::CONFLICT, "the lobby is full");
        };
        room.members[slot] = Some(Member { name, tx: tx.clone() });
        (room_id, slot)
    };
    let leave_state = state.clone();
    let leave_room = room_id.clone();
    upgrade
        .max_message_size(MAX_FRAME + 64)
        .on_failed_upgrade(move |_| leave(&leave_state, &leave_room, slot))
        .on_upgrade(move |socket| member(state, room_id, slot, socket, tx, rx))
}

fn leave(state: &Lobbies, room_id: &str, slot: usize) {
    let mut rooms = state.rooms.lock().unwrap();
    if let Some(room) = rooms.get_mut(room_id) {
        room.members[slot] = None;
        room.switch.leave(slot);
        if room.switch.is_empty() {
            rooms.remove(room_id);
        } else {
            room.broadcast_roster(room_id);
        }
    }
}

async fn member(
    state: Shared,
    room_id: String,
    slot: usize,
    socket: WebSocket,
    tx: mpsc::Sender<Message>,
    mut rx: mpsc::Receiver<Message>,
) {
    let (mut sink, mut stream) = socket.split();
    let welcome = Event::Welcome { room: room_id.clone(), slot, ip: ip_text(slot) };
    let _ = tx.try_send(Message::Text(serde_json::to_string(&welcome).expect("welcome serializes").into()));
    if let Some(room) = state.rooms.lock().unwrap().get(&room_id) {
        room.broadcast_roster(&room_id);
    }
    // Writer: everything for this member goes through its queue; a ping keeps idle proxies from closing the socket.
    let writer = tokio::spawn(async move {
        let mut ping = tokio::time::interval(Duration::from_secs(20));
        loop {
            tokio::select! {
                message = rx.recv() => match message {
                    Some(message) => if sink.send(message).await.is_err() { break },
                    None => break,
                },
                _ = ping.tick() => if sink.send(Message::Ping(Default::default())).await.is_err() { break },
            }
        }
    });
    while let Some(Ok(message)) = stream.next().await {
        match message {
            Message::Binary(frame) => {
                let mut rooms = state.rooms.lock().unwrap();
                let Some(room) = rooms.get_mut(&room_id) else { break };
                for (to, frame) in room.switch.on_frame(slot, &frame) {
                    if let Some(member) = &room.members[to] {
                        let _ = member.tx.try_send(Message::Binary(frame.into())); // a full queue drops the frame
                    }
                }
            }
            Message::Close(_) => break,
            _ => {}
        }
    }
    leave(&state, &room_id, slot);
    drop(tx);
    writer.abort();
}

/// Serves until the listener fails.
pub async fn serve(listener: tokio::net::TcpListener) -> std::io::Result<()> {
    axum::serve(listener, router(Arc::new(Lobbies::default()))).await
}
