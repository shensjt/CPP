# 博客构建

仓库只带 `blog/demo/` 里的演示内容；真实文章放本地 `blog/content/`（已 gitignore）。构建后生成静态站到 `build/www/`。服务器只负责发文件，不需要 Python。

## 目录

```text
blog/
├── mkdocs.yml           站点配置
├── mkdocs-demo.yml      演示站配置
├── requirements.txt     MkDocs Material
├── post-template.md     文章模板
├── content/             真实内容（gitignore）
│   ├── index.md         首页
│   ├── about.md
│   ├── devlog/
│   ├── deepdive/
│   ├── notes/
│   ├── release/
│   ├── projects/
│   └── images/
├── drafts/              草稿（gitignore，不参与构建）
├── demo/                演示内容
└── overrides/           可选，主题定制
```

## 依赖

```bash
python3 -m venv .venv
. .venv/bin/activate
pip install -r blog/requirements.txt
```

Ubuntu 24.04 的系统 Python 受 PEP 668 保护，建议用 venv。

## 写一篇文章

1. 在对应分块新建文件：

```bash
$EDITOR blog/content/devlog/2026-11-19-first-post.md
```

2. 第一行写 H1，下面写日期和正文：

```markdown
# 文章标题

2026-11-19

正文……

## 背景

![图片](thread-pool.png)

相关阅读：[另一篇](../deepdive/another-post.md)
```

3. 图片放文章旁边，或放 `content/images/` 并用 `../images/xxx.png`。
4. 站内链接写相对 `.md` 路径，MkDocs 会改写成 HTML URL；目标不存在时构建失败。

## 预览与构建

```bash
# 本地预览真实内容
make serve

# 真实内容构建到 build/www
make blog-local

# 演示内容构建到 build/www
make blog

# 完整验证：C++ 构建/测试 + 演示站构建到 build/verify-www
make verify
```

## 发布

```bash
make publish
```

脚本会构建、快照 `www.prev`、rsync 到服务器、smoke，失败自动回滚。不重启容器。

## 注意

- 不要手改 `build/www/`，下次构建会清空；
- 不再使用 front matter；MkDocs 默认不解析，会当正文渲染；
- `draft` 文章放 `blog/drafts/`，写完再移进 `content/`；
- 文件名就是 URL，发布后不要随便改名；
- 真实内容不要提交到公开仓库。
