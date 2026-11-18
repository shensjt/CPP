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
ss -ltnp | grep ':8080'
curl -fsS http://127.0.0.1:8080/
```

旧 systemd unit 已停用，旧 `deploy/` 脚本已删除；不再提供 systemd 回滚路径。

## 记录

回滚后记录：

- 失败版本 tag；
- 回滚到的 tag；
- 失败阶段（build / pull / up / smoke）；
- 原始日志；
- 后续修复。
