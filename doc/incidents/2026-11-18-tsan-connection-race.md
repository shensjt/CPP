# INC-2026-11-18-01：TcpServer 连接处理的所有权交接竞态

| 项 | 值 |
|---|---|
| 日期 | 2026-11-18 |
| 状态 | 已修复，待合并 |
| 严重级别 | 中（核心路径潜在数据竞争，未观测到线上故障） |
| 影响范围 | `common/net/tcp_server.cc`、`common/net/tcp_connection.h` |
| 首次引入 | `v0.1-demo` |
| 发现方式 | CI 的 TSan job |
| 修复分支 | `chore/ci-sanitizers` |
| 修复提交 | `fix(net): 修复 TcpServer 连接处理的所有权交接竞态` |

## 摘要

`TcpServer` 在线程池模式下，同一个 `TcpConnection` 可能被两个 worker 同时处理，导致对连接读写 Buffer 的无同步并发访问。TSan 报出 2 处数据竞争，`net_tests` 与 `kv_tests` 失败。根因有两层：`Rearm` 早于 `InFlight` 清除，以及事件所有权交接缺少内存序。修复方式是调整 `Rearm` 顺序，并为每条连接增加一把处理期间持有的互斥锁。

## 影响

- 潜在影响：同一连接的读 Buffer / 写 Buffer 被并发读写，可能造成响应内容错乱、协议解析异常，严重时进程崩溃。
- 实际影响：未在生产环境观测到故障；缺陷由 CI 的 TSan 在合并前发现。
- 数据影响：无持久化数据损坏的报告。

## 检测

CI 的 `tsan` job 输出：

```text
ThreadSanitizer: reported 2 warnings
67% tests passed, 2 tests failed out of 6
Failed: net_tests, kv_tests
```

本地单次运行 6/6 通过。用 CPU 绑定 + 循环重跑后复现：

```bash
taskset -c 0,1 ./build/tsan/net_tests   # 第 3 轮失败
taskset -c 0,1 ./build/tsan/kv_tests    # 第 43 轮失败
```

## 时间线（2026-11-18）

1. CI 的 TSan job 在 `net_tests`、`kv_tests` 报数据竞争。
2. 本地单次运行通过，判断为时序相关。
3. 使用 `taskset -c 0,1` + 循环重跑复现，拿到完整调用栈。
4. 定位第一层：`Rearm` 早于 `SetInFlight(false)`。
5. 调整顺序后，`kv_tests` 100 轮通过；`net_tests` 仍有 1 处报告。
6. 定位第二层：`Rearm` 读 `write_buffer_` 与下一个 worker 写同一 Buffer 之间没有内存序。
7. 增加连接级互斥锁，串行化事件处理、重新注册和关闭。
8. 验证：TSan `net_tests` 30 轮、`kv_tests` 100 轮零警告；ASan 与 `make verify` 全绿。

## 根因

### 第一层：重新注册早于所有权释放

线程池模式下，worker 处理完事件后调用 `Rearm()`，把 fd 重新注册到 epoll。原实现把 `Rearm()` 放在 `HandleEvents()` 末尾，此时 `InFlight` 仍为 `true`：

```text
worker A: HandleEvents → Rearm() → epoll_ctl(MOD)
          │                          fd 重新激活
          │                          ↓
IO 线程:  │                      新事件到达 → Post worker B
          ↓
worker A: SetInFlight(false)
```

`EPOLLONESHOT` 只保证“一个事件只投递一次”，不保证“上一个 worker 已经退出”。`epoll_ctl(MOD)` 之后、`SetInFlight(false)` 之前，IO 线程可以派发下一个 worker。

TSan 证据：

```text
worker A: Buffer::ReadFd            （写 read_buffer_）
worker B: resp::Parser::Parse       （读 read_buffer_）
```

### 第二层：交接缺少内存序

把 `Rearm` 移到 `SetInFlight(false)` 之后，两个 worker 不再同时进入 `HandleEvents`。但 `Rearm()` 内部要读 `write_buffer_` 决定是否关注 `EPOLLOUT`：

```text
worker A: Rearm → UpdateInterest → Buffer::ReadableBytes   （读）
worker B: Send  → Append        → Buffer::HasWritten        （写）
```

epoll 的事件通知不是 C++ 的同步原语，不建立 happens-before 关系。只要下一个 worker 在 `epoll_ctl` 之后启动，它和上一个 worker 的 `Rearm` 就构成数据竞争。

TSan 证据：

```text
SUMMARY: ThreadSanitizer: data race buffer.cc:40 in Buffer::HasWritten(unsigned long)
SUMMARY: ThreadSanitizer: data race buffer.cc:15 in Buffer::ReadableBytes() const
```

## 修复

### 1. 调整重新注册顺序

`HandleEvents` 不再负责 `Rearm`。worker 任务按以下顺序执行：

```text
HandleEvents → SetInFlight(false) → Rearm
```

这样 fd 重新激活时，所有权已经交还。

### 2. 每条连接一把锁

`TcpConnection` 增加 `std::recursive_mutex`。worker 在整个 `HandleEvents + SetInFlight + Rearm` 期间持有；`CloseConnection` 在关闭连接前持有同一把锁。

- 用 `recursive_mutex` 是因为关闭路径可能从 `HandleEvents` 内部调用 `CloseConnection`，同一线程会重入；
- 锁只在连接处理期间持有，正常情况下无竞争；
- 代价是每个事件多一次无竞争加解锁，后续用压测量化。

## 验证

| 检查 | 结果 |
|---|---|
| TSan 全量 ctest | 6/6 通过 |
| TSan `net_tests` 循环 | 30 轮零警告 |
| TSan `kv_tests` 循环 | 100 轮零警告 |
| ASan/UBSan 全量 | 6/6 通过 |
| `make verify` | 6/6 通过，博客构建成功 |

## 为什么之前没发现

- 竞争窗口很小，单次运行大概率不触发；
- 本地机器与 CI runner 的调度不同，CI 更容易撞上；
- 之前的 TSan 只跑一次，没有重复压测；
- TSan 报告只在失败时保留，平时不会主动暴露。

## 行动项

| # | 动作 | 目的 | 状态 |
|---|---|---|---|
| 1 | 把 ASan/TSan 加入 CI | 合并前发现此类问题 | 已完成 |
| 2 | CI 中对 `net_tests`、`kv_tests` 重复运行 TSan | 降低偶发漏检 | 待做 |
| 3 | 压测 Echo，量化每连接锁的开销 | 确认性能代价 | 待做 |
| 4 | 记录本案例到 `doc/incidents/` | 形成可复用经验 | 已完成 |
| 5 | 评估无锁交接方案 | 仅当锁开销显著时考虑 | 可选 |

## 经验教训

1. **`EPOLLONESHOT` 不等于线程安全。** 它解决“事件重复投递”，不解决“所有权交接”的内存序。
2. **单次 TSan 通过不代表没有竞态。** 并发缺陷需要重复运行和不同调度。
3. **先复现，再修复。** 用 CPU 绑定和循环把偶发变成可重复，才能验证修复。
4. **正确性优先，性能其次。** 先保证串行化正确，再用数据决定是否优化。
5. **CI 的价值不只是“跑测试”，而是用不同环境暴露本地看不到的问题。**

## 关联

- 代码：`common/net/tcp_server.cc`、`common/net/tcp_connection.h`
- 后续同类问题：[INC-2026-11-18-02 fd 复用导致的 HttpParser UAF](2026-11-18-fd-reuse-parser-uaf.md)
- 发布记录：[v0.1-demo](../releases/v0.1-demo.md)
