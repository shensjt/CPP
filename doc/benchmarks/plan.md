# 性能测试方案（v1.0-A 基线）

> 目标：测当前单 Reactor + 线程池架构，回答 IO 线程是否饱和、B 阶段是否值得做。
> 环境：WSL2 / Ubuntu，客户端与服务端同机，数字只做相对比较。

## 1. 工具

| 用途 | 工具 |
|---|---|
| HTTP | `wrk` |
| KV SET/GET | `redis-benchmark` |
| KV MIXED | `memtier_benchmark` |
| CPU / RSS / context switch | `pidstat` |
| HTTPS / HTTP2（后续 C/D） | `h2load` |

当前版本：

```text
wrk 4.1.0
redis-benchmark 8.0.5
memtier_benchmark 2.5.1
pidstat 12.7.7
h2load 1.68.0
```

`bench_kv` 已移除，不作为基准工具。

## 2. 环境记录

每份数据必须包含：

```text
机器 / CPU / 内存 / 内核
客户端与服务端是否同机
编译器 / 编译选项
工具及版本
被测 commit / tag
server 参数
运行命令
```

`scripts/bench_http.sh` 和 `scripts/bench_kv.sh` 会自动写 `environment.txt`。

## 3. HTTP 矩阵

| 维度 | 取值 |
|---|---|
| server threads | 1 / 2 / 4 / 8 |
| 连接 | keep-alive / close |
| 文件 | `/`（约 1.35KB）/ `/1mb.bin` |
| LRU | 16MB / 0 |
| 版本 | v0.2-kv / v0.3.0 / v0.5.0（连接锁对比） |

命令：

```bash
THREADS=4 CACHE_BYTES=16777216 RUNS=3 ./scripts/bench_http.sh
THREADS=4 CACHE_BYTES=0 RUNS=3 ./scripts/bench_http.sh
THREADS=4 PATH_URL=/1mb.bin CONNECTIONS=32 RUNS=3 ./scripts/bench_http.sh
KEEP_ALIVE=0 THREADS=4 RUNS=3 ./scripts/bench_http.sh
```

底层：

```bash
wrk -t4 -c100 -d30s --latency http://127.0.0.1:18080/
```

关闭 keep-alive：

```bash
wrk -t4 -c100 -d30s --latency -H "Connection: close" http://127.0.0.1:18080/
```

采集：

```text
QPS
P50 / P90 / P99
非 2xx
RSS
IO / worker CPU
context switch
```

`pidstat -u -t -r -w -p <pid> 1` 输出到 `pidstat.log`。

## 4. KV 矩阵

| 维度 | 取值 |
|---|---|
| 模式 | SET / GET / MIXED |
| clients | 1 / 4 / 16 / 128 |
| AOF | 关闭 / everysec |
| keys | 10000 |
| value size | 64 |

命令：

```bash
MODE=set   CLIENTS=128 REQUESTS=200000 AOF=off      RUNS=3 ./scripts/bench_kv.sh
MODE=get   CLIENTS=128 REQUESTS=200000 AOF=off      RUNS=3 ./scripts/bench_kv.sh
MODE=mixed CLIENTS=128 REQUESTS=200000 AOF=off      RUNS=3 ./scripts/bench_kv.sh
MODE=set   CLIENTS=128 REQUESTS=200000 AOF=everysec RUNS=3 ./scripts/bench_kv.sh
```

### SET / GET

```bash
redis-benchmark -h 127.0.0.1 -p 16380 \
  -n 200000 -c 128 -d 64 -r 10000 \
  -t set,get
```

`redis-benchmark` 会打印：

```text
WARNING: Could not fetch server CONFIG
```

KV 不支持 `CONFIG`，这是预期，不影响计时。

### MIXED

```bash
memtier_benchmark -s 127.0.0.1 -p 16380 --protocol=redis \
  --threads=4 --clients=32 \
  --ratio=1:1 --key-pattern=R:R \
  --key-minimum=1 --key-maximum=10000 \
  --requests=1600 \
  --hide-histogram --print-percentiles=50,90,99
```

**必须先预热**，否则 GET 100% miss：

```bash
memtier_benchmark -s 127.0.0.1 -p 16380 --protocol=redis \
  --threads=1 --clients=1 --ratio=1:0 --key-pattern=S:S \
  --key-minimum=1 --key-maximum=10000 \
  --requests=10000 --hide-histogram
```

`scripts/bench_kv.sh` 已包含预热步骤。

## 5. 指标

```text
QPS
P50 / P90 / P99
错误数
RSS
每线程 CPU（io / worker-N）
context switch
AOF 文件增长
AOF 回放耗时
```

## 6. 采集方式

- 每组 3 次，取中位数；
- 每次重启 server，避免缓存/连接状态跨组污染；
- 客户端与服务端同机，记录客户端 CPU 占用；
- `wrk` / `redis-benchmark` / `memtier` 的输出原样保存到 `doc/benchmarks/raw/`；
- `pidstat` 采样间隔 1s，保存到同一目录。

## 7. 结论

必须回答：

```text
1. 当前架构的 QPS / P50 / P99
2. IO 线程是否饱和
3. request-path 优化是否值得做
4. B4 主从 Reactor 是否值得做
5. sendfile 是否需要评估
```

IO 线程饱和的判断：

```text
IO 线程 CPU 接近 100%
worker CPU 大量空闲
QPS 随 worker 增加不再提升
P99 被 IO 线程拖高
```

## 8. 验收

```text
1. 每组有环境、命令、3 次原始输出、中位数
2. 数据与 v0.5.0 代码一致
3. 有 IO 线程饱和度结论
4. 有 B 阶段建议：做 / 不做 / 只评估
5. 打 tag v1.0.0-alpha.1
```

## 9. 边界

- 实验室数据直连 `127.0.0.1`，不经过 Caddy/HTTPS；
- 生产边缘数据在 C/D 阶段单独测；
- 不为了数字好看换工具或改请求。

## 10. 数据记录

每个实验在 `doc/benchmarks/raw/<日期>-<实验>/` 保存：

```text
environment.txt
command.txt
server.log
wrk.log / redis-benchmark.log / memtier.log
pidstat.log
aof.txt / replay.txt（AOF 实验）
```

- 3 次原始输出都保留，summary 写中位数；
- 不提交 AOF 二进制；
- `strace` 放诊断目录，标注 tracing 会降低 QPS；
- 失败运行保留并标注 `invalid`；
- summary 关联 ADR。

## 11. 关联决策

- [ADR-0001](../decisions/0001-mature-benchmark-tools.md)：成熟工具
- [ADR-0002](../decisions/0002-baseline-before-optimization.md)：先基线
- [ADR-0003](../decisions/0003-request-path-before-reactor.md)：request-path 优先
