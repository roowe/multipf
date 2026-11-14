#!/usr/bin/env python3
"""
gopf multi-language performance benchmark orchestrator.

Orchestrates:
  1. A Rust-based TCP echo backend (high-performance, fixed).
  2. One gopf proxy implementation at a time (Go / Python / Rust / TS / Zig / Elixir).
  3. An asyncio TCP load generator that hammers the proxy with concurrent
     request–response cycles and measures throughput + latency.
  4. CPU / RSS sampling of the proxy process under test.

Outputs:
  - bench/results/results.json   — raw numbers
  - bench/results/report.txt     — human-readable comparison
  - bench/results/report.md      — markdown table for README

Usage:
    python3 bench/bench.py                      # full matrix
    python3 bench/bench.py --langs go rust      # only selected languages
    python3 bench/bench.py --concurrency 10 100 # custom concurrency levels
    python3 bench/bench.py --duration 5         # shorter per-scenario duration
"""

import argparse
import asyncio
import json
import os
import resource
import signal
import statistics
import subprocess
import sys
import time
from dataclasses import dataclass, field, asdict
from pathlib import Path
from typing import Dict, List, Optional, Tuple

try:
    import psutil
except ImportError:  # optional dependency, ps remains the fallback
    psutil = None

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

PROJECT_ROOT = Path(__file__).resolve().parent.parent
BENCH_DIR = PROJECT_ROOT / "bench"
CONFIG_DIR = BENCH_DIR / "configs"
RESULTS_DIR = BENCH_DIR / "results"
BACKEND_BIN = BENCH_DIR / "backend" / "target" / "release" / "bench-backend"

BACKEND_ADDR = "127.0.0.1:9000"

# ---------------------------------------------------------------------------
# Language implementations
# ---------------------------------------------------------------------------

@dataclass
class LangImpl:
    name: str
    port: int
    cmd: List[str]  # absolute-path command, port and config appended at runtime
    listen_fmt: str = "{port}"  # Go needs ":{port}", others use "{port}"

LANGUAGES: Dict[str, LangImpl] = {
    "go": LangImpl(
        name="Go",
        port=1248,
        cmd=[str(PROJECT_ROOT / "gopf")],
        listen_fmt=":{port}",
    ),
    "python": LangImpl(
        name="Python",
        port=1249,
        cmd=[sys.executable, str(PROJECT_ROOT / "gopf_python" / "gopf.py")],
    ),
    "rust": LangImpl(
        name="Rust",
        port=1250,
        cmd=[str(PROJECT_ROOT / "gopf_rust" / "target" / "release" / "gopf_rust")],
    ),
    "typescript": LangImpl(
        name="TypeScript",
        port=1251,
        cmd=["node", str(PROJECT_ROOT / "gopf_ts" / "dist" / "main.js")],
    ),
    "zig": LangImpl(
        name="Zig",
        port=1252,
        cmd=[str(PROJECT_ROOT / "gopf_zig" / "zig-out" / "bin" / "gopf_zig")],
    ),
    "elixir": LangImpl(
        name="Elixir",
        port=1253,
        cmd=[str(PROJECT_ROOT / "gopf_ex" / "bin" / "gopf_ex")],
    ),
    "c": LangImpl(
        name="C",
        port=1254,
        cmd=[str(PROJECT_ROOT / "gopf_c" / "gopf_c")],
    ),
    "cpp": LangImpl(
        name="C++",
        port=1255,
        cmd=[str(PROJECT_ROOT / "gopf_cpp" / "gopf_cpp")],
    ),
}

# ---------------------------------------------------------------------------
# Config generation
# ---------------------------------------------------------------------------

def write_config(lang_key: str) -> Path:
    """Write a per-language settings.conf pointing at the Rust backend.

    Uses a minimal config with only ``Hosts`` so that strict JSON parsers
    (e.g. Zig) accept it; the unused ``TrafficUrl`` field is omitted.
    """
    cfg = {
        "Hosts": [{"Addr": BACKEND_ADDR, "Weight": 100}],
    }
    path = CONFIG_DIR / f"settings_{lang_key}.conf"
    path.write_text(json.dumps(cfg, indent=2))
    return path

# ---------------------------------------------------------------------------
# Process management
# ---------------------------------------------------------------------------

def start_process(cmd: List[str], log_path: Path) -> subprocess.Popen:
    """Start a subprocess, redirecting stdout/stderr to a log file."""
    log_f = open(log_path, "w")
    proc = subprocess.Popen(
        cmd,
        stdout=log_f,
        stderr=subprocess.STDOUT,
        cwd=str(PROJECT_ROOT),
        preexec_fn=os.setsid,
    )
    return proc

def stop_process(proc: subprocess.Popen):
    """Kill the whole process group."""
    try:
        os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
    except (ProcessLookupError, OSError):
        pass
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        except (ProcessLookupError, OSError):
            pass
        proc.wait()

def wait_for_port(port: int, timeout: float = 10.0) -> bool:
    """Wait until a TCP port accepts connections."""
    import socket
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                return True
        except OSError:
            time.sleep(0.1)
    return False

# ---------------------------------------------------------------------------
# Resource sampling
# ---------------------------------------------------------------------------

@dataclass
class ResourceSample:
    cpu_percent: float
    rss_mb: float

_psutil_procs: Dict[int, object] = {}

def sample_resources(pid: int) -> Optional[ResourceSample]:
    """Sample CPU% and RSS (MB) of a process.

    psutil is preferred: on some macOS builds `ps` reports rss=0 for every
    process, which silently produces empty resource charts.
    """
    if psutil is not None:
        try:
            proc = _psutil_procs.get(pid)
            if proc is None:
                proc = psutil.Process(pid)
                proc.cpu_percent(interval=None)  # prime the delta counter
                _psutil_procs[pid] = proc
            return ResourceSample(
                cpu_percent=proc.cpu_percent(interval=None),
                rss_mb=proc.memory_info().rss / (1024.0 * 1024.0),
            )
        except Exception:
            _psutil_procs.pop(pid, None)

    try:
        out = subprocess.check_output(
            ["ps", "-p", str(pid), "-o", "%cpu=,rss="],
            text=True,
        ).strip()
        if not out:
            return None
        parts = out.split()
        cpu = float(parts[0])
        rss_kb = float(parts[1])
        if rss_kb <= 0:
            return None
        return ResourceSample(cpu_percent=cpu, rss_mb=rss_kb / 1024.0)
    except (subprocess.CalledProcessError, ValueError, IndexError):
        return None

async def resource_sampler(pid: int, interval: float, results: List[ResourceSample], stop_event: asyncio.Event):
    """Periodically sample process resources until stop_event is set."""
    while not stop_event.is_set():
        s = sample_resources(pid)
        if s is not None:
            results.append(s)
        try:
            await asyncio.wait_for(stop_event.wait(), timeout=interval)
        except asyncio.TimeoutError:
            pass

# ---------------------------------------------------------------------------
# Load generator
# ---------------------------------------------------------------------------

@dataclass
class BenchResult:
    lang: str
    concurrency: int
    duration_sec: float
    payload_size: int
    total_requests: int
    qps: float
    avg_latency_ms: float
    p50_latency_ms: float
    p90_latency_ms: float
    p95_latency_ms: float
    p99_latency_ms: float
    max_latency_ms: float
    min_latency_ms: float
    errors: int
    cpu_avg: float
    cpu_max: float
    rss_avg_mb: float
    rss_max_mb: float
    latency_stdev_ms: float = 0.0

async def worker(
    port: int,
    payload: bytes,
    stop_event: asyncio.Event,
    latencies: List[float],
    errors: List[int],
):
    """One concurrent connection: repeatedly send payload, wait for echo."""
    reader = writer = None
    try:
        reader, writer = await asyncio.open_connection("127.0.0.1", port)
        while not stop_event.is_set():
            t0 = time.perf_counter()
            try:
                writer.write(payload)
                await writer.drain()
                # Read exactly len(payload) bytes back (echo server)
                received = 0
                buf = bytearray()
                while received < len(payload):
                    chunk = await reader.read(len(payload) - received)
                    if not chunk:
                        raise ConnectionError("backend closed")
                    buf.extend(chunk)
                    received += len(chunk)
                t1 = time.perf_counter()
                latencies.append((t1 - t0) * 1000.0)
            except (ConnectionError, OSError, asyncio.IncompleteReadError):
                errors[0] += 1
                # Reconnect
                try:
                    writer.close()
                    await writer.wait_closed()
                except Exception:
                    pass
                try:
                    reader, writer = await asyncio.open_connection("127.0.0.1", port)
                except OSError:
                    await asyncio.sleep(0.05)
    except OSError:
        errors[0] += 1
    finally:
        if writer is not None:
            try:
                writer.close()
                await writer.wait_closed()
            except Exception:
                pass

async def run_scenario(
    lang: LangImpl,
    concurrency: int,
    duration: float,
    payload_size: int,
) -> BenchResult:
    """Run one benchmark scenario against a currently-running proxy."""
    payload = b"A" * payload_size
    stop_event = asyncio.Event()
    latencies: List[float] = []
    errors = [0]

    # Warm-up: open one connection and do a few rounds
    try:
        reader, writer = await asyncio.open_connection("127.0.0.1", lang.port)
        for _ in range(5):
            writer.write(payload)
            await writer.drain()
            await reader.readexactly(len(payload))
        writer.close()
        await writer.wait_closed()
    except Exception as e:
        print(f"  [warm-up failed for {lang.name}: {e}]")

    # Start workers
    tasks = [
        asyncio.create_task(worker(lang.port, payload, stop_event, latencies, errors))
        for _ in range(concurrency)
    ]

    # Let them run
    await asyncio.sleep(duration)
    stop_event.set()

    # Wait for workers to finish (with timeout)
    try:
        await asyncio.wait_for(asyncio.gather(*tasks), timeout=5.0)
    except asyncio.TimeoutError:
        for t in tasks:
            t.cancel()

    total_requests = len(latencies)
    qps = total_requests / duration if duration > 0 else 0

    if latencies:
        sorted_lat = sorted(latencies)
        avg_lat = statistics.mean(sorted_lat)
        stdev_lat = statistics.stdev(sorted_lat) if len(sorted_lat) > 1 else 0.0
        p50 = percentile(sorted_lat, 50)
        p90 = percentile(sorted_lat, 90)
        p95 = percentile(sorted_lat, 95)
        p99 = percentile(sorted_lat, 99)
        min_lat = sorted_lat[0]
        max_lat = sorted_lat[-1]
    else:
        avg_lat = stdev_lat = p50 = p90 = p95 = p99 = min_lat = max_lat = 0.0

    return BenchResult(
        lang=lang.name,
        concurrency=concurrency,
        duration_sec=duration,
        payload_size=payload_size,
        total_requests=total_requests,
        qps=round(qps, 2),
        avg_latency_ms=round(avg_lat, 3),
        p50_latency_ms=round(p50, 3),
        p90_latency_ms=round(p90, 3),
        p95_latency_ms=round(p95, 3),
        p99_latency_ms=round(p99, 3),
        max_latency_ms=round(max_lat, 3),
        min_latency_ms=round(min_lat, 3),
        latency_stdev_ms=round(stdev_lat, 3),
        errors=errors[0],
        cpu_avg=0.0,
        cpu_max=0.0,
        rss_avg_mb=0.0,
        rss_max_mb=0.0,
    )

def percentile(sorted_data: List[float], pct: float) -> float:
    if not sorted_data:
        return 0.0
    k = (len(sorted_data) - 1) * (pct / 100.0)
    f = int(k)
    c = min(f + 1, len(sorted_data) - 1)
    if f == c:
        return sorted_data[f]
    return sorted_data[f] + (sorted_data[c] - sorted_data[f]) * (k - f)

# ---------------------------------------------------------------------------
# Full benchmark for one language
# ---------------------------------------------------------------------------

async def benchmark_language(
    lang_key: str,
    lang: LangImpl,
    concurrency_levels: List[int],
    duration: float,
    payload_size: int,
) -> List[BenchResult]:
    print(f"\n{'='*60}")
    print(f"Benchmarking: {lang.name} (port {lang.port})")
    print(f"{'='*60}")

    config_path = write_config(lang_key)
    log_path = RESULTS_DIR / f"log_{lang_key}.txt"

    listen_arg = lang.listen_fmt.format(port=lang.port)
    cmd = lang.cmd + ["--listen", listen_arg, str(config_path)]
    print(f"  Starting: {' '.join(cmd)}")
    proc = start_process(cmd, log_path)

    # Wait for proxy to be ready
    if not wait_for_port(lang.port, timeout=15):
        print(f"  [ERROR] {lang.name} proxy failed to start on port {lang.port}")
        print(f"  Check log: {log_path}")
        stop_process(proc)
        return []

    # Small delay for stabilisation
    await asyncio.sleep(1.0)

    results: List[BenchResult] = []
    for conc in concurrency_levels:
        print(f"  --- concurrency={conc}, duration={duration}s, payload={payload_size}B ---")

        # Start resource sampler
        samples: List[ResourceSample] = []
        stop_sampler = asyncio.Event()
        sampler_task = asyncio.create_task(
            resource_sampler(proc.pid, 0.5, samples, stop_sampler)
        )

        # Let sampler get a baseline
        await asyncio.sleep(0.3)

        result = await run_scenario(lang, conc, duration, payload_size)

        # Stop sampler
        stop_sampler.set()
        await asyncio.wait_for(sampler_task, timeout=2.0)

        # Aggregate resource stats
        if samples:
            cpus = [s.cpu_percent for s in samples]
            rss_list = [s.rss_mb for s in samples]
            result.cpu_avg = round(statistics.mean(cpus), 2)
            result.cpu_max = round(max(cpus), 2)
            result.rss_avg_mb = round(statistics.mean(rss_list), 2)
            result.rss_max_mb = round(max(rss_list), 2)

        print(f"    QPS={result.qps:>10.2f}  avg={result.avg_latency_ms:>8.3f}ms  "
              f"p95={result.p95_latency_ms:>8.3f}ms  p99={result.p99_latency_ms:>8.3f}ms  "
              f"CPU={result.cpu_avg:>6.1f}%  RSS={result.rss_avg_mb:>7.1f}MB  "
              f"err={result.errors}")
        results.append(result)

        # Cool-down between scenarios
        await asyncio.sleep(1.0)

    stop_process(proc)
    print(f"  {lang.name} done.")
    return results

# ---------------------------------------------------------------------------
# Backend management
# ---------------------------------------------------------------------------

def start_backend() -> subprocess.Popen:
    log_path = RESULTS_DIR / "log_backend.txt"
    cmd = [str(BACKEND_BIN), "--listen", BACKEND_ADDR, "--mode", "echo"]
    print(f"Starting backend: {' '.join(cmd)}")
    proc = start_process(cmd, log_path)
    if not wait_for_port(9000, timeout=10):
        raise RuntimeError("Backend failed to start on port 9000")
    print("Backend ready.")
    return proc

# ---------------------------------------------------------------------------
# Report generation
# ---------------------------------------------------------------------------

def generate_reports(all_results: List[BenchResult], concurrency_levels: List[int]):
    """Generate text and markdown reports."""
    # Text report
    lines = []
    lines.append("=" * 90)
    lines.append("gopf Multi-Language Performance Benchmark Report")
    lines.append("=" * 90)
    lines.append(f"Backend: Rust tokio echo server ({BACKEND_ADDR})")
    lines.append(f"Payload: {all_results[0].payload_size if all_results else 0} bytes")
    lines.append(f"Duration per scenario: {all_results[0].duration_sec if all_results else 0}s")
    lines.append(f"Concurrency levels: {concurrency_levels}")
    lines.append("")

    for conc in concurrency_levels:
        lines.append("-" * 90)
        lines.append(f"Concurrency = {conc}")
        lines.append("-" * 90)
        header = (f"{'Language':<12} {'QPS':>10} {'avg(ms)':>10} {'p50(ms)':>10} "
                  f"{'p95(ms)':>10} {'p99(ms)':>10} {'max(ms)':>10} {'CPU%':>7} {'RSS(MB)':>9} {'err':>5}")
        lines.append(header)
        lines.append("-" * 90)
        for r in all_results:
            if r.concurrency == conc:
                lines.append(
                    f"{r.lang:<12} {r.qps:>10.2f} {r.avg_latency_ms:>10.3f} "
                    f"{r.p50_latency_ms:>10.3f} {r.p95_latency_ms:>10.3f} "
                    f"{r.p99_latency_ms:>10.3f} {r.max_latency_ms:>10.3f} "
                    f"{r.cpu_avg:>7.1f} {r.rss_avg_mb:>9.1f} {r.errors:>5}"
                )
        lines.append("")

    # Summary: best QPS per language across all concurrency levels
    lines.append("=" * 90)
    lines.append("Summary: Peak QPS per language")
    lines.append("=" * 90)
    best = {}
    for r in all_results:
        if r.lang not in best or r.qps > best[r.lang].qps:
            best[r.lang] = r
    for lang_name, r in sorted(best.items(), key=lambda x: -x[1].qps):
        lines.append(f"  {lang_name:<12} peak QPS = {r.qps:>10.2f}  "
                     f"@ concurrency={r.concurrency}  avg_lat={r.avg_latency_ms}ms  "
                     f"CPU={r.cpu_avg}%  RSS={r.rss_avg_mb}MB")
    lines.append("")

    report_text = "\n".join(lines)
    (RESULTS_DIR / "report.txt").write_text(report_text)
    print("\n" + report_text)

    # Markdown report
    md_lines = []
    md_lines.append("# gopf Multi-Language Performance Benchmark\n")
    md_lines.append(f"- **Backend**: Rust tokio echo server (`{BACKEND_ADDR}`)")
    md_lines.append(f"- **Payload**: {all_results[0].payload_size if all_results else 0} bytes (echo)")
    md_lines.append(f"- **Duration**: {all_results[0].duration_sec if all_results else 0}s per scenario")
    md_lines.append(f"- **Concurrency levels**: {', '.join(map(str, concurrency_levels))}\n")

    for conc in concurrency_levels:
        md_lines.append(f"## Concurrency = {conc}\n")
        md_lines.append("| Language | QPS | avg (ms) | p50 (ms) | p95 (ms) | p99 (ms) | max (ms) | CPU % | RSS (MB) | errors |")
        md_lines.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
        for r in all_results:
            if r.concurrency == conc:
                md_lines.append(
                    f"| {r.lang} | {r.qps:.2f} | {r.avg_latency_ms:.3f} | "
                    f"{r.p50_latency_ms:.3f} | {r.p95_latency_ms:.3f} | "
                    f"{r.p99_latency_ms:.3f} | {r.max_latency_ms:.3f} | "
                    f"{r.cpu_avg:.1f} | {r.rss_avg_mb:.1f} | {r.errors} |"
                )
        md_lines.append("")

    (RESULTS_DIR / "report.md").write_text("\n".join(md_lines))

    # JSON results
    json_data = [asdict(r) for r in all_results]
    (RESULTS_DIR / "results.json").write_text(json.dumps(json_data, indent=2))

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

async def main_async(args):
    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    CONFIG_DIR.mkdir(parents=True, exist_ok=True)

    selected_langs = args.langs if args.langs else list(LANGUAGES.keys())
    # Validate
    for k in selected_langs:
        if k not in LANGUAGES:
            print(f"Unknown language: {k}. Available: {list(LANGUAGES.keys())}")
            sys.exit(1)

    # Start backend
    backend_proc = start_backend()
    try:
        all_results: List[BenchResult] = []
        for lang_key in selected_langs:
            lang = LANGUAGES[lang_key]
            results = await benchmark_language(
                lang_key, lang, args.concurrency, args.duration, args.payload
            )
            all_results.extend(results)

        if all_results:
            generate_reports(all_results, args.concurrency)
        else:
            print("No results collected.")
    finally:
        print("\nStopping backend...")
        stop_process(backend_proc)
        print("Done.")

def main():
    # Raise file descriptor limit for high-concurrency tests.
    # Rust/Zig proxies use try_clone() (dup) per connection, so 200
    # concurrent connections can consume ~800+ FDs. macOS default is 256.
    try:
        soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
        target = min(65536, hard)
        if soft < target:
            resource.setrlimit(resource.RLIMIT_NOFILE, (target, hard))
            print(f"Raised RLIMIT_NOFILE: {soft} -> {target} (hard={hard})")
    except (ValueError, OSError) as e:
        print(f"Warning: could not raise RLIMIT_NOFILE: {e}")

    parser = argparse.ArgumentParser(description="gopf multi-language benchmark")
    parser.add_argument(
        "--langs", nargs="+", default=None,
        help="Languages to benchmark (default: all). Choices: go python rust typescript zig elixir",
    )
    parser.add_argument(
        "--concurrency", nargs="+", type=int, default=[10, 50, 100, 200],
        help="Concurrency levels to test (default: 10 50 100 200)",
    )
    parser.add_argument(
        "--duration", type=float, default=10.0,
        help="Duration in seconds per scenario (default: 10)",
    )
    parser.add_argument(
        "--payload", type=int, default=1024,
        help="Payload size in bytes (default: 1024)",
    )
    args = parser.parse_args()
    asyncio.run(main_async(args))

if __name__ == "__main__":
    main()
