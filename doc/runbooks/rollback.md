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
curl -fsS http://127.0.0.1:8080/css/style.css >/dev/null
```

如果 `.env.prev` 不存在，手动写入上一个可用的 sha：

```bash
echo "IMAGE_TAG=sha-<previous>" > .env
docker compose up -d
```

## 回滚到 systemd

systemd unit 仍保留，但当前是 disabled。

```bash
cd /opt/sjtweb-runtime

# 先停容器，释放 8080
docker compose down

# 启动旧服务
systemctl enable --now sjtwebserver

curl -I http://127.0.0.1:8080/
```

外部验证：

```bash
curl --noproxy '*' -I http://39.106.115.254:8080/
```

## 回滚后确认

```bash
systemctl is-active sjtwebserver
systemctl is-enabled sjtwebserver
docker compose ps
ss -ltnp | grep ':8080'
```

- 容器和 systemd 只能有一个占用 8080；
- 回滚到 systemd 后，systemd 会重新开机自启；
- 再切回容器：

```bash
systemctl stop sjtwebserver
systemctl disable sjtwebserver
cd /opt/sjtweb-runtime
docker compose up -d
```

## 记录

回滚后记录：

- 失败版本 tag；
- 回滚到的 tag；
- 失败阶段（build / pull / up / smoke）；
- 原始日志；
- 后续修复。
