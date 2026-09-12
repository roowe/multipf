# gopf 多语言性能测试

对 gopf 项目中 6 种语言实现的 TCP 四层反向代理进行系统化性能对比测试。

## 架构

```
┌─────────────────┐     ┌──────────────────┐     ┌─────────────────────┐
│  Python 编排器   │────▶│  被测代理 (6种)   │────▶│  Rust Backend       │
│  bench/bench.py │     │  Go/Py/Rust/TS/  │     │  tokio echo server  │
│  - 启动/停止     │     │  Zig/Elixir      │     │  127.0.0.1:9000    │
│  - 生成配置      │     └──────────────────┘     └─────────────────────┘
│  - 并发压测      │
│  - 采集 CPU/内存 │
│  - 生成报告      │
└─────────────────┘
```

- **Backend**：Rust + tokio 实现的高性能 TCP echo server（`bench/backend/`），作为所有代理的统一后端，确保后端不成为瓶颈。
- **编排器**：Python asyncio 实现（`bench/bench.py`），负责启动 backend、依次启动各语言代理、运行并发压测、采集资源指标、生成报告。
- **压测方式**：asyncio 异步 TCP 客户端，固定并发连接数，每个连接持续发送 1KB 请求并等待 echo 响应，测量 QPS 和延迟分布。

## 目录结构

```
bench/
├── backend/                    # Rust tokio echo server (测试后端)
│   ├── Cargo.toml
│   ├── src/main.rs
│   └── target/release/bench-backend
├── configs/                    # 各语言代理的配置文件 (自动生成)
├── results/                    # 测试结果
│   ├── results.json            # 原始数据
│   ├── report.txt              # 文本报告
│   ├── report.md               # Markdown 报告
│   ├── benchmark_report.html   # 交互式可视化报告
│   └── log_*.txt               # 各代理运行日志
├── bench.py                    # 编排和压测主脚本
└── README.md                   # 本文件
```

## 使用方法

### 1. 编译 backend

```bash
cd bench/backend
cargo build --release
```

### 2. 运行完整测试

```bash
# 在项目根目录运行
python3 bench/bench.py
```

默认参数：
- 并发级别：10, 50, 100, 200
- 单场景时长：10 秒
- 请求载荷：1024 字节
- 测试语言：全部 6 种

### 3. 自定义参数

```bash
# 只测试 Go 和 Rust
python3 bench/bench.py --langs go rust

# 自定义并发级别
python3 bench/bench.py --concurrency 10 100 500

# 缩短测试时间
python3 bench/bench.py --duration 5

# 自定义载荷大小
python3 bench/bench.py --payload 4096
```

### 4. 查看结果

```bash
# 交互式 HTML 报告（推荐，浏览器打开）
open bench/results/benchmark_report.html

# 文本报告
cat bench/results/report.txt

# 原始 JSON 数据
cat bench/results/results.json
```

## 测试指标

| 指标 | 说明 |
|---|---|
| QPS | 每秒完成的请求-响应数 |
| avg / p50 / p90 / p95 / p99 / max | 延迟分布（毫秒） |
| CPU % | 代理进程平均 CPU 使用率（多核可 >100%） |
| RSS MB | 代理进程平均常驻内存 |
| errors | 压测期间的连接错误数 |

## 各语言代理启动方式

| 语言 | 端口 | 启动命令 |
|---|---|---|
| Go | 1248 | `./gopf --listen :1248 settings.conf` |
| Python | 1249 | `python3 gopf_python/gopf.py --listen 1249 settings.conf` |
| Rust | 1250 | `./gopf_rust/target/release/gopf_rust --listen 1250 settings.conf` |
| TypeScript | 1251 | `node gopf_ts/dist/main.js --listen 1251 settings.conf` |
| Zig | 1252 | `./gopf_zig/zig-out/bin/gopf_zig --listen 1252 settings.conf` |
| Elixir | 1253 | `./gopf_ex/bin/gopf_ex --listen 1253 settings.conf` |

> 注意：Go 版本的 `--listen` 参数需要 `:port` 格式（如 `:1248`），其他语言使用纯端口号。

## 已知限制

1. **回环测试**：所有组件运行在同一台机器的 loopback 接口上，结果反映的是代理本身的 CPU/内存开销，不包含真实网络延迟。
2. **短连接模型**：压测使用持续连接反复发送请求，不测试连接建立/销毁的开销。
3. **单后端**：所有代理配置为单后端（127.0.0.1:9000），不测试负载均衡逻辑。
4. **macOS 平台**：测试在 macOS 上运行，Linux 上的 epoll/io_uring 可能带来不同结果。
5. **文件描述符限制**：编排脚本会自动将 RLIMIT_NOFILE 提升到 65536。Rust/Zig 代理使用 `try_clone()`（dup）复制 socket，200 并发下每个连接约占 4 个 fd，需确保系统 fd 限制足够。

## 并发问题修复记录

首轮测试发现 Rust/Zig/Elixir/Python 在高并发下出现大量连接错误，已修复：

| 语言 | 问题 | 修复 |
|---|---|---|
| Rust | 5 秒读超时导致高并发下连接被提前断开 | 移除 `set_read_timeout`，依赖 peer FIN 自然终止 |
| Rust/Zig/Elixir/Python | listen backlog 过小（128 或默认 5），高并发下新连接被丢弃 | 统一提升到 4096 |
| 全部 | macOS 默认 RLIMIT_NOFILE=256，Rust/Zig 每连接 dup 4 个 fd 导致耗尽 | 编排脚本启动时提升到 65536 |

修复后 24 个测试场景（6 语言 × 4 并发级别）全部零错误。
