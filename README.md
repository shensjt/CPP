# SJTWebServer

一个从零实现的高并发静态 Web 服务器：C++17 + epoll + 线程池，支持 HTTP/1.1 keep-alive、静态文件、LRU 缓存、异步日志和空闲连接超时。博客站点本身就跑在这台服务器上。

## 功能

- 线程池 + 阻塞队列，任务投递与优雅停机
- epoll 事件循环，非阻塞 IO，`EPOLLONESHOT` 下 worker 拥有连接
- 分段 Buffer，`readv` 读取，处理半包/粘包
- HTTP/1.1 请求解析：请求行、Headers、Body 状态机
- 静态文件服务：MIME、目录 index、HEAD、404/403/405/400
- keep-alive 与流水线，`CloseAfterWrite` 保证响应发完再关
- LRU 文件缓存（默认 16MB）
- 双缓冲异步日志，everysec fsync，按大小滚动
- 空闲连接超时，默认 60 秒
- 路径穿越、URL 编码绕过、符号链接逃逸防护

## 架构

```text
浏览器
  │ HTTPS
  ▼
Caddy（TLS / 反向代理）
  │ HTTP/1.1 127.0.0.1:8080
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
- 连接注册 `EPOLLONESHOT`，同一 fd 同一时刻只有一个 worker
- TLS 由 Caddy 处理，C++ 里只跑明文 HTTP

## 快速开始

```bash
cmake -S . -B build/server -DCMAKE_BUILD_TYPE=Release
cmake --build build/server -j
./build/server/web_server --host 127.0.0.1 --port 8080 \
  --root projects/web_server/www --threads 4 --timeout 60
```

打开 `http://127.0.0.1:8080/`。

## 博客构建

```bash
python3 blog/scripts/build.py
```

- 源文件：`blog/posts/**/*.md`、`blog/about.md`
- 模板：`blog/templates/`
- 输出：`projects/web_server/www/`

> 仓库里目前只有测试占位文章，正式内容由作者自行填写。

## 测试

```bash
cmake -S tests -B build/tests
cmake --build build/tests -j
ctest --test-dir build/tests --output-on-failure
```

当前 5 个测试可执行文件、127 个用例：

- 线程池 / 阻塞队列
- 内存池
- Epoll / Buffer / TcpServer / TcpConnection
- HTTP 解析 / 响应 / 静态文件 / LRU / HttpServer
- 异步日志

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
  www/             生成的静态站点
blog/              博客源文件、模板、构建脚本
tests/             单元/集成测试
deploy/            Caddyfile、systemd、部署脚本
```

## 已知限制

- 没有实现 sendfile，大文件仍走用户态缓冲
- 内存池尚未接入 Web 服务器，目前作为独立组件
- TLS 不在 C++ 内实现，交给 Caddy
- 没有限流、连接数上限和认证
- KV 存储（跳表 + AOF）还在规划中
- 博客目前只有测试占位内容

## 部署

见 [deploy/README.md](deploy/README.md)。

## 链接

- GitHub: https://github.com/shensjt/CPP
