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

对 6 种语言实现进行了系统化性能对比测试（1KB 载荷，10 秒/场景，并发 10/50/100/200）。修复并发问题后，**所有语言在所有并发级别下零连接错误**。

| 语言 | 峰值 QPS | 峰值并发 | 200并发 QPS | 200并发 p99 | 内存(峰值) | CPU效率 |
|---|---:|---:|---:|---:|---:|---|
| **Zig** | **85,291** | 50 | 83,777 | 4.10ms | 35.8 MB | 高 (~197%) |
| **Rust** | 84,915 | 50 | 84,086 | 3.08ms | 36.0 MB | 高 (~198%) |
| **Go** | 77,257 | 50 | 72,329 | 3.44ms | 22.5 MB | 中 (~418%) |
| **Elixir** | 70,596 | 100 | 70,022 | 6.88ms | 86.1 MB | 低 (~689%) |
| **TypeScript** | 66,972 | 50 | 66,114 | 6.06ms | 64.1 MB | 单核 (~92%) |
| **Python** | 30,879 | 50 | 25,088 | 23.78ms | 63.2 MB | 中 (~337%) |

**修复的并发问题**：
1. Rust 移除了 5 秒读超时（高并发下导致连接被提前断开）
2. 所有语言 listen backlog 从 128/5 提升到 4096
3. 编排脚本统一将 RLIMIT_NOFILE 从 256 提升到 65536（Rust/Zig 每连接 try_clone() 复制 fd）

**修复效果**（200并发）：
- Rust: QPS 19,091 → 84,086（+340%），错误 144,417 → 0
- Zig: QPS 41,737 → 83,777（+101%），错误 88,578 → 0
- Elixir: QPS 31,680 → 70,022（+121%），错误 94,277 → 0

**关键结论**：
- **Zig/Rust** 性能最强：85K QPS、最低延迟、最小内存
- **Go** 综合最均衡：77K QPS + 零错误 + 低内存 + 生态成熟
- **TypeScript** 最稳定：单线程零错误，适合 I/O 密集型
- **Python** 仅适合原型：GIL 限制，性能为其他语言的 1/3

详细测试方法、交互式图表和原始数据见 [bench/README.md](bench/README.md) 和 [bench/results/benchmark_report.html](bench/results/benchmark_report.html)。

## ⚠️ 免责声明

**本项目代码由 Doubao（豆包）AI 生成，未经人工审核。**

代码可能存在缺陷、安全漏洞或性能问题，请勿直接用于生产环境。使用前请自行审查、测试和验证。

## License

MIT
