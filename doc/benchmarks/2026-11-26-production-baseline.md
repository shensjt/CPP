# 2026-11-26 生产基线：Caddy + HTTPS

> 状态：v1.0-C 初测。
> 入口：`https://shenjuntao.com/`
> 镜像：`sha-3aef82ee12ed1fb5e0738ed5581a956789c4a7fc`
> 决策关联：v1.0-C 生产接入。

## 环境

```text
客户端：WSL2，跨境 WAN
服务端：阿里云 ECS Ubuntu 22.04，2 vCPU，1.7 GiB
Caddy：2.11.9
容器：4 worker
TLS：TLSv1.3 / TLS_AES_128_GCM_SHA256
协议：HTTP/2
工具：h2load nghttp2/1.68.0
```

## 方法

- 目标使用 `/healthz`，避免 34KB 首页把跨境带宽当成服务容量；
- `h2load -n 3000 -c 32 -m 10`，3 次；
- Caddy CPU 用 `pidstat -u -p $(pgrep -x caddy) 1` 采样；
- 首页 34KB 的首轮尝试在 40% 时停止：WSL 到阿里云单请求约 1.2s，测到的是 WAN 带宽，已标记 invalid。

## 结果

| 轮次 | req/s | 请求平均 | 请求最大 | 2xx | 错误 |
|---|---:|---:|---:|---:|---:|
| 1 | 2100.55 | 106.89ms | 525.87ms | 3000 | 0 |
| 2 | 1712.97 | 128.55ms | 1.10s | 3000 | 0 |
| 3 | 1727.25 | 124.18ms | 1.02s | 3000 | 0 |

Caddy CPU 采样：

```text
43% – 67%（2 vCPU）
```

`h2load` 输出的是请求平均值/最大值，不是 P99；这里不伪造分位数。

## 解读

- 小响应经 Caddy + TLS + 跨境 WAN，约 1.7k–2.1k req/s；
- Caddy CPU 43%–67%，是 TLS 和连接处理的主要成本；
- 首页 34KB 的 1.2s 单请求延迟是 WAN 限制，不是 origin 容量结论；
- 这组数据只用于 C 阶段生产链路观察，不和实验室直连 A/B 数据混用。

## 原始数据

```text
raw/2026-11-26-production/h2load-healthz.log
raw/2026-11-26-production/caddy-pidstat.log
raw/2026-11-26-production/environment.txt
raw/2026-11-26-production/README.md
```

首轮首页测试保留在 `h2load.log`，标注为 invalid。

## 下一步

- 需要更可信的容量数据时，换同地域客户端或内网压测；
- C 阶段完成后打 `v1.0.0-rc.1`。
