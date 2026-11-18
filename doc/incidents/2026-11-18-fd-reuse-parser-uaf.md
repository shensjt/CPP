# INC-2026-11-18-02：fd 复用导致的 HttpParser UAF

| 项 | 值 |
|---|---|
| 日期 | 2026-11-18 |
| 状态 | 已修复，待合并 |
| 严重级别 | 高（use-after-free / 堆损坏） |
| 影响范围 | `common/net/tcp_server.cc`、`common/net/tcp_connection.*`、`projects/web_server/http/http_server.cc` |
| 首次引入 | `v0.1-demo` |
| 发现方式 | `fast-track` 合并后的 TSan CI；本地循环压测复现 |
| 修复分支 | `fix/fd-reuse-parser-uaf` |

## 摘要

旧连接关闭时，`TcpConnection::Close()` 先 `close(fd)`，但此时连接还留在 `TcpServer::connections_` 表里。内核可以立刻把同一个 fd 复用给新连接：新连接的 `emplace` 失败，新连接对象析构时关闭客户端 socket；随后旧连接的 `OnClose(fd)` 又删掉了新连接刚创建的 `HttpParser`，worker 仍在解析该 parser，造成 use-after-free。修复方式是延迟 fd 的 `close`，并把上层 close 回调放在真正关闭 fd 之前。

## 影响

- 客户端偶发 `ECONNRESET` 或 EOF，收不到响应；
- `HttpParser` 被释放后仍被 worker 使用，可能导致堆损坏、崩溃；
- 失败率与并发和调度有关，本地约 1/50–1/200，CI 上偶发；
- 未观测到线上故障，属于潜在缺陷。

## 检测

`fast-track` 合并后的 TSan job 失败。本地用 `HttpServer_ConcurrentClients` 循环复现：

```bash
for i in $(seq 1 500); do
  ./build/verify-tests/web_server_tests HttpServer_ConcurrentClients || break
done
```

失败信息先是：

```text
READ_FAIL errno=104 buffer=[]
```

后来出现 TSan 报告：

```text
WARNING: ThreadSanitizer: data race
  Write ... HttpServer::OnClose(int)
  Previous read ... HttpParser::Parse(...)
```

以及普通构建退出时的 `tcache_thread_shutdown(): unaligned tcache chunk detected`。

## 根因

### 时序

```text
worker A（旧连接 fd=16）
  HandleRead 检测到 EOF
  → conn->Close()
       epoll Del
       ::close(16)          ← fd 立即被内核复用
  → CloseConnection(16)     ← 表项还没删

IO 线程
  accept() 得到 fd=16
  → connections_.emplace(16, new_conn) 失败（旧表项还在）
  → new_conn 析构
       Close() → ::close(16)  ← 关掉了新客户端
```

### 第二层：parser 误删

即使 fd 复用没有立刻发生，`HttpServer` 的 parser 也以 fd 为 key：

```text
CloseConnection(fd)
  → conn->Close()
  → conn->CloseFd()        ← fd 关闭，可被复用
  → close_callback_(fd)
       → OnClose(fd) 根据 fd 删除 parser
```

如果新连接在 `CloseFd()` 和 `OnClose(fd)` 之间复用了同一个 fd，并创建了新的 parser，`OnClose(fd)` 会删掉新连接的 parser。worker 正在 `OnMessage` 里使用它，形成 UAF。

## 修复

1. `TcpConnection::Close()` 只做 `epoll Del` 和状态标记，把 fd 暂存到 `pending_close_fd_`，不立即 `close`；
2. 新增 `TcpConnection::CloseFd()`，由 `TcpServer` 在连接表项移除后调用；
3. `CloseConnection` 的顺序改为：
   ```text
   从 connections_ 移除
   → conn->Close()
   → close_callback_(fd)（清理 parser / 会话）
   → conn->CloseFd()（真正 close）
   ```
4. `HandleAccept` 对 `emplace` 失败做防御处理，避免两个对象拥有同一个 fd；
5. 测试增加失败阶段、fd、端口、errno 的诊断，失败时能直接定位。

## 验证

| 检查 | 结果 |
|---|---|
| 普通构建 `HttpServer_ConcurrentClients` 循环 | 800 轮零失败 |
| TSan `HttpServer_ConcurrentClients` 循环 | 800 轮零失败 |
| 全量 `make verify` | 6/6 通过 |
| ASan/UBSan | 6/6 通过 |
| TSan 全量 ctest | 20 轮全通过 |

## 为什么之前没发现

- fd 复用的窗口很小，单次测试很难撞上；
- parser 以 fd 为 key，问题只在“关闭 + 复用 + 新连接解析”三者重叠时出现；
- 之前的 TSan 只跑一次，没有循环压测；
- 普通构建偶发 EOF，容易被误认为测试本身不稳定。

## 行动项

| # | 动作 | 目的 | 状态 |
|---|---|---|---|
| 1 | 延迟 fd close，回调先于 close | 从根上避免复用窗口 | 已完成 |
| 2 | 并发 HTTP 测试循环压测 | 提高复现概率 | 已完成 |
| 3 | 检查其他以 fd 为 key 的状态表 | 排除同类问题 | 待做 |
| 4 | CI 中对 HTTP 并发用例重复运行 | 降低漏检 | 待做 |

## 经验教训

1. **fd 不是稳定身份。** 它会被内核复用，任何以 fd 为 key 的长期状态都要考虑关闭和复用的顺序。
2. **资源释放顺序是协议。** 先摘表、再回调、最后 close，顺序错了就会让新连接继承旧状态。
3. **偶发失败要放大。** 单次通过不代表正确，循环 + 绑核 + TSan 才能把窗口撞开。
4. **测试要报“失败在哪一步”。** 只有 `failures == 1` 无法定位；加上 connect/send/read/status 分类和 errno，排错成本立刻下降。

## 关联

- 前置竞态：[INC-2026-11-18-01](2026-11-18-tsan-connection-race.md)
- 代码：`common/net/tcp_server.cc`、`common/net/tcp_connection.h`、`common/net/tcp_connection.cc`
- 测试：`tests/test_http_server.cc`
- 发布记录：[v0.1-demo](../releases/v0.1-demo.md)
