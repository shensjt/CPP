# 内容发布 Runbook

> 环境：本地 WSL / Linux
> 目标：`/opt/sjtweb-runtime/www`
> 入口：`https://shenjuntao.com/`
> 更新：2026-11-26

## 前提

- 本地能免密 `ssh deploy@39.106.115.254`；
- 服务器有 `rsync` 和 `curl`；
- `/opt/sjtweb-runtime/www` 归 `deploy` 所有；
- 生产 compose 挂载 `/opt/sjtweb-runtime/www:/app/www:ro`；
- 本地已安装 MkDocs Material：

```bash
python3 -m venv .venv
. .venv/bin/activate
pip install -r blog/requirements.txt
```

## 发布

```bash
make publish
```

等价于：

```bash
mkdocs build -f blog/mkdocs.yml --strict
test -s build/www/index.html

ssh deploy@39.106.115.254 "
  cd /opt/sjtweb-runtime
  rm -rf www.prev
  cp -al www www.prev
"

rsync -az --delete-after --delay-updates --chmod=D755,F644 \
  build/www/ deploy@39.106.115.254:/opt/sjtweb-runtime/www/

ssh deploy@39.106.115.254 '
  curl -fsS http://127.0.0.1:8080/ >/dev/null &&
  curl -fsS http://127.0.0.1:8080/sitemap.xml >/dev/null
'
```

脚本变量：

| 变量 | 默认值 |
|---|---|
| `HOST` | `deploy@39.106.115.254` |
| `REMOTE` | `/opt/sjtweb-runtime` |
| `BASE` | `http://127.0.0.1:8080` |
| `MKDOCS` | `mkdocs` |
| `SSH_KEY` | 空（使用默认 key / agent） |

如果 key 不在默认位置：

```bash
SSH_KEY=$HOME/.ssh/sjt_deploy make publish
```

其他示例：

```bash
HOST=deploy@example.com make publish
```

## 验证

```bash
ssh deploy@39.106.115.254 '
  curl -I http://127.0.0.1:8080/
  curl -I http://127.0.0.1:8080/sitemap.xml
'

curl --noproxy '*' -I https://shenjuntao.com/

内容发布只写 `/opt/sjtweb-runtime/www`，不重启容器，也不重载 Caddy。
```

发布前后确认容器没有重启：

```bash
ssh deploy@39.106.115.254 '
  cd /opt/sjtweb-runtime
  docker inspect --format "{{.State.StartedAt}}" "$(docker compose ps -q web)"
'
```

## 回滚

```bash
ssh deploy@39.106.115.254 '
  cd /opt/sjtweb-runtime
  rsync -a --delete www.prev/ www/
  curl -fsS http://127.0.0.1:8080/ >/dev/null
'
```

只有一份 `www.prev`。不能用 `mv www.prev www` 替换目录：容器的 bind mount 指向 `www`，替换目录会让容器继续看着旧 inode。

## 失败处理

| 现象 | 处理 |
|---|---|
| `mkdocs build --strict` 失败 | 按报错修相对链接 / 锚点 |
| 空构建 | 检查 `blog/content/` 和 `docs_dir` |
| rsync 非 0 | 重跑；`www.prev` 还在 |
| smoke 失败 | 脚本自动回滚；检查 `/` 和 `/sitemap.xml` |
| 页面没更新 | 强制刷新；检查 mtime/ETag |
| 404 | 补尾斜杠；检查文件是否存在 |
| 403 | 检查 `--chmod` 和目录属主 |
| 线上出现 demo | 检查 bind mount，勿用 named volume |
| 首次发布回滚失败 | `www.prev` 为空，人工处理 |

## 首次上线顺序

v0.5 的 CD smoke 检查 `/sitemap.xml`。如果服务器还是旧内容目录（没有 sitemap），合并 `main` 后 CD 会失败并回滚。因此顺序必须是：

```text
1. 服务器内容目录迁移 + compose 挂载
2. 本地 make publish，确认 /sitemap.xml 200
3. 再合并 main / 触发 CD
```

## 一次性服务器迁移

从旧内容目录迁移到运行时目录：

```bash
sudo mkdir -p /opt/sjtweb-runtime/www
sudo rsync -a /opt/sjtwebserver/projects/web_server/www/ /opt/sjtweb-runtime/www/
sudo chown -R deploy:deploy /opt/sjtweb-runtime/www
```

生产 compose 改为：

```yaml
volumes:
  - /opt/sjtweb-runtime/www:/app/www:ro
```

然后：

```bash
cd /opt/sjtweb-runtime
docker compose up -d
docker inspect "$(docker compose ps -q web)" --format '{{json .Mounts}}'
```

确认 `Type=bind`、`Source=/opt/sjtweb-runtime/www`、`RW=false`。

## 不重启

内容发布只写宿主机目录，不调用 `docker compose`。服务器启动时已固化 root，但 LRU 每次命中都会 `stat` 文件，mtime/size 变化后重新读取，所以发布后刷新即可。
