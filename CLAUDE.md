# CLAUDE.md

> 项目协作约定。Claude Code 会读取本文件。

## 项目

两个共享 `common/` 基础设施的 C++17 项目：

- `web_server`：epoll + 线程池的静态 HTTP 文件服务
- `kv_store`：跳表 + RESP + 读写锁 + AOF 的 KV 存储

主要目录：

```text
common/       net / thread_pool / log / memory_pool
projects/     web_server / kv_store
tests/        单元与集成测试
blog/         Markdown → HTML 构建
deploy/       旧 systemd 部署脚本（当前停用）
doc/          项目工程文档
docs/         学习与面试材料（不入库）
```

## 常用命令

```bash
make verify        # 构建 + 测试 + 博客构建，本地与 CI 共用
make test          # 只跑测试
make blog          # 生成 build/www
make server        # 编译 web_server
make run           # 本地启动 web_server
make run-kv        # 本地启动 kv_store

docker build --build-arg APT_MIRROR=mirrors.aliyun.com -t sjtwebserver:dev .
docker compose up --build -d
```

## 文档规则

- `doc/`：只放项目事实，例如架构、发布记录、故障复盘、压测数据、运维手册。
- `docs/`：只放学习、讲解、方案讨论、面试材料。
- `doc/` 不写计划、目录说明、学习笔记，也不链接 `docs/` 的讲解材料。
- 源码注释短而自然；详细解释放 `docs/`。
- 常青文档（架构、runbook）只保留最新版；发布、故障、压测记录只追加。

## 分支与发布

```text
feature/fix/chore 分支
  → PR 到 fast-track
  → CI 绿
  → 合并

fast-track 到可发布里程碑
  → PR 到 main
  → CI 绿
  → 合并
  → CD 自动部署
```

- `fast-track` 是开发集成分支，只跑 CI。
- `main` 是生产分支，合并触发 CD。
- 不要直接把日常改动推到 `main`。

## CI / CD

CI（`.github/workflows/ci.yml`）：

- `verify`：Release 构建 + 测试 + 博客构建
- `asan`：ASan/UBSan
- `tsan`：TSan
- `docker`：构建镜像并启动容器冒烟

CD（`.github/workflows/deploy.yml`）：

- 触发：push 到 `main`
- 构建并推送 `sha-<commit>` 镜像到阿里云 ACR
- SSH 到 ECS，更新 `/opt/sjtweb-runtime/.env`
- `docker compose pull` / `up -d`
- smoke test：`/` 和 `/css/style.css`
- 失败恢复 `.env.prev`

## 部署

- 服务器：阿里云 ECS，Docker Compose
- 运行时目录：`/opt/sjtweb-runtime`
- 内容目录：`/opt/sjtwebserver/projects/web_server/www`
- 镜像：`crpi-m3ex8dsaem6zwhaf.cn-beijing.personal.cr.aliyuncs.com/sjtwebserver/sjtwebserver:sha-<commit>`
- 入口：`http://39.106.115.254:8080`（无域名、无 TLS）
- systemd `sjtwebserver` 已 stop / disable，unit 保留作回滚兜底
- 部署与回滚：[doc/runbooks/deploy.md](doc/runbooks/deploy.md)、[doc/runbooks/rollback.md](doc/runbooks/rollback.md)

## 提交与 PR

- 使用 Conventional Commits，例如 `fix(net): ...`、`ci: ...`、`docs: ...`。
- 一个提交只做一件事。
- PR 描述写清背景、修改、验证、风险。
- 提交和 PR 不加 AI 署名。

## 已知限制

- 没有域名和 TLS，直接 HTTP 8080。
- GitHub runner 到 ACR 认证端点偶发 `connection reset`，重跑 deploy job。
- 读缓冲 2MB、写缓冲 8MB，超限直接关闭连接；没有完整背压。
- 访问日志写在本地文件，没有集中日志、metrics 和告警。
- 内容流水线尚未实现。
- KV 没有 TTL、淘汰、AOF 重写和集群。
- Web Server 没有 sendfile、限流和连接数上限。

<!--
文档空间结构设计（按要求暂以注释保留，确认后启用）：

doc/
├── architecture.md                 常青：系统、模块、数据流、决策、限制
├── runbooks/
│   ├── deploy.md
│   └── rollback.md
├── releases/
│   ├── v0.1-demo.md
│   ├── v0.2-kv.md
│   └── v0.3.0.md
├── incidents/
│   ├── 2026-11-18-tsan-connection-race.md
│   └── 2026-11-18-fd-reuse-parser-uaf.md
└── benchmarks/
    └── YYYY-MM-DD-主题.md         有真实数据再建

入口：根 README 的“文档”一节。
-->
