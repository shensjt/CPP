# 博客构建

Markdown 源文件在 `blog/posts/`，构建后生成静态站到 `projects/web_server/www/`。服务器只负责发文件，不需要 Python。

## 依赖

```bash
python3 -m venv .venv
. .venv/bin/activate
pip install -r blog/requirements.txt
```

Ubuntu 也可以直接装系统包：

```bash
sudo apt install python3-yaml python3-pygments
```

## 目录

```text
blog/
├── config.yaml          站点名、作者、GitHub
├── about.md             关于页
├── post-template.md     文章模板，不在 posts/ 下，不会被构建
├── posts/
│   ├── devlog/
│   ├── deepdive/
│   ├── career/
│   └── release/
├── templates/
│   ├── base.html        页面骨架
│   ├── index.html       首页
│   ├── post.html        文章页
│   ├── list.html        栏目页
│   ├── favicon.svg
│   └── css/style.css
└── scripts/build.py
```

## 写一篇文章

1. 复制模板：

```bash
cp blog/post-template.md blog/posts/devlog/2026-11-02-first-post.md
```

2. 填 front matter：

```yaml
---
title: "文章标题"
date: "2026-11-02"
category: "devlog"        # devlog / deepdive / career / release
tags: ["C++", "epoll"]
summary: "列表页显示的一句话"
draft: false
---
```

3. 正文用 Markdown 写。当前支持标题、段落、列表、引用、代码块、行内代码、加粗、链接；暂不支持表格和图片。

4. 构建：

```bash
python3 blog/scripts/build.py
```

输出：

```text
built N posts into .../projects/web_server/www
```

## 本地预览

```bash
python3 -m http.server -d projects/web_server/www 8000
# http://127.0.0.1:8000/
```

或者用 C++ 服务器：

```bash
./build/server/web_server --host 127.0.0.1 --port 8080 \
  --root projects/web_server/www --threads 4 --timeout 60
```

## 发布

```bash
rsync -avz --delete projects/web_server/www/ \
  root@<ECS公网IP>:/opt/sjtwebserver/www/
```

静态文件直接生效，不需要重启服务。

## 注意

- `build.py` 会清空并重建 `www/`，不要手改生成后的 HTML。
- `draft: true` 的文章不会生成。
- `www/` 已加入 `.gitignore`，只提交源码和模板。
