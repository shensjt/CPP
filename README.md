# SJT C++ Projects

[![ci](https://github.com/shensjt/CPP/actions/workflows/ci.yml/badge.svg?branch=fast-track)](https://github.com/shensjt/CPP/actions/workflows/ci.yml)

两个共享同一套基础设施的 C++17 项目：

- **SJTWebServer**：epoll + 线程池的高并发静态 Web 服务器，博客站点线上运行
- **KV Store**：Redis 风格的跳表 KV 存储，支持 RESP 协议、读写锁和 AOF 持久化

---

# SJTWebServer

一个从零实现的高并发静态 Web 服务器：C++17 + epoll + 线程池，支持 HTTP/1.1 keep-alive、静态文件、LRU 缓存、异步日志和空闲连接超时。博客站点本身就跑在这台服务器上。

## Web Server · 功能

- 线程池 + 阻塞队列，任务投递与优雅停机
- epoll 事件循环，非阻塞 IO，`EPOLLONESHOT` 下 worker 拥有连接
- 分段 Buffer，`readv` 读取，处理半包/粘包
- HTTP/1.1 请求解析：请求行、Headers、Body 状态机
- 静态文件服务：MIME、目录 index、HEAD、404/403/405/400/413
- keep-alive 与流水线，`CloseAfterWrite` 保证响应发完再关
- LRU 文件缓存（默认 16MB，按 mtime/size 失效）
- HTTP 缓存协商：ETag / Last-Modified / 304
- 请求限额：request line 8KB、Headers 32KB、Body 1MB，超限 413
- 读写缓冲上限：读 2MB / 写 8MB，超限关闭慢连接
- 双缓冲异步日志，everysec fsync，按大小滚动，保留 5 份
- 访问日志：method/path/status/bytes/duration
- `/healthz`：返回 status 和编译期注入的 commit
- 空闲连接超时，默认 60 秒
- 路径穿越、URL 编码绕过、符号链接逃逸防护

## Web Server · 架构

```text
浏览器
  │ HTTP/1.1 39.106.115.254:8080
  ▼
web_server 容器（0.0.0.0:8080）
  ▼
TcpServer
  ├── Epoll             事件通知
  ├── ThreadPool        worker
  └── TcpConnection     fd + 读/写 Buffer
        ├── HttpParser
        ├── StaticFileHandler ── LruCache
        └── HttpResponse
```

- IO 线程只做 `epoll_wait` 和分发
- worker 负责连接的读写、HTTP 解析和静态文件处理
- `EPOLLONESHOT` + 连接级锁，保证事件投递和所有权交接
- 当前没有域名和 TLS，直接 HTTP 8080；Caddy 未安装

## 版本

- `v0.1-demo`：Web Server 可演示快照
- `v0.2-kv`：KV Store（跳表 + RESP + 读写锁 + AOF）

仓库只保存源码、模板、测试、部署配置和一篇演示文章；`build/www/` 是构建产物，不入库。

## 环境要求

- Linux / macOS，C++17 编译器，CMake ≥ 3.16
- Python 3 + PyYAML + Pygments（生成博客）

Ubuntu/Debian：

```bash
sudo apt install build-essential cmake python3-yaml python3-pygments
```

macOS：

```bash
brew install cmake python3
pip3 install -r blog/requirements.txt
```

## 快速开始

```bash
git clone https://github.com/shensjt/CPP.git
cd CPP

# 1. 生成演示站点
python3 blog/scripts/build.py

# 2. 编译服务器
cmake -S . -B build/server -DCMAKE_BUILD_TYPE=Release
cmake --build build/server -j

# 3. 运行
./build/server/web_server --host 127.0.0.1 --port 8080 \
  --root build/www --threads 4 --timeout 60
```

打开 `http://127.0.0.1:8080/`，首页会显示仓库自带的示例文章。

也可以：

```bash
make blog
make server
make run
```

自己的文章不放进公开仓库，放在本地 `docs/blog/`（已 gitignore），构建时指定内容目录：

```bash
python3 blog/scripts/build.py --content docs/blog
# 或
make blog-local
```

`build/www/` 是构建产物，不入库。干净 clone 后必须先跑 `build.py`，否则服务器没有可发的内容。

## 博客构建链路

```text
blog/demo/posts/**/*.md     仓库演示内容
docs/blog/posts/**/*.md     本地真实内容（gitignore）
        │
        │ blog/scripts/build.py
        │   解析 front matter → Markdown 转 HTML
        │   套 post.html → base.html
        ▼
build/www/    首页/栏目页/文章页/CSS（构建产物，不入库）
        │
        │ web_server --root build/www
        ▼
浏览器
```

- 演示内容：`blog/demo/posts/**/*.md`、`blog/demo/about.md`
- 个人内容：`docs/blog/`（本地，gitignore）
- 模板：`blog/templates/`
- 输出：`build/www/`
- 构建依赖：[blog/requirements.txt](blog/requirements.txt)
- 详细说明：[blog/README.md](blog/README.md)
- 部署：`rsync -avz --delete build/www/ user@host:/opt/sjtwebserver/www/`

> 仓库里只有一篇演示文章；真实文章放本地 `docs/blog/`，用 `--content docs/blog` 构建，不会进公开仓库。

## Docker（本地）

构建：

```bash
docker build --build-arg APT_MIRROR=mirrors.aliyun.com -t sjtwebserver:dev .
```

运行：

```bash
docker run --rm -p 127.0.0.1:8080:8080 sjtwebserver:dev
```

或使用 compose：

```bash
APT_MIRROR=mirrors.aliyun.com docker compose up --build -d
docker compose ps
curl -f http://127.0.0.1:8080/
docker compose down
```

- 镜像包含 `web_server` 与 `blog/demo` 生成的 `www`；
- 容器内监听 `0.0.0.0:8080`，宿主机只发布到 `127.0.0.1`；
- 运行用户是 `app`（UID 10001），不是 root；
- `APT_MIRROR` 只影响构建阶段，默认 `archive.ubuntu.com`，国内可用 `mirrors.aliyun.com`。

## 测试

```bash
cmake -S tests -B build/tests
cmake --build build/tests -j
ctest --test-dir build/tests --output-on-failure
```

当前 6 个测试可执行文件、206 个用例：

- 线程池 / 阻塞队列
- 内存池
- Epoll / Buffer / TcpServer / TcpConnection
- HTTP 解析 / 响应 / 静态文件 / LRU / HttpServer
- 异步日志
- KV：跳表 / RESP / KVStore / 命令分发 / AOF / 服务集成

Sanitizer 构建：

```bash
cmake -S tests -B build/tests-asan -DENABLE_ASAN=ON
cmake --build build/tests-asan -j
ctest --test-dir build/tests-asan --output-on-failure

cmake -S tests -B build/tests-tsan -DENABLE_TSAN=ON
cmake --build build/tests-tsan -j
ctest --test-dir build/tests-tsan --output-on-failure
```

ASan/UBSan 和 TSan 均通过，正常构建零告警。

## 压测数据

环境：WSL2 / Ubuntu，AMD Ryzen 7 7840H，16 逻辑核；客户端与服务端同机，数字只用于相对比较。

Echo（64B payload，8 客户端）：

- 单 IO 线程：约 37k QPS
- 4 worker：约 34k QPS

Echo（32 客户端）：

- 单 IO 线程：约 61k QPS
- 4 worker：约 74k QPS

HTTP keep-alive（`GET /`，1.35KB 响应，16 客户端）：

- 单 IO 线程：约 9.5k QPS
- 4 worker：约 23.5k QPS

结论：Echo 这类几乎无计算量的场景，线程池开销大于收益；HTTP 解析 + 静态文件有实际工作，4 worker 提升约 2.5 倍。

---

# KV Store

Redis 风格的 KV 存储，复用 Web Server 的 `common/net`、线程池和测试体系。

## KV · 功能

- RESP 协议：数组 + inline 命令，二进制安全，半包不消费 Buffer
- 字符串 KV + 有序集合 ZSet（`unordered_map` + 跳表）
- `PING / SET / GET / DEL / EXISTS`
- `ZADD / ZSCORE / ZREM / ZRANGE / ZRANGEBYSCORE`
- `std::shared_mutex` 读写锁：读读并行，写写串行
- AOF：RESP 编码追加、everysec fsync、启动 replay、尾部截断修复
- `sigwait` 优雅停机：worker 退出后才停 AOF
- 压测客户端 `bench_kv`

## KV · 架构

```text
nc / Redis 客户端
  │ RESP over TCP
  ▼
TcpServer（epoll + ThreadPool + EPOLLONESHOT）
  │
  ▼
KvServer::OnMessage
  ├── resp::Parser          无状态，半包留在 Buffer
  ├── CommandHandler        参数校验 + RESP 响应
  │     └── KvStore（shared_mutex）
  │           ├── unordered_map<string, Entry>
  │           └── ZSet ── dict(member→score) + SkipList(score, member)
  └── Aof                   写锁内追加，后台 everysec fsync
```

## KV · 命令

| 命令 | 说明 |
|---|---|
| `PING` | 连通性 |
| `SET key value` | 字符串写入 |
| `GET key` | 字符串读取 |
| `DEL key [key...]` | 删除，返回删除数量 |
| `EXISTS key [key...]` | 存在性 |
| `ZADD key score member [score member...]` | 有序集合写入，返回新增 member 数 |
| `ZSCORE key member` | 查 score |
| `ZREM key member [member...]` | 删除 member |
| `ZRANGE key start stop` | 按名次，支持负下标 |
| `ZRANGEBYSCORE key min max` | 按 score，支持 `(`、`-inf`、`+inf` |

## KV · 运行

```bash
# 编译全部目标（web_server / kv_store / bench_kv）
cmake -S . -B build/server -DCMAKE_BUILD_TYPE=Release
cmake --build build/server -j

# 默认 AOF 路径 data/kv.aof
./build/server/kv_store --host 127.0.0.1 --port 6380 --threads 4

# 关闭持久化
./build/server/kv_store --port 6380 --threads 4 --no-aof

# 另一个终端
nc 127.0.0.1 6380
```

也可以用 `make run-kv`。

## KV · 压测

环境同 Web Server：WSL2 / Ubuntu，AMD Ryzen 7 7840H，16 逻辑核；客户端与服务端同机，数字只用于相对比较。

128 客户端，每组 25.6 万请求（2000 请求/客户端），3 次取中位数：

| 配置 | SET | GET | MIXED |
|---|---|---|---|
| 单 IO 线程 | 55.6k | 54.0k | 57.4k |
| 4 worker | 92.0k | 107.2k | 80.3k |
| 4 worker + AOF everysec | 76.7k | 109.7k | 69.3k |

结论：

- 4 worker 对 SET 提升约 1.65 倍，对 GET 提升约 2 倍
- AOF 只影响写路径：SET 下降约 17%，GET 基本不变
- everysec 的代价是崩溃最多丢约 1 秒数据
- 压测客户端本身也会成为瓶颈，数字用于横向比较

---

# 工程与交付

## 文档

- 架构：[doc/architecture.md](doc/architecture.md)
- 部署与回滚：[doc/runbooks/](doc/runbooks/)
- 发布记录：[doc/releases/](doc/releases/)
- 故障复盘：[doc/incidents/](doc/incidents/)

## 目录结构

```text
common/
  thread_pool/     线程池 + 阻塞队列
  memory_pool/     10 桶内存池（独立组件）
  net/             Epoll / TcpServer / TcpConnection / Buffer
  log/             双缓冲异步日志
projects/web_server/
  http/            HttpParser / HttpResponse / StaticFileHandler / HttpServer
  lru_cache/       LRU
projects/kv_store/
  skiplist/        跳表（有序集合）
  store/           KVStore / ZSet / 分数格式
  protocol/        RESP 解析与序列化
  command/         命令分发
  aof/             AOF 追加、刷盘、回放
  server/          KvServer
  bench/           bench_kv 压测客户端
blog/              博客源文件、模板、构建脚本
tests/             单元/集成测试
deploy/            旧 systemd 部署脚本（当前停用）
doc/               项目工程文档
  architecture.md  架构与关键决策
  releases/        发布记录
  incidents/       故障复盘
  runbooks/        部署与回滚
CLAUDE.md          协作约定
Dockerfile         多阶段镜像构建
.dockerignore      构建上下文排除
compose.yaml       本地容器编排
build/             构建产物（不入库）
  server/          可执行文件
  www/             生成的静态站点
```

## 已知限制

Web Server：

- 没有实现 sendfile，大文件仍走用户态缓冲
- 内存池尚未接入 Web 服务器，目前作为独立组件
- 当前没有域名和 TLS，直接 HTTP 8080
- 读缓冲 2MB、写缓冲 8MB，超限直接关闭慢连接；没有完整的暂停/恢复背压
- 没有限流、连接数上限和认证

KV Store：

- AOF 没有实现重写，文件会随写操作持续增长
- `ZRANGE` 按名次是 O(N)，没有 Redis 的 `span` 优化
- 没有 key 过期、淘汰策略、事务和集群
- AOF everysec 在崩溃时最多丢约 1 秒数据
- 没有认证和限流
- 压测客户端与服务端同机，数字只做相对比较

其他：

- 博客仓库只带一篇演示文章，真实内容在本地 `docs/blog/` 维护

## 部署

当前生产使用 Docker Compose：

- 运行目录：`/opt/sjtweb-runtime`
- 内容目录：`/opt/sjtwebserver/projects/web_server/www`
- 入口：`http://39.106.115.254:8080`
- 镜像：阿里云 ACR，tag 为 `sha-<commit>`
- CD：合并 `main` 后自动构建、推送、部署、smoke

部署与回滚：

- [doc/runbooks/deploy.md](doc/runbooks/deploy.md)
- [doc/runbooks/rollback.md](doc/runbooks/rollback.md)

`deploy/` 下的 systemd 脚本是旧部署方式，当前停用，保留作回滚兜底。

## 链接

- GitHub: https://github.com/shensjt/CPP
