# gopf_c

C implementation of gopf — a lightweight TCP layer-4 load balancer.

## Build

Requires a C11 compiler (clang / gcc) and `make`. No external libraries.

```bash
make            # -O2, produces ./gopf_c
make debug      # -O0 -g
```

## Usage

```bash
gopf_c [--listen <port>] <config_file>
```

- `--listen <port>` / `-l <port>` — local port to listen on (default: 1254)
- `<config_file>` — path to JSON config file

### Config format

Same JSON format as the Go original:

```json
{
  "Hosts": [
    {"Addr": "127.0.0.1:9000", "Weight": 100}
  ]
}
```

## Signals

- `SIGHUP` — reload config from the original path
- `SIGUSR1` — print the current connection count to stderr
- `SIGINT` / `SIGTERM` — graceful shutdown

## Architecture

| File | Purpose |
|---|---|
| `src/main.c` | CLI entry, signal handlers, self-pipe wakeup |
| `src/config.c` | JSON config parsing via vendored cJSON |
| `src/backend.c` | Host/Backend structs, weighted random selection, config swap |
| `src/ev.c` | Readiness abstraction: `kqueue` on macOS, `epoll` on Linux |
| `src/proxy.c` | Listener, non-blocking connect, connection state machine, forwarding |
| `src/vendor/cJSON.c` | Vendored cJSON v1.7.18 (MIT) |

### Key design decisions

- **Single-threaded event loop**: one thread drives all connections through
  `kqueue`/`epoll`. No thread-per-connection — the earlier benchmark showed
  that model collapsing at 200 concurrent clients.
- **Non-blocking state machine**: each connection moves
  `CONNECTING → FORWARDING`, with a 5s connect timeout swept once per second.
  `connect()` returning `EINPROGRESS` is completed by checking `SO_ERROR`
  on write readiness.
- **Backpressure by not reading**: each direction has a single 16 KB buffer.
  A source is only read when its pending write buffer is empty, so buffers
  never grow and memory stays flat under load.
- **TCP half-close**: `recv() == 0` triggers `shutdown(other, SHUT_WR)`; the
  connection only closes once both directions have EOF'd and buffers drained.
- **Interest caching**: the loop only issues `kevent`/`epoll_ctl` when the
  desired read/write mask actually changes. Removing the redundant
  re-registration raised peak throughput from ~69k to ~83k QPS.
- **Self-pipe signal handling**: handlers only write one byte; config parsing
  and shutdown happen on the event-loop thread.
- **Unblocked signals**: the process explicitly clears `SIGHUP/SIGINT/SIGTERM/SIGUSR1`
  from the inherited signal mask, so supervisors that start children with a
  full mask cannot silently disable reload and shutdown.

## Dependencies

- `cJSON` v1.7.18 — vendored, MIT license (`src/vendor/`)
- Everything else is POSIX + libc.
