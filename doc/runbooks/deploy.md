# 部署 Runbook

> 环境：阿里云 ECS（Ubuntu 22.04）
> 入口：`http://39.106.115.254:8080`（无域名、无 TLS）
> 更新：2026-11-18

## 服务器布局

```text
/opt/sjtweb-runtime/
  compose.yaml               生产 compose
  .env                       IMAGE_TAG=sha-<commit>
  .env.prev                  CD 部署前备份
  www/                       发布目录，容器只读挂载
  www.prev/                  上一次内容发布快照

Caddy：未安装
```

旧 systemd unit 已停用；`/opt/sjtwebserver` 旧目录如仍存在，只是历史残留，可手动清理。

`/opt/sjtweb-runtime`、`.env` 和 `www/` 归 `deploy` 用户所有；容器以 `app`（UID 10001）运行，只读挂载内容目录。

## 镜像

```text
crpi-m3ex8dsaem6zwhaf.cn-beijing.personal.cr.aliyuncs.com/sjtwebserver/sjtwebserver:${IMAGE_TAG}
```

- `IMAGE_TAG` 使用 `sha-<commit>`，不可变；
- 不部署 `latest`；
- ACR 仓库私有，服务器 `deploy` 用户已登录。

## 自动部署（CD）

触发：push 到 `main`。

```text
build-and-push
  → build linux/amd64 镜像
  → 关闭 provenance / SBOM
  → push :sha-<commit>

deploy
  → SSH deploy@39.106.115.254
  → cd /opt/sjtweb-runtime
  → 备份 .env 为 .env.prev
  → 写入 IMAGE_TAG=sha-<commit>
  → docker compose pull
  → docker compose up -d
  → curl /healthz，校验 commit
  → curl / 和 /sitemap.xml
  → 失败：恢复 .env.prev 并 up -d
```

GitHub Secrets：

```text
SSH_HOST / SSH_USER / SSH_KEY
ACR_USERNAME / ACR_PASSWORD
```

## 手动部署

```bash
cd /opt/sjtweb-runtime

# 指定要部署的镜像 tag
echo "IMAGE_TAG=sha-<commit>" > .env

docker compose pull
docker compose up -d

curl -fsS http://127.0.0.1:8080/healthz
curl -fsS http://127.0.0.1:8080/ >/dev/null
curl -fsS http://127.0.0.1:8080/sitemap.xml >/dev/null
```

`/healthz` 的 `commit` 应与 `.env` 里 `IMAGE_TAG` 去掉 `sha-` 前缀后的值一致。

## 内容发布

内容由本地 MkDocs 构建，不走镜像和 CD：

```bash
make publish
```

脚本流程：

```text
mkdocs build --strict
→ 快照 www → www.prev
→ rsync --delete-after --delay-updates
→ curl / 和 /sitemap.xml
→ 失败：rsync www.prev 回 www
```

不重启容器。详见 [publish-content.md](publish-content.md)。

## 验证

```bash
cd /opt/sjtweb-runtime
cat .env
docker compose ps
curl -s http://127.0.0.1:8080/healthz
curl -I http://127.0.0.1:8080/
curl -I http://127.0.0.1:8080/sitemap.xml

# 外部
curl --noproxy '*' -I http://39.106.115.254:8080/
```

期望：

- `.env` 里的 `IMAGE_TAG` 与 `docker compose ps` 的 image tag 一致；
- `/healthz` 的 `commit` 与 `IMAGE_TAG` 去掉 `sha-` 后一致。

## 停机

```bash
cd /opt/sjtweb-runtime
docker compose stop web
```

`docker stop` 发送 SIGTERM：服务停止 accept、等待 worker 退出、刷新日志后退出。默认 grace period 内应正常结束。

## 常见故障

### ACR 认证连接被重置

```text
dockerauth.cn-hangzhou.aliyuncs.com:443
connection reset by peer
```

GitHub runner 与阿里云认证服务之间的跨境网络抖动。重新运行失败的 `deploy` job；不需要改 workflow。

### ACR 拒绝 OCI manifest

```text
unknown manifest class for application/vnd.oci.empty.v1+json
```

buildx 默认附加 attestation，ACR 不支持。workflow 已设置：

```text
platforms: linux/amd64
provenance: false
sbom: false
```

### smoke test 失败

CD 会自动恢复 `.env.prev` 并重新 `up -d`。查看：

```bash
cd /opt/sjtweb-runtime
cat .env.prev
docker compose ps
docker compose logs --tail=100
```

### 内容更新后页面没变

LRU 会按 mtime/size 失效，正常情况下内容发布后不需要重启容器。先检查：

- `make publish` 是否成功；
- `/opt/sjtweb-runtime/www` 是否已更新；
- 浏览器是否缓存（强制刷新）；
- 容器挂载路径是否正确。

仍无法解决时再重启：

```bash
cd /opt/sjtweb-runtime
docker compose restart web
```

### 线上出现 demo

检查容器实际挂载：

```bash
cd /opt/sjtweb-runtime
docker inspect "$(docker compose ps -q web)" --format '{{json .Mounts}}'
```

期望 `Type` 是 `bind`、`Source` 是 `/opt/sjtweb-runtime/www`、`RW` 是 `false`。如果配成了 named volume，Docker 会把镜像里的 demo 复制进去，宿主机 rsync 的文件不会被看到。

### 8080 被占用

检查 systemd 是否又被启动：

```bash
systemctl is-active sjtwebserver
ss -ltnp | grep ':8080'
```

容器和 systemd 不能同时占用 8080。

## 回滚

见 [rollback.md](rollback.md)。
