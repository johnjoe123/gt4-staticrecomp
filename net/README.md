# gt4net

Lobby and relay server, and the bridge between the game and a lobby, for GT4Recomp's LAN battle over the internet.
Design, usage and limits: [docs/ONLINE-LAN.md](../docs/ONLINE-LAN.md).

```
cargo test
cargo build --release      # target/release/gt4net
gt4net --help
```

This is its own Cargo workspace, separate from the launcher.
