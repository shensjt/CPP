# ADR-0003：先请求路径优化，暂缓主从 Reactor

- 状态：Accepted（2026-11-20）
- 日期：2026-11-19
- 关联：[基线](../benchmarks/2026-11-20-baseline.md)、[pilot](../benchmarks/2026-11-19-pilot.md)、[诊断](../benchmarks/2026-11-19-diagnosis.md)
- 取代：无

## 背景

原 v1.0-B 计划做 A0 / B1 / B2 主从 Reactor 对比。pilot 和 strace 显示：

- 4 worker 时 IO 线程约 25%，没有饱和；
- 4 → 8 worker，QPS 从 19.5k 到 20.3k，几乎不涨；
- 每请求约 5 次 `readlink`、4 次 `newfstatat`、1 次 `epoll_ctl`；
- 1MB 文件约 600 req/s，body 拷贝和内核 send 成本明显。

主从 Reactor 解决的是 IO 线程/事件分发瓶颈；当前证据不支持它是主要瓶颈。

## 证据

正式基线（3 次 × 30s，中位数）：

```text
IO 线程：15.6%（1 worker）– 28.3%（8 worker）
2 worker：16,615 QPS，P99 16.90ms
4 worker：16,696 QPS，P99 26.19ms
8 worker：19,142 QPS，P99 120.27ms
4 worker cache0：16,712 QPS，worker 93.5%
4 worker cache16：16,696 QPS，worker 73.0%
1MB：645 req/s
close：9,244 QPS，io 53.4%
```

strace（每请求）：

```text
readlink ~5
newfstatat ~4
epoll_ctl ~1
```

详见：

- [正式基线](../benchmarks/2026-11-20-baseline.md)
- [pilot](../benchmarks/2026-11-19-pilot.md)
- [诊断](../benchmarks/2026-11-19-diagnosis.md)

## 决策

v1.0-B 改为 request-path A/B，主从 Reactor 暂缓：

| 方案 | 内容 |
|---|---|
| B0 | 当前实现 |
| B1 | 缓存规范化路径 + 单次 stat |
| B2 | 避免 LRU/Response body 拷贝，响应直接拼 buffer |
| B3 | 大文件 sendfile A/B |
| B4 | 主从 Reactor，仅在 B1/B2 后 IO/epoll 仍是热点时做 |

顺序：

```text
B1 → B2 → B3 → 复测
  → 如果 IO/epoll 仍高，再做 B4
  → 否则主从 Reactor 移到 v1.1
```

## 备选

- 直接做主从 Reactor：证据不支持，否决；
- 只做 sendfile：只覆盖大文件，小文件路径 syscall 仍在；
- 保持现状：1MB 和 20k QPS 的瓶颈仍在，不采纳。

## 后果

- v1.0-B 的计划和设计需要从 reactor 改成 request-path A/B；
- 主从 Reactor 移到 v1.1，v1.0 保留稳定实现；
- 路径缓存必须保留 root/符号链接安全语义，B1 设计里单独说明；
- body 拷贝优化会改动 `LruCache` 使用方式，需要测试覆盖。

## 复评条件

完整 A 矩阵（3 次 × 30s）已完成：

- IO 线程仍明显低于 100%，`epoll_ctl` 不是主要 syscall；
- 本条已改为 Accepted；
- 主从 Reactor 推迟到 v1.1。

B4 仍保留为条件项：如果 B1/B2 后 IO 线程接近 100%，或 `epoll_ctl` 成为主要热点，再按 B4 做 A/B。
