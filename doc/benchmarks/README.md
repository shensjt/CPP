# 性能数据

本目录保存可复现的压测数据和结论。数据是决策依据，不能只写结论。

## 记录规则

- 每个实验一个目录：`raw/<日期>-<实验>/`
- 每个配置保存：
  - `environment.txt`：机器、commit、工具版本、server 参数
  - `wrk.log` / `redis-benchmark.log` / `memtier.log`
  - `pidstat.log`
  - `server.log`
- 每组 3 次，summary 写中位数，原始输出放 raw；
- 不提交 AOF 二进制，只提交 `aof.txt` / `replay.txt`；
- `strace` 只用于诊断，保留原始输出，不用它的 QPS 下结论；
- 失败的运行不删除，在 summary 标注 `invalid`；
- 每份 summary 关联对应的 ADR。

## 目录

```text
benchmarks/
├── README.md
├── plan.md
├── 2026-11-19-pilot.md
├── 2026-11-19-diagnosis.md
└── raw/
    ├── 2026-11-19-pilot/
    └── 2026-11-19-diagnosis/
```

## 索引

| 日期 | 类型 | 状态 | commit | 结论 |
|---|---|---|---|---|
| 2026-11-19 | pilot | 非最终 | `8a9eecf` | IO 线程未饱和；4→8 worker QPS 不涨 |
| 2026-11-19 | diagnosis | 诊断 | `8a9eecf` | 每请求 readlink ~5、newfstatat ~4、epoll_ctl ~1 |
| 2026-11-20 | baseline | **正式** | `ee1f418` | IO 15.6–28.3%；4→8 worker QPS +15%，P99 恶化；先 request-path |
| 2026-11-20 | B1 path cache | 已评估，不合并 | B1 分支 | readlink 0、stat 2/req；QPS/P99 收益不稳定 |

A 阶段基线已完成，见 [2026-11-20-baseline.md](2026-11-20-baseline.md)。

B1 已评估但不合并，见 [2026-11-20-b1-path.md](2026-11-20-b1-path.md)。
