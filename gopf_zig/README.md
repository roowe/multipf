# gopf_zig

Zig implementation of gopf — a lightweight TCP layer-4 load balancer.

## Build

Requires Zig 0.16.0+ and libc.

```bash
zig build
```

The binary is output to `zig-out/bin/gopf_zig`.

## Usage

```bash
gopf_zig [--listen <port>] <config_file>
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
- `SIGUSR1` — print status

## Architecture

| File | Purpose |
|---|---|
| `src/main.zig` | CLI entry, config loading, signal handlers |
| `src/backend.zig` | Host/Backend structs, weighted random selection, thread-safe store |
| `src/config.zig` | JSON config file loading via POSIX open/read |
| `src/proxy.zig` | TCP listener, per-connection threads, bidirectional forwarding |
| `src/net.zig` | Thin C socket API wrapper (`@cImport`) for Zig 0.16 compatibility |

### Key design decisions

- **Per-connection threads**: each accepted connection spawns a dedicated handler thread; two forwarding threads per connection (one per direction)
- **TCP half-close**: `shutdown(SHUT_WR)` after EOF on the source side, matching Go's `CloseWrite` behavior
- **Per-connection RNG**: each connection creates a local `DefaultPrng` seeded from the connection args pointer — avoids global mutex contention and data races
- **Thread-safe config store**: `std.atomic.Mutex` spinlock protects the current Backend for hot-reload
- **C socket wrapper**: Zig 0.16 removed most `std.posix` socket functions; `net.zig` wraps `@cImport`'d C socket API directly

## Comparison with Go/Elixir versions

| Aspect | Go | Elixir | Zig |
|---|---|---|---|
| Concurrency | goroutines | BEAM processes | OS threads |
| Forwarding | `io.Copy` (2 goroutines) | recursive recv/send (2 Tasks) | recv/send loop (2 threads) |
| Half-close | `CloseWrite` | `:gen_tcp.shutdown(:write)` | `shutdown(SHUT_WR)` |
| Config lock | `sync.RWMutex` | Agent (message passing) | `atomic.Mutex` spinlock |
| Signals | `signal.Notify` | `:erl_signal_server` gen_event | `sigaction` |
| JSON | `encoding/json` | OTP 27+ `:json` | `std.json.parseFromSlice` |
