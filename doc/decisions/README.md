# 架构决策记录

一决策一文件。Accepted 之后不回头改；结论变化时新增 ADR，并在新 ADR 里写 `Supersedes`。

| ID | 日期 | 状态 | 决策 |
|---|---|---|---|
| [0001](0001-mature-benchmark-tools.md) | 2026-11-19 | Accepted | 压测使用成熟工具，移除 bench_kv |
| [0002](0002-baseline-before-optimization.md) | 2026-11-19 | Accepted | 先基线，再优化 |
| [0003](0003-request-path-before-reactor.md) | 2026-11-19 | Accepted | 先请求路径优化，暂缓主从 Reactor |
| [0004](0004-b1-path-cache-not-merged.md) | 2026-11-20 | Accepted | B1 路径缓存不合并 |
| [0005](0005-b2-shared-body-accepted.md) | 2026-11-20 | Accepted | B2 共享 body 采纳 |

## 模板

```markdown
# ADR-NNNN：标题

- 状态：Proposed / Accepted / Superseded
- 日期：YYYY-MM-DD
- 关联：doc/benchmarks/...
- 取代：无

## 背景

## 证据

## 决策

## 备选

## 后果

## 复评条件
```
