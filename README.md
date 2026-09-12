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

## ⚠️ 免责声明

**本项目代码由 Doubao（豆包）AI 生成，未经人工审核。**

代码可能存在缺陷、安全漏洞或性能问题，请勿直接用于生产环境。使用前请自行审查、测试和验证。

## License

MIT
