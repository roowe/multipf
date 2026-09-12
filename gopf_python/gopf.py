#!/usr/bin/env python3
"""
gopf_python — a lightweight TCP layer-4 load balancer in pure Python.

Usage:
    python3 gopf.py [--listen <port>] <config_file>

Config format (JSON, same as Go original):
    {"Hosts": [{"Addr": "127.0.0.1:8080", "Weight": 100}]}

Signals:
    SIGHUP  — reload config
"""

import argparse
import json
import os
import random
import signal
import socket
import sys
import threading
from dataclasses import dataclass, field
from typing import List, Optional

# ---------------------------------------------------------------------------
# Backend
# ---------------------------------------------------------------------------

@dataclass
class Host:
    addr: str
    weight: int


@dataclass
class Backend:
    hosts: List[Host]
    total_weight: int = 0

    def __post_init__(self):
        self.total_weight = sum(h.weight for h in self.hosts)


class BackendStore:
    """Thread-safe backend storage with hot-reload support."""

    def __init__(self, backend: Backend):
        self._backend = backend
        self._lock = threading.Lock()

    def get(self) -> Backend:
        with self._lock:
            return self._backend

    def update(self, new_backend: Backend):
        with self._lock:
            self._backend = new_backend


def choose_host(rng: random.Random, backend: Backend) -> Optional[Host]:
    """Weighted random host selection. Returns None if total_weight <= 0."""
    if backend.total_weight <= 0 or not backend.hosts:
        return None
    v = rng.randrange(backend.total_weight)
    for host in backend.hosts:
        if v < host.weight:
            return host
        v -= host.weight
    return None


# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------

def load_config(path: str) -> Backend:
    """Load and parse a JSON config file."""
    with open(path, "r") as f:
        data = json.load(f)
    hosts = [
        Host(addr=h["Addr"], weight=h["Weight"])
        for h in data.get("Hosts", [])
    ]
    return Backend(hosts=hosts)


# ---------------------------------------------------------------------------
# Proxy
# ---------------------------------------------------------------------------

CONNECT_TIMEOUT = 5.0
RECV_BUF_SIZE = 64 * 1024


def start_proxy(port: int, store: BackendStore):
    """Start the TCP proxy listener. Blocks the calling thread."""
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("0.0.0.0", port))
    listener.listen(4096)  # large backlog to handle bursty connections
    print(f"gopf_python listening on port {port}", flush=True)

    while True:
        try:
            client, _addr = listener.accept()
        except OSError as e:
            print(f"accept failed: {e}", file=sys.stderr, flush=True)
            continue
        t = threading.Thread(
            target=handle_connection,
            args=(client, store),
            daemon=True,
        )
        t.start()


def handle_connection(client: socket.socket, store: BackendStore):
    """Handle a single client connection."""
    # Per-connection RNG (thread-safe, no global contention)
    rng = random.Random()

    backend = store.get()
    host = choose_host(rng, backend)
    if host is None:
        print("choose host failed", file=sys.stderr, flush=True)
        client.close()
        return

    try:
        upstream = socket.create_connection(
            tuple(host.addr.rsplit(":", 1)),
            timeout=CONNECT_TIMEOUT,
        )
    except OSError as e:
        print(f"connect to {host.addr} failed: {e}", file=sys.stderr, flush=True)
        client.close()
        return

    upstream.settimeout(None)  # blocking mode for forwarding

    set_keepalive(client)
    set_keepalive(upstream)

    forward_both(client, upstream)

    client.close()
    upstream.close()


def set_keepalive(sock: socket.socket):
    """Enable SO_KEEPALIVE on the socket."""
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
    except OSError:
        pass


def forward_both(client: socket.socket, upstream: socket.socket):
    """Bidirectional forwarding: two threads, one per direction."""
    t1 = threading.Thread(
        target=forward,
        args=(client, upstream),
        daemon=True,
    )
    t2 = threading.Thread(
        target=forward,
        args=(upstream, client),
        daemon=True,
    )
    t1.start()
    t2.start()
    t1.join()
    t2.join()


def forward(source: socket.socket, dest: socket.socket):
    """Forward data from source to dest until source closes its read side.
    On source EOF, half-closes dest's write side (TCP half-close)."""
    try:
        while True:
            try:
                data = source.recv(RECV_BUF_SIZE)
            except OSError:
                break
            if not data:
                break  # peer closed write side
            try:
                dest.sendall(data)
            except OSError:
                break
    finally:
        # Half-close: signal dest that no more data is coming
        try:
            dest.shutdown(socket.SHUT_WR)
        except OSError:
            pass


# ---------------------------------------------------------------------------
# CLI & Signals
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="gopf_python — a lightweight TCP layer-4 load balancer",
        usage="gopf_python [--listen <port>] <config_file>",
    )
    parser.add_argument(
        "--listen", "-l",
        type=int,
        default=1248,
        help="local port to listen on (default: 1248)",
    )
    parser.add_argument(
        "config_file",
        help="path to JSON config file",
    )
    args = parser.parse_args()

    # Load initial config
    try:
        initial_backend = load_config(args.config_file)
    except Exception as e:
        print(f"load config failed: {e}", file=sys.stderr, flush=True)
        sys.exit(1)

    store = BackendStore(initial_backend)
    config_path = args.config_file

    # SIGHUP → reload config
    def handle_sighup(signum, frame):
        try:
            new_backend = load_config(config_path)
            store.update(new_backend)
            print("reload succeed", flush=True)
        except Exception as e:
            print(f"reload failed: {e}", file=sys.stderr, flush=True)

    signal.signal(signal.SIGHUP, handle_sighup)

    # Start proxy (blocks)
    try:
        start_proxy(args.listen, store)
    except KeyboardInterrupt:
        print("\nshutting down", flush=True)
        sys.exit(0)


if __name__ == "__main__":
    main()
