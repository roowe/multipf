# gopf_cpp

C++ implementation of gopf — a lightweight TCP layer-4 load balancer.

## Build

Requires a C++20 compiler (clang++ / g++ 10+). No external libraries beyond
the vendored JSON header.

```bash
make            # -O2, produces ./gopf_cpp
make debug      # -O0 -g
```

## Usage

```bash
gopf_cpp [--listen <port>] <config_file>
```

- `--listen <port>` / `-l <port>` — local port to listen on (default: 1255)
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
| `src/main.cpp` | CLI entry, signal handlers, self-pipe wakeup |
| `src/config.cpp` | JSON config parsing via `nlohmann/json` |
| `src/backend.cpp` | `Host`/`Backend`, weighted `mt19937_64` selection, transactional reload |
| `src/ev.cpp` | `EventLoop`: `kqueue` on macOS, `epoll` on Linux |
| `src/proxy.cpp` | `Proxy::Impl` — listener, non-blocking connect, state machine, forwarding |
| `third_party/json.hpp` | Vendored nlohmann/json v3.11.3 (MIT) |

### Key design decisions

- **Same event-driven model as the C version**: one thread, one `EventLoop`,
  no thread-per-connection. Both implementations are deliberately comparable
  at the syscall level.
- **RAII ownership**: sockets are plain `int` fds closed by `close_conn`, and
  connections live in a `std::list<std::unique_ptr<Connection>>` so the
  address handed to the event loop is stable. Closed connections are spliced
  into a graveyard list and destroyed only after the current event batch,
  which prevents use-after-free when kqueue delivers both filters for one fd.
- **Non-blocking connect**: `EINPROGRESS` is completed by checking `SO_ERROR`
  on write readiness; connections stuck in `CONNECTING` for 5s are swept.
- **Backpressure by not reading**: a 16 KB buffer per direction; the source
  is read only when its pending buffer is empty.
- **TCP half-close**: `recv() == 0` → `shutdown(other, SHUT_WR)`; the
  connection closes only after both EOFs and all buffers drained.
- **Interest caching**: `EventLoop::modify` is only called when the desired
  read/write mask changes.
- **Transactional reload**: a failed parse keeps the old backend list.

## Dependencies

- `nlohmann/json` v3.11.3 — vendored single header (`third_party/`)
- Everything else is POSIX + the C++ standard library.
