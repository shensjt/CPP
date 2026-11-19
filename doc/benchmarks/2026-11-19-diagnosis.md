# 2026-11-19 诊断

> 状态：诊断数据。`strace` 会显著降低 QPS，只看 syscall 次数和比例，不用它的 QPS 下结论。
> commit：`8a9eecf`（server 代码 `ee1f418`）

## 1. strace

命令：

```bash
strace -c -f -o strace-t4.txt \
  ./build/server/web_server --host 127.0.0.1 --port 18080 \
  --root build/bench-www --threads 4 --timeout 60 --cache-bytes 16777216

wrk -t4 -c100 -d5s --latency http://127.0.0.1:18080/
```

strace 下 QPS 约 2.6k，明显低于正常值；这是 tracing 开销，不是性能结论。

### syscall 统计

| syscall | calls | 每请求约 | 说明 |
|---|---:|---:|---|
| `readlink` | 65,000 | 5 | 全部失败，`weakly_canonical` 的符号链接检查 |
| `newfstatat` | 52,001 | 4 | 目录/普通文件/`stat` 多次 |
| `readv` | 26,178 | 2 | 读请求 |
| `epoll_ctl` | 13,204 | 1 | `EPOLLONESHOT` rearm |
| `sendto` | 12,999 | 1 | 写响应 |
| `faccessat2` | 12,999 | 1 | 路径/文件检查 |
| `futex` | 663 | <1 | 锁/条件变量，strace 下时间不可直接比较 |

请求数约 13,000。

### 结论

- 每请求约 5 次 `readlink`，而且全部失败；
- 每请求约 4 次 `newfstatat`；
- `epoll_ctl` 只有约 1 次/请求；
- 路径解析和文件元数据检查是 worker `%system` 的主要来源；
- 主从 Reactor 只能优化 `epoll_ctl`/IO 分发这一小部分，不能消除 readlink/stat。

## 2. 客户端 CPU

strace 运行期间：

```text
wrk %CPU 约 22.7%
```

这个数字受 strace 降速影响，不能代表正常 20k QPS 时的客户端压力；完整基线需要单独采样 `wrk` CPU。

## 3. 补充实验

| 配置 | QPS | 说明 |
|---|---:|---|
| 8 worker，cache16 | 20,303 | pilot |
| 8 worker，cache0 | 15,570 | 关闭缓存后下降，说明缓存有效 |
| 4 worker，50 conns | 19,666 | 与 100/200 接近 |
| 4 worker，100 conns | 19,538 | pilot |
| 4 worker，200 conns | 20,151 | 只涨延迟 |

结论：

- 连接数从 50 到 200，QPS 基本不变，服务端已饱和；
- cache 关闭后 8 worker 反而低于 4 worker，说明缓存关闭时更多线程在争抢文件/路径工作；
- 增加 worker 不是当前主要杠杆。

## 4. 原始数据

```text
raw/2026-11-19-diagnosis/strace-t4.txt
raw/2026-11-19-diagnosis/strace-wrk.log
raw/2026-11-19-diagnosis/wrk-cpu.log
raw/2026-11-19-diagnosis/strace-server.log
raw/2026-11-19-diagnosis/http-t8-cache0/
raw/2026-11-19-diagnosis/http-t4-c50/
raw/2026-11-19-diagnosis/http-t4-c200/
```

## 5. 关联

- [pilot](2026-11-19-pilot.md)
- [ADR-0003](../decisions/0003-request-path-before-reactor.md)
