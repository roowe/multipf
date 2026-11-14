# multipf

多语言实现的 TCP 四层反向代理 / 负载均衡器（**p**roxy **f**orwarder，简称 **pf**）。

项目起源于 [gopf](https://github.com/RaindyRoye/gopf)——一个用 Go 编写的简单端口转发器；在此基础上扩展出多种编程语言的实现，用于学习和对比不同语言在网络并发编程上的表达与性能。

## 已实现语言

| 目录 | 语言 |
|---|---|
| 根目录 (`gopf.go`) | Go |
| `gopf_ex/` | Elixir |
| `gopf_python/` | Python |
| `gopf_rust/` | Rust |
| `gopf_ts/` | TypeScript |
| `gopf_zig/` | Zig |
| `gopf_c/` | C |
| `gopf_cpp/` | C++ |

## 功能

- TCP 四层流量转发
- 基于权重的后端负载均衡
- 配置热重载（信号驱动）
- 连接保活（keep-alive）

## 快速开始（Go 版本）

```bash
go build -o gopf .
./gopf settings.conf
```

配置文件 `settings.conf` 示例：

```json
{
  "TrafficUrl": "",
  "Hosts": [
    { "Addr": "127.0.0.1:80", "Weight": 10 }
  ]
}
```

## 性能测试

对 8 种语言实现进行了系统化性能对比测试（1KB 载荷，10 秒/场景，并发 10/50/100/200）。**32 个测试场景全部零连接错误**。

| 语言 | 峰值 QPS | 峰值并发 | 200并发 QPS | 200并发 p99 | 200并发内存 | 200并发 CPU |
|---|---:|---:|---:|---:|---:|---:|
| **Zig** | 85,283 | 50 | 80,258 | 3.63ms | 47.7 MB | 241.1% |
| **C++** | 83,459 | 200 | 83,459 | 2.71ms | 23.8 MB | 90.6% |
| **C** | 83,421 | 100 | 81,776 | 3.55ms | 17.6 MB | 90.9% |
| **Rust** | 80,768 | 100 | 75,250 | 5.25ms | 29.4 MB | 227.2% |
| **Go** | 72,992 | 50 | 70,465 | 4.22ms | 28.2 MB | 423.1% |
| **Elixir** | 70,710 | 10 | 67,596 | 6.08ms | 71.6 MB | 699.0% |
| **TypeScript** | 68,468 | 10 | 68,202 | 3.39ms | 59.3 MB | 93.0% |
| **Python** | 31,266 | 10 | 24,654 | 24.44ms | 62.5 MB | 366.7% |

**C / C++ 实现说明**：两者都采用单线程事件循环（macOS `kqueue` / Linux `epoll`），无 thread-per-connection。单核 CPU 跑到 82-83K QPS，吞吐与多线程的 Rust/Zig 同一水平，内存只有其一半左右（C 200并发 17.6MB vs Zig 47.7MB）。C 使用内嵌 cJSON，C++ 使用 vendored nlohmann/json，均无运行时依赖。

> 数据为单次 10 秒采样；受机器负载影响，不同轮次间峰值 QPS 波动约 ±5-8%，同档位相对排名可能小幅变化。

**关键结论**：
- **Zig**：峰值 85K QPS，多核方案中吞吐最高
- **C / C++**：单核跑到 83K QPS（约 90% 单核），内存最低，适合容器配额受限或按核水平扩展的场景
- **Rust**：81K QPS，尾延迟表现受本轮采样波动影响
- **Go**：综合最均衡，73K QPS + 零错误 + 生态成熟
- **TypeScript**：单线程事件循环稳定，68K QPS
- **Python**：GIL 限制明显，性能约为其他实现的 1/3，仅适合原型

**并发问题修复记录**：
1. Rust 移除了 5 秒读超时（高并发下导致连接被提前断开）
2. 所有语言 listen backlog 从 128/5 提升到 4096
3. 编排脚本统一将 RLIMIT_NOFILE 从 256 提升到 65536（Rust/Zig 每连接 try_clone() 复制 fd）
4. C/C++：修复 close 后同批次事件访问已释放连接的 UAF（改为延迟回收）
5. C/C++：显式解除继承的信号屏蔽（否则 SIGHUP/SIGTERM 处理器永远不会执行）
6. bench：资源采样改用 psutil（本机 `ps` 对所有进程回报 RSS=0）

详细的测试方法、交互式图表和原始数据见 [bench/README.md](bench/README.md) 和 [bench/results/benchmark_report.html](bench/results/benchmark_report.html)。

## ⚠️ 免责声明

**本项目代码由 Doubao（豆包）AI 生成，未经人工审核。**

代码可能存在缺陷、安全漏洞或性能问题，请勿直接用于生产环境。使用前请自行审查、测试和验证。

## License

MIT
