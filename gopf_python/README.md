# gopf_python

Pure-Python implementation of gopf — a lightweight TCP layer-4 load balancer.

Zero external dependencies — uses only the Python standard library.

## Requirements

- Python 3.8+

## Usage

```bash
python3 gopf.py [--listen <port>] <config_file>
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

- `SIGHUP` — reload config from the original path

## Architecture

Single file (`gopf.py`) with clear section separation:

| Section | Purpose |
|---|---|
| `Backend` | `Host`/`Backend` dataclasses, weighted random selection, `BackendStore` (threading.Lock) |
| `Config` | JSON config file loading |
| `Proxy` | TCP listener, per-connection threads, bidirectional forwarding, TCP half-close |
| `CLI & Signals` | argparse CLI, SIGHUP handler |

### Key design decisions

- **Per-connection threads**: each accepted connection spawns a daemon thread; two forwarding threads per connection (one per direction)
- **TCP half-close**: `socket.shutdown(SHUT_WR)` after EOF on the source side, matching Go's `CloseWrite` behavior
- **Per-connection RNG**: `random.Random()` per connection — avoids global lock contention
- **Thread-safe config store**: `threading.Lock` protects the current Backend for hot-reload
- **SIGHUP reload**: `signal.signal(SIGHUP, handler)` directly reloads config in the main thread
- **Zero dependencies**: only `socket`, `threading`, `json`, `signal`, `argparse`, `random` from stdlib

## Comparison with Go/Elixir/Zig/Rust versions

| Aspect | Go | Elixir | Zig | Rust | Python |
|---|---|---|---|---|---|
| Concurrency | goroutines | BEAM processes | OS threads | OS threads | OS threads |
| Forwarding | `io.Copy` (2 goroutines) | recursive recv/send (2 Tasks) | recv/send loop (2 threads) | read/write loop (2 threads) | recv/sendall loop (2 threads) |
| Half-close | `CloseWrite` | `:gen_tcp.shutdown(:write)` | `shutdown(SHUT_WR)` | `shutdown(Write)` | `shutdown(SHUT_WR)` |
| Config lock | `sync.RWMutex` | Agent (message passing) | `atomic.Mutex` spinlock | `RwLock` | `threading.Lock` |
| Signals | `signal.Notify` | `:erl_signal_server` gen_event | `sigaction` | `signal-hook` | `signal.signal` |
| JSON | `encoding/json` | OTP 27+ `:json` | `std.json.parseFromSlice` | `serde_json` | `json.load` |
| Dependencies | stdlib | stdlib | stdlib + libc | 5 crates | stdlib only |
