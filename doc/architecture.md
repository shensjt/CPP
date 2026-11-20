# 系统架构

> 更新：2026-11-18
> 适用范围：`main` 分支与当前线上容器部署

## 1. 系统边界与部署

两个可执行程序，共享 `common/`：

- `web_server`：静态 HTTP 文件服务；
- `kv_store`：RESP 协议的 KV 存储。

当前生产拓扑：

```text
浏览器
  │ HTTP/1.1 :8080
  ▼
web_server 容器（0.0.0.0:8080）
  └── /app/www
        ▲ 只读挂载
        │
/opt/sjtweb-runtime/www
```

- 容器由 Docker Compose 管理，运行目录 `/opt/sjtweb-runtime`；
- 旧 systemd unit 已 stop / disable；旧 `deploy/` 脚本已删除；
- 当前没有域名和 TLS，Caddy 未安装；
- 镜像从阿里云 ACR 拉取，tag 为 `sha-<commit>`。

## 2. 代码分层

| 路径 | 职责 |
|---|---|
| `common/net/` | `Epoll`、`TcpServer`、`TcpConnection`、`Buffer` |
| `common/thread_pool/` | 线程池与阻塞队列 |
| `common/log/` | 双缓冲异步日志 |
| `common/memory_pool/` | 固定大小分桶内存池（独立组件） |
| `projects/web_server/http/` | HTTP 解析、响应、静态文件、`HttpServer` |
| `projects/web_server/lru_cache/` | 线程安全 LRU |
| `projects/kv_store/skiplist/` | 跳表 |
| `projects/kv_store/store/` | `KVStore`、`ZSet`、score/member 编码 |
| `projects/kv_store/protocol/` | RESP 解析与序列化 |
| `projects/kv_store/command/` | 命令分发与参数校验 |
| `projects/kv_store/aof/` | AOF 追加、fsync、回放、尾部修复 |
| `projects/kv_store/server/` | `KvServer` 接入网络层 |
| `tests/` | 6 个测试可执行文件、206 个用例 |
| `blog/` | MkDocs 内容站配置、演示内容和发布配置 |

## 3. 线程与并发模型

### Web Server

```text
IO 线程
  epoll_wait
    → accept
    → 连接可读
    → InFlight = true
    → Post 到线程池

worker
  HandleRead / 回调 / Send
  → InFlight = false
  → Rearm
```

- 单 IO 线程负责 `epoll_wait`、accept 和分发；
- 连接注册 `EPOLLONESHOT`，一次事件只投递一个 worker；
- `InFlight` 标记连接是否正在处理，空闲扫描据此跳过；
- 每条连接有一把 `recursive_mutex`，覆盖 `HandleEvents + Rearm` 和关闭路径；
- 关闭顺序：从连接表移除 → `Close()`（epoll Del，fd 暂存）→ `close_callback_` → `CloseFd()`；
- 连接表由 `connections_mutex_` 保护，worker 持有 `shared_ptr` 保证生命周期；
- 主线程用 `sigtimedwait` 处理 SIGINT/SIGTERM；`Stop()` 后 Start 返回，worker join，最后刷新日志。

### KV Store

- 复用同一套 `TcpServer` / `TcpConnection` / `Buffer`；
- `KVStore` 使用 `std::shared_mutex`：读读并行，写写串行；
- AOF 在写锁内追加，后台 everysec fsync；
- `sigwait` 优雅停机：worker 退出后再停 AOF。

## 4. 数据流

### HTTP

```text
accept
  → EPOLLIN
  → TcpConnection::HandleRead
  → Buffer
  → HttpParser 状态机
  → StaticFileHandler（LruCache）
  → HttpResponse
  → TcpConnection::Send
  → Rearm
```

- 每个连接一个 `HttpParser`，支持 keep-alive 和流水线；
- 静态文件服务做 URL 解码、路径规范化、root 范围校验；
- 静态文件响应带 `ETag` / `Last-Modified` / `Cache-Control`，条件请求返回 304；
- 请求限额：request line 8KB、Headers 32KB、Body 1MB，Body 超限返回 413；
- `/healthz` 返回 status 和编译期注入的 commit；
- 访问日志记录 method/path/status/bytes/duration；
- 响应统一生成 `Content-Length` 和 `Connection`；
- `CloseAfterWrite` 保证响应发完再关闭。

### KV

```text
RESP
  → resp::Parser
  → CommandHandler
  → KVStore（shared_mutex）
  → ZSet / SkipList
  → AOF 追加
  → RESP 响应
```

- `resp::Parser` 无状态，半包留在 Buffer；
- 字符串使用 `unordered_map`，ZSet 使用 dict + 跳表；
- AOF 使用 RESP 编码，启动时 replay，尾部截断可修复。

## 5. 关键决策

### 5.1 单 Reactor + 线程池

实现简单，`EPOLLONESHOT` 保证同一 fd 同一时刻只有一个 worker。代价是单 IO 线程在连接数和事件数很大时成为瓶颈，且 worker 同时做 IO 和业务。

### 5.2 EPOLLONESHOT + 连接级锁

`EPOLLONESHOT` 只保证事件投递互斥，不保证所有权交接的内存序。连接级锁补齐了 `Rearm` 和下一个 worker 之间的交接，关闭路径也使用同一把锁。相关故障记录：

- [2026-11-18 TSan 连接所有权竞态](incidents/2026-11-18-tsan-connection-race.md)
- [2026-11-18 fd 复用导致的 HttpParser UAF](incidents/2026-11-18-fd-reuse-parser-uaf.md)

### 5.3 构建产物 `build/www`

HTML 构建产物统一放在 `build/`，不写源码树。`make blog` 生成 `build/www`，`make verify` 使用独立的 `build/verify-www`，避免覆盖本地开发站点。

### 5.4 容器运行

- 多阶段 Dockerfile：builder 编译并生成 `www`，runtime 只保留二进制和 `www`；
- 非 root 用户 `app`（UID 10001）；
- 容器内监听 `0.0.0.0:8080`；
- 内容通过 `/opt/sjtweb-runtime/www` 只读挂载，代码发布和内容发布解耦；
- 内容由本地 MkDocs 构建，`make publish` 通过 rsync 发布，不重启容器；
- 当前没有 Caddy / 域名 / TLS，宿主机直接发布 `8080`。

### 5.5 ACR 与部署

- CI/CD 使用 `sha-<commit>` 作为镜像 tag，不使用 `latest`；
- CD 在 `main` 合并后构建、推送 ACR、SSH 部署；
- 部署前备份 `.env` 为 `.env.prev`，smoke 失败自动恢复；
- ACR 认证从 GitHub runner 偶发 `connection reset`，重跑 deploy job。

### 5.6 AOF everysec

写路径追加 AOF，后台每秒 fsync。吞吐优于 always，代价是崩溃最多丢约 1 秒数据。

### 5.7 缓存一致性

LRU 缓存项保存 `body + mtime + size`，命中后 `stat` 比对当前文件；不一致按 miss 处理。静态响应带 `ETag` / `Last-Modified` / `Cache-Control`，条件请求返回 304。内容更新后不需要重启容器。

### 5.8 可观测性

编译期通过 `BUILD_COMMIT` 注入 commit；`/healthz` 返回 `status` 和 `commit`；访问日志记录 method/path/status/bytes/duration；日志按大小滚动并保留 5 份。CD smoke 校验 `/healthz` 的 commit，不匹配自动回滚。当前没有 metrics、告警和链路追踪。

## 6. 已知限制

Web Server：

- 单 IO 线程，没有多 Reactor；
- 没有 sendfile，大文件走用户态缓冲；
- 没有限流、连接数上限和认证；
- 内存池未接入 Web Server；
- 读缓冲 2MB、写缓冲 8MB，超限直接关闭连接；没有完整暂停/恢复背压。

KV Store：

- AOF 没有重写，文件持续增长；
- `ZRANGE` 按名次是 O(N)；
- 没有 key 过期、淘汰、事务和集群；
- AOF everysec 崩溃最多丢约 1 秒数据。

部署：

- 没有域名和 TLS，直接 HTTP 8080；
- 有 `/healthz` 和访问日志，但没有 metrics / 告警 / 链路追踪；
- CD smoke 检查 `/`、`/sitemap.xml` 和 `/healthz` 的 commit；
- 内容发布是本地脚本：构建、快照 `www.prev`、rsync、smoke、失败回滚；
- 内容只有一份 `www.prev` 快照，没有 push-to-publish。

## 7. 性能与瓶颈

v1.0-A 基线已完成：见 [2026-11-20-baseline.md](benchmarks/2026-11-20-baseline.md)。

结论：

- IO 线程 15.6%–28.3%，没有饱和；
- 2 → 4 worker QPS 基本不变；
- 4 → 8 worker QPS 只从 16.7k 到 19.1k，P99 从 26ms 恶化到 120ms；
- 1MB 文件 645 req/s，body 拷贝和内核发送是主要成本；
- 每请求约 5 次 `readlink`、4 次 `newfstatat`、1 次 `epoll_ctl`。

决策：[ADR-0003](decisions/0003-request-path-before-reactor.md) Accepted，先做 request-path 优化；主从 Reactor 作为 B4 条件项，暂缓到 v1.1。

## 8. 入口

- 项目说明：[README.md](../README.md)
- 内容流水线：[blog-pipeline.md](blog-pipeline.md)
- 部署与回滚：[runbooks/deploy.md](runbooks/deploy.md)、[runbooks/rollback.md](runbooks/rollback.md)
- 内容发布：[runbooks/publish-content.md](runbooks/publish-content.md)
- 发布记录：[releases/](releases/)
- 故障复盘：[incidents/](incidents/)
