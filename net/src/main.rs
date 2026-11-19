use gt4net::bridge::{self, JoinOptions};
use gt4net::server::Event;
use gt4net::{DEFAULT_LINK_PORT, DEFAULT_SERVER_PORT};
use std::process::ExitCode;

const HELP: &str = "\
gt4net: GT4Recomp LAN-over-internet

  gt4net server [--bind ADDRESS:PORT]
      Run a lobby server (default 0.0.0.0:47800). Put it on a VPS, or behind Cloudflare Tunnel.

  gt4net host [--name NAME] [--title TITLE] [--password PASSWORD] [--port PORT] [--cloudflare]
      Host a lobby on this PC: runs a server here and joins it. Friends join with your address,
      or with the https address printed when --cloudflare is given (needs cloudflared installed).

  gt4net create SERVER [--name NAME] [--title TITLE] [--password PASSWORD] [--room CODE]
      Open a new lobby on an existing server and join it.

  gt4net join SERVER --room CODE [--name NAME] [--password PASSWORD]
      Join a lobby. SERVER is host:port, or an https:// address.

  gt4net list SERVER
      Show the open lobbies of a server.

Common: --link-port PORT   UDP port on 127.0.0.1 where the game connects (default 47624).
Start the bridge first, then the game with GT4_NET=1.";

struct Args(Vec<String>);

impl Args {
    fn flag(&mut self, name: &str) -> bool {
        match self.0.iter().position(|a| a == name) {
            Some(i) => {
                self.0.remove(i);
                true
            }
            None => false,
        }
    }

    fn value(&mut self, name: &str) -> Result<Option<String>, String> {
        match self.0.iter().position(|a| a == name) {
            Some(i) if i + 1 < self.0.len() => {
                self.0.remove(i);
                Ok(Some(self.0.remove(i)))
            }
            Some(_) => Err(format!("{name} needs a value")),
            None => Ok(None),
        }
    }

    fn number(&mut self, name: &str, default: u16) -> Result<u16, String> {
        match self.value(name)? {
            Some(text) => text.parse().map_err(|_| format!("{name} needs a port number")),
            None => Ok(default),
        }
    }

    fn positional(&mut self, what: &str) -> Result<String, String> {
        if self.0.is_empty() { Err(format!("missing {what}")) } else { Ok(self.0.remove(0)) }
    }

    fn done(&self) -> Result<(), String> {
        match self.0.first() {
            Some(extra) => Err(format!("unexpected argument '{extra}'")),
            None => Ok(()),
        }
    }
}

fn show(event: &Event) {
    match event {
        Event::Welcome { room, slot, ip } => {
            println!("Joined lobby {room} as player {} (game address {ip}).", slot + 1);
        }
        Event::Roster { title, players, .. } => {
            let names: Vec<_> = players.iter().map(|p| format!("{} ({})", p.name, p.ip)).collect();
            println!("{title}: {} of {} players: {}", players.len(), gt4net::MAX_PLAYERS, names.join(", "));
        }
    }
}

fn default_name() -> String {
    std::env::var("USERNAME").or_else(|_| std::env::var("USER")).unwrap_or_else(|_| "Player".into())
}

/// Starts `cloudflared` with a quick tunnel to the local server and prints the public address when it appears.
fn start_cloudflare(port: u16) -> Result<tokio::process::Child, String> {
    use tokio::io::{AsyncBufReadExt, BufReader};
    let mut child = tokio::process::Command::new("cloudflared")
        .args(["tunnel", "--no-autoupdate", "--url", &format!("http://127.0.0.1:{port}")])
        .stdin(std::process::Stdio::null())
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::piped())
        .kill_on_drop(true)
        .spawn()
        .map_err(|e| format!("cannot start cloudflared ({e}); install it from Cloudflare or host without --cloudflare"))?;
    let stderr = child.stderr.take().expect("piped stderr");
    tokio::spawn(async move {
        let mut lines = BufReader::new(stderr).lines();
        while let Ok(Some(line)) = lines.next_line().await {
            if let Some(start) = line.find("https://") {
                let address: String = line[start..].chars().take_while(|c| !c.is_whitespace() && *c != '|').collect();
                if address.ends_with(".trycloudflare.com") {
                    println!("Friends join with:  {address}");
                }
            }
        }
    });
    Ok(child)
}

async fn run(mut args: Args) -> Result<(), String> {
    let command = args.positional("a command (try --help)")?;
    match command.as_str() {
        "server" => {
            let bind = args.value("--bind")?.unwrap_or_else(|| format!("0.0.0.0:{DEFAULT_SERVER_PORT}"));
            args.done()?;
            let listener = tokio::net::TcpListener::bind(&bind).await.map_err(|e| format!("cannot listen on {bind}: {e}"))?;
            println!("Lobby server on {bind}. Stop with Ctrl+C.");
            tokio::select! {
                result = gt4net::server::serve(listener) => result.map_err(|e| format!("server stopped: {e}")),
                _ = tokio::signal::ctrl_c() => Ok(()),
            }
        }
        "list" => {
            let server = args.positional("the server address")?;
            args.done()?;
            let lobbies = bridge::list(&server).await?;
            if lobbies.is_empty() {
                println!("No open lobbies.");
            }
            for lobby in lobbies {
                let lock = if lobby.locked { " [password]" } else { "" };
                println!("{}  {}  {}/{}{}  {}", lobby.id, lobby.title, lobby.players, lobby.max, lock, lobby.names.join(", "));
            }
            Ok(())
        }
        "host" | "create" | "join" => {
            let name = args.value("--name")?.unwrap_or_else(default_name);
            let title = args.value("--title")?;
            let password = args.value("--password")?;
            let room = args.value("--room")?;
            let link_port = args.number("--link-port", DEFAULT_LINK_PORT)?;
            let port = args.number("--port", DEFAULT_SERVER_PORT)?;
            let cloudflare = args.flag("--cloudflare");
            let mut _tunnel = None;
            let server = if command == "host" {
                let listener = tokio::net::TcpListener::bind(("0.0.0.0", port))
                    .await
                    .map_err(|e| format!("cannot listen on port {port}: {e}"))?;
                tokio::spawn(gt4net::server::serve(listener));
                println!("Hosting on port {port}. Friends on your network or with a forwarded port join with your address and port {port}.");
                if cloudflare {
                    _tunnel = Some(start_cloudflare(port)?);
                }
                format!("127.0.0.1:{port}")
            } else {
                args.positional("the server address")?
            };
            args.done()?;
            if command == "join" && room.is_none() {
                return Err("join needs --room CODE (see: gt4net list SERVER)".into());
            }
            let options = JoinOptions { server, room, create: command != "join", title, name, password, link_port };
            println!("Game link on 127.0.0.1:{link_port}. Start the game with GT4_NET=1.");
            tokio::select! {
                result = bridge::run(options, show) => result,
                _ = tokio::signal::ctrl_c() => Ok(()),
            }
        }
        "--help" | "-h" | "help" => {
            println!("{HELP}");
            Ok(())
        }
        other => Err(format!("unknown command '{other}' (try --help)")),
    }
}

#[tokio::main]
async fn main() -> ExitCode {
    match run(Args(std::env::args().skip(1).collect())).await {
        Ok(()) => ExitCode::SUCCESS,
        Err(why) => {
            eprintln!("gt4net: {why}");
            ExitCode::FAILURE
        }
    }
}
