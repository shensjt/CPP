# ADR-0001：压测使用成熟工具

- 状态：Accepted
- 日期：2026-11-19
- 关联：[benchmarks/plan.md](../benchmarks/plan.md)、`scripts/bench_http.sh`、`scripts/bench_kv.sh`
- 取代：无

## 背景

KV 原来使用自研 `bench_kv`：

- 只统计 QPS，没有延迟分位数；
- 混合读写比例固定，不能配置；
- 同机压测时客户端容易成为瓶颈；
- 数据无法和标准工具交叉验证。

Web 也没有正式压测客户端。

## 决策

使用成熟工具：

| 用途 | 工具 |
|---|---|
| HTTP | `wrk` |
| KV SET/GET | `redis-benchmark` |
| KV MIXED | `memtier_benchmark` |
| CPU / RSS / context switch | `pidstat` |
| HTTPS / HTTP2（后续） | `h2load` |

- 删除 `bench_kv` 源码、CMake target、Makefile target；
- README 不再保留 `bench_kv` 产生的旧数据；
- 脚本只负责编排工具和保存输出，不自己发请求。

## 备选

- 继续维护 `bench_kv`：否决，测量能力不足；
- 自研 `bench_http`：否决，成熟工具更可靠；
- 只用 `redis-benchmark`：否决，无法做 MIXED。

## 后果

- 本地需要安装 `wrk`、`sysstat`、`redis-tools`、`memtier`；
- `redis-benchmark` 会提示 `Could not fetch server CONFIG`，这是预期；
- `memtier` 做 MIXED 前必须先预热 key 空间；
- 旧 README 的 KV 数字不可复现，已移除；
- 原始数据统一放 `doc/benchmarks/raw/`。

## 证据

工具版本：

```text
wrk 4.1.0
redis-benchmark 8.0.5
memtier_benchmark 2.5.1
pidstat 12.7.7
h2load 1.68.0
```

兼容性 smoke：`wrk`、`redis-benchmark`、`memtier` 均可对当前 server 发请求；`memtier` 预热后 GET miss 为 0。
