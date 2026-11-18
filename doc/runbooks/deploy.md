# 部署 Runbook

> 环境：阿里云 ECS（Ubuntu 22.04）
> 入口：`http://39.106.115.254:8080`（无域名、无 TLS）
> 更新：2026-11-18

## 服务器布局

```text
/opt/sjtwebserver/
  projects/web_server/www/   真实静态内容，容器只读挂载
  build/server/web_server    systemd 旧二进制，当前未使用
  ...                        源码仓库

/opt/sjtweb-runtime/
  compose.yaml               生产 compose
  .env                       IMAGE_TAG=sha-<commit>
  .env.prev                  CD 部署前备份

systemd：sjtwebserver 已 stop / disable，unit 文件保留作回滚兜底
Caddy：未安装
```

`/opt/sjtweb-runtime` 及 `.env` 归 `deploy` 用户所有；容器以 `app`（UID 10001）运行，只读挂载内容目录。

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
  → curl / 和 /css/style.css
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
curl -fsS http://127.0.0.1:8080/css/style.css >/dev/null
```

`/healthz` 的 `commit` 应与 `.env` 里 `IMAGE_TAG` 去掉 `sha-` 前缀后的值一致。

## 验证

```bash
cd /opt/sjtweb-runtime
cat .env
docker compose ps
curl -s http://127.0.0.1:8080/healthz
curl -I http://127.0.0.1:8080/

# 外部
curl --noproxy '*' -I http://39.106.115.254:8080/
```

期望：

- `.env` 里的 `IMAGE_TAG` 与 `docker compose ps` 的 image tag 一致；
- `/healthz` 的 `commit` 与 `IMAGE_TAG` 去掉 `sha-` 后一致。

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

- 内容目录是否已更新；
- 浏览器是否缓存（强制刷新）；
- 容器挂载路径是否正确。

仍无法解决时再重启：

```bash
cd /opt/sjtweb-runtime
docker compose restart web
```

### 8080 被占用

检查 systemd 是否又被启动：

```bash
systemctl is-active sjtwebserver
ss -ltnp | grep ':8080'
```

容器和 systemd 不能同时占用 8080。

## 回滚

见 [rollback.md](rollback.md)。
