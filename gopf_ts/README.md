# gopf_ts

TypeScript / Node.js implementation of gopf — a lightweight TCP layer-4 load balancer.

Zero runtime dependencies — uses only Node.js built-in modules.

## Requirements

- Node.js 18+
- TypeScript 5+ (build-time only)

## Build

```bash
npm install
npm run build
```

Compiled JavaScript is output to `dist/`.

## Usage

```bash
node dist/main.js [--listen <port>] <config_file>
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
- `SIGINT` / `SIGTERM` — graceful shutdown

## Architecture

| File | Purpose |
|---|---|
| `src/main.ts` | CLI entry, config loading, SIGHUP/SIGINT signal handlers |
| `src/backend.ts` | Host/Backend interfaces, weighted random selection, BackendStore |
| `src/config.ts` | JSON config file loading |
| `src/proxy.ts` | TCP listener, per-connection handling, bidirectional forwarding, TCP half-close |

### Key design decisions

- **Event-driven, single-threaded**: Node.js event loop handles all connections concurrently without per-connection threads
- **`allowHalfOpen: true`**: critical on both server and client sockets. By default Node.js auto-destroys the socket when the peer sends FIN. `allowHalfOpen: true` keeps the write side open, enabling TCP half-close — same as Go's `CloseWrite` and Elixir's `exit_on_close: false`
- **Explicit data event forwarding**: `socket.on('data')` → `other.write(data)`, with `end` event triggering `other.end()` for half-close
- **Per-connection no RNG lock needed**: `Math.random()` is per-isolate and thread-safe in Node.js single-threaded model
- **No config lock needed**: Node.js is single-threaded, so BackendStore updates are atomic with respect to the event loop
- **Zero runtime dependencies**: only `net`, `fs`, `process` from Node.js built-ins

## Comparison with Go/Elixir/Zig/Rust/Python versions

| Aspect | Go | Elixir | Zig | Rust | Python | TypeScript |
|---|---|---|---|---|---|---|
| Concurrency | goroutines | BEAM processes | OS threads | OS threads | OS threads | Event loop |
| Forwarding | `io.Copy` | recursive recv/send | recv/send loop | read/write loop | recv/sendall loop | data events |
| Half-close | `CloseWrite` | `exit_on_close: false` | `shutdown(SHUT_WR)` | `shutdown(Write)` | `shutdown(SHUT_WR)` | `allowHalfOpen: true` |
| Config lock | `sync.RWMutex` | Agent | `atomic.Mutex` | `RwLock` | `threading.Lock` | none (single-threaded) |
| Signals | `signal.Notify` | `:erl_signal_server` | `sigaction` | `signal-hook` | `signal.signal` | `process.on` |
| JSON | `encoding/json` | OTP 27 `:json` | `std.json` | `serde_json` | `json.load` | `JSON.parse` |
| Runtime deps | stdlib | stdlib | libc | 5 crates | stdlib | zero (built-ins) |
