# 部署说明

目标结构：

```text
Caddy :443 ──> web_server 127.0.0.1:8080
```

C++ 服务只监听回环地址，TLS 和公网入口由 Caddy 处理。

## 1. 服务器准备

以 Debian/Ubuntu 为例：

```bash
sudo useradd -r -s /usr/sbin/nologin webserver || true
sudo mkdir -p /opt/sjtwebserver/bin /opt/sjtwebserver/www /opt/sjtwebserver/logs
sudo chown -R webserver:webserver /opt/sjtwebserver
```

防火墙/安全组只开放 22、80、443；8080 不要对公网开放。

## 2. 安装 Caddy

按官方文档安装后，复制 `deploy/Caddyfile` 到 `/etc/caddy/Caddyfile`，把 `example.com` 换成你的域名：

```bash
sudo cp deploy/Caddyfile /etc/caddy/Caddyfile
sudo caddy validate --config /etc/caddy/Caddyfile
sudo systemctl reload caddy
```

Caddy 会自动申请 Let's Encrypt 证书。前提是域名已解析到服务器，80/443 可达。

## 3. 安装 systemd 服务

```bash
sudo cp deploy/sjtwebserver.service /etc/systemd/system/sjtwebserver.service
sudo systemctl daemon-reload
sudo systemctl enable --now sjtwebserver
sudo systemctl status sjtwebserver
sudo journalctl -u sjtwebserver -f
```

unit 里的路径默认是 `/opt/sjtwebserver`，服务绑定 `127.0.0.1:8080`，空闲连接 60 秒关闭。

## 4. 部署

本地：

```bash
SSH_TARGET=webserver@your-server-ip ./deploy/deploy.sh
```

脚本会：

1. 运行 `blog/scripts/build.py` 生成静态站
2. 编译 `web_server`
3. `rsync` 推送 `www/` 和二进制
4. 保留上一个二进制为 `web_server.prev`，然后重启 systemd 服务

首次部署前确认远端目录存在：

```bash
ssh webserver@your-server-ip 'ls -ld /opt/sjtwebserver/{bin,www,logs}'
```

## 5. 验收

```bash
curl -I https://example.com/
curl -I https://example.com/css/style.css
curl -s -o /dev/null -w '%{http_code}\n' \
  --path-as-is https://example.com/../../etc/passwd   # 期望 403
curl -s -o /dev/null -w '%{http_code}\n' https://example.com/nope  # 期望 404
ss -lntp | grep -E ':(80|443|8080)'
```

## 6. 回滚

```bash
ssh webserver@your-server-ip '
  cd /opt/sjtwebserver
  mv bin/web_server.prev bin/web_server
  sudo systemctl restart sjtwebserver
'
```

## 7. 域名与备案

- 大陆服务器：域名需要 ICP 备案；备案通常要求服务器购买 3 个月以上，管局审核 1–3 周。备案期间不要对外解析域名。
- 香港/海外服务器：不需要 ICP 备案，HTTPS 证书正常申请。
- 校招会临时演示可以用 Cloudflare Tunnel：

```bash
cloudflared tunnel --url http://127.0.0.1:8080
```

这是临时地址，不是正式域名；演示可以，不要长期对外服务。

## 8. 日志

- 应用日志：`/opt/sjtwebserver/logs/server.log`（相对 `WorkingDirectory` 生成）
- 访问日志：Caddy 默认输出到 journald，`sudo journalctl -u caddy -f`
- 服务状态：`sudo systemctl status sjtwebserver`
