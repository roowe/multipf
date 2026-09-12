# gopf_rust

Rust implementation of gopf — a lightweight TCP layer-4 load balancer.

## Build

Requires Rust 1.70+ (stable).

```bash
cargo build --release
```

The binary is output to `target/release/gopf_rust`.

## Usage

```bash
gopf_rust [--listen <port>] <config_file>
```

- `--listen <port>` / `-l <port>` — local port to listen on (default: 1248)
- `<config_file>` — path to JSON config file

### Config format

Same JSON format as the Go original:

```json
{
  "Hosts": [
    {"Addr": "127.0.0.1:8080", "Weight": 100},
    {"Addr": "127.0.0.1:8081", "Weight": 50}
  ]
}
```

## Signals

- `SIGHUP` — reload config from the original path (checked every 1 second)

## Architecture

| File | Purpose |
|---|---|
| `src/main.rs` | CLI entry, config loading, SIGHUP signal handler (signal-hook) |
| `src/backend.rs` | Host/Backend structs, weighted random selection, `Arc<RwLock<Backend>>` thread-safe store |
| `src/config.rs` | JSON config file loading via serde/serde_json |
| `src/proxy.rs` | TCP listener, per-connection threads, bidirectional forwarding, TCP half-close |

### Key design decisions

- **Per-connection threads**: each accepted connection spawns a dedicated handler thread; two forwarding threads per connection (one per direction) via `TcpStream::try_clone()`
- **TCP half-close**: `shutdown(Shutdown::Write)` after EOF on the source side, matching Go's `CloseWrite` behavior
- **Per-connection RNG**: `StdRng::from_entropy()` per connection — avoids global mutex contention and data races
- **Thread-safe config store**: `Arc<RwLock<Backend>>` for hot-reload with reader-writer lock
- **SIGHUP reload**: `signal-hook::flag` sets an `AtomicBool`; a background thread polls every second and reloads when set

## Dependencies

- `serde` + `serde_json` — JSON config parsing
- `signal-hook` — SIGHUP signal handling
- `rand` — weighted random selection
- `libc` — `SO_KEEPALIVE` socket option (Unix only)

## Comparison with Go/Elixir/Zig versions

| Aspect | Go | Elixir | Zig | Rust |
|---|---|---|---|---|
| Concurrency | goroutines | BEAM processes | OS threads | OS threads |
| Forwarding | `io.Copy` (2 goroutines) | recursive recv/send (2 Tasks) | recv/send loop (2 threads) | read/write loop (2 threads) |
| Half-close | `CloseWrite` | `:gen_tcp.shutdown(:write)` | `shutdown(SHUT_WR)` | `shutdown(Shutdown::Write)` |
| Config lock | `sync.RWMutex` | Agent (message passing) | `atomic.Mutex` spinlock | `RwLock` |
| Signals | `signal.Notify` | `:erl_signal_server` gen_event | `sigaction` | `signal-hook` |
| JSON | `encoding/json` | OTP 27+ `:json` | `std.json.parseFromSlice` | `serde_json` |
