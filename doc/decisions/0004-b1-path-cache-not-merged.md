# ADR-0004：B1 路径缓存不合并

- 状态：Accepted（2026-11-20）
- 日期：2026-11-20
- 关联：[B1 数据](../benchmarks/2026-11-20-b1-path.md)
- 取代：无

## 背景

ADR-0003 决定先做 request-path 优化。B1 实现：

- URL -> 规范化路径的 1MB LRU；
- 单次 `lstat` 替代 `is_directory` / `is_regular_file` / `stat`；
- 符号链接替换时重新规范化并做 root 检查。

## 证据

B1 的 syscall 明显减少：

```text
readlink      0        （baseline 约 5/请求）
newfstatat    2/请求   （baseline 约 4/请求）
```

worker CPU 下降约 10%。

但 interleaved A/B（各 3 次 × 30s）：

```text
t4 baseline 18,761 QPS，B1 18,366 QPS
t8 baseline 19,055 QPS，B1 19,919 QPS
```

QPS / P99 波动大，没有稳定的 QPS +20% 或 P99 -30% 收益。

## 决策

B1 **不合并**到 `fast-track`：

- 保留 `feat/v1.0-b1-path` 分支和代码作为实验记录；
- v1.0-B 继续 B2：去掉 LRU/Response body 拷贝；
- B2 以 B0 为基线，不和 B1 混在一起。

## 备选

- 合并 B1：否决，收益不稳定，且增加路径缓存的安全面；
- 合并 B1 再测 B2：否决，无法区分收益来源；
- 丢弃 B1 代码：否决，保留分支供后续复评。

## 后果

- 主分支不引入路径缓存；
- B1 的 readlink/stat 数据保留在 benchmark；
- 如果后续 CPU 成为瓶颈，可以重新评估 B1；
- B2 直接针对 1MB 文件的 body 拷贝。

## 复评条件

- B2/B3 完成后，如果 worker CPU 再次成为主要瓶颈；
- 或连接锁/客户端瓶颈被排除后，路径解析重新成为热点。
