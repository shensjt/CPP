# 回滚 Runbook

> 环境：阿里云 ECS
> 更新：2026-11-18

## 回滚到上一个镜像 tag

CD 部署前会把当前 `.env` 备份为 `.env.prev`。

```bash
cd /opt/sjtweb-runtime

cat .env.prev
cp .env.prev .env

docker compose pull
docker compose up -d

curl -fsS http://127.0.0.1:8080/ >/dev/null
curl -fsS http://127.0.0.1:8080/sitemap.xml >/dev/null
```

如果 `.env.prev` 不存在，手动写入上一个可用的 sha：

```bash
echo "IMAGE_TAG=sha-<previous>" > .env
docker compose up -d
```

## 内容回滚

只回滚内容，不动镜像和容器：

```bash
ssh deploy@39.106.115.254 '
  cd /opt/sjtweb-runtime
  rsync -a --delete www.prev/ www/
'
curl -fsS http://127.0.0.1:8080/
```

只有一份 `www.prev`。重要发布前另行留档。不能用 `mv www.prev www` 替换目录：容器的 bind mount 指向 `www`，替换目录会让容器继续看着旧 inode。

## 回滚后确认

```bash
cd /opt/sjtweb-runtime
docker compose ps
ss -ltnp | grep -E ':(80|443|8080)\b'
systemctl is-active caddy
curl -fsS http://127.0.0.1:8080/healthz
curl -fsS https://shenjuntao.com/healthz
curl -I https://www.shenjuntao.com/
```

旧 systemd unit 已停用，旧 systemd 部署脚本已删除；不再提供 systemd 回滚路径。

## Caddy 配置回滚

Caddyfile 由仓库 `deploy/Caddyfile` 管理。修改前先备份：

```bash
sudo cp /etc/caddy/Caddyfile /etc/caddy/Caddyfile.bak
```

恢复旧配置：

```bash
sudo cp /etc/caddy/Caddyfile.bak /etc/caddy/Caddyfile
sudo caddy validate --config /etc/caddy/Caddyfile
sudo systemctl restart caddy
```

## 临时回退到 IP:8080

只在 Caddy 不可用时使用，恢复后要改回：

```bash
# 1. 安全组临时放行 8080
# 2. compose 端口改回 0.0.0.0:8080:8080
cd /opt/sjtweb-runtime
sed -i 's/"127.0.0.1:8080:8080"/"0.0.0.0:8080:8080"/' compose.yaml
docker compose up -d

# 3. 停 Caddy
sudo systemctl stop caddy
```

恢复后把端口改回 `127.0.0.1:8080:8080`，重启容器，再关闭安全组 8080。

## 记录

回滚后记录：

- 失败版本 tag；
- 回滚到的 tag；
- 失败阶段（build / pull / up / smoke）；
- 原始日志；
- 后续修复。
