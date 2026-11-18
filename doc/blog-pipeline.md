# 博客内容流水线

> 更新：2026-11-18
> 工具：MkDocs Material
> 输出：`build/www/`

## 1. 结构

```text
blog/
├── mkdocs.yml           真实内容配置
├── mkdocs-demo.yml      演示内容配置
├── requirements.txt     mkdocs-material
├── content/             真实内容（gitignore）
│   ├── index.md
│   ├── about.md
│   ├── devlog/
│   ├── deepdive/
│   ├── notes/
│   ├── release/
│   ├── projects/
│   └── images/
├── drafts/              草稿（gitignore，不参与构建）
├── demo/                公开演示内容
└── overrides/           可选主题定制
```

一个顶层目录是一个分块，每个分块用 `index.md` 作为落地页。文件夹结构自动生成顶部 tabs 和侧边导航。

## 2. 内容规则

- 每页第一行是 `# 标题`，导航用它作为标题；
- 文章文件名用 `YYYY-MM-DD-slug.md`，决定 URL 和排序；
- 不使用 front matter；MkDocs 默认不解析，会当正文渲染；
- 站内链接写相对 `.md` 路径，构建时会被改写成 HTML URL；
- 图片放文章旁边，或放 `content/images/` 并用 `../images/...`；
- 草稿放 `blog/drafts/`，不放进 `content/`。

示例：

```markdown
# 线程池调优记录

2026-11-19

正文……

## 背景

![架构](thread-pool.png)

相关阅读：[另一篇](../deepdive/another-post.md)
```

## 3. URL

| 源文件 | URL |
|---|---|
| `content/index.md` | `/` |
| `content/about.md` | `/about/` |
| `content/devlog/index.md` | `/devlog/` |
| `content/devlog/2026-10-31-thread-pool.md` | `/devlog/2026-10-31-thread-pool/` |

服务器发现目标是目录时自动补 `index.html`，所以目录式 URL 直接可用。

## 4. 构建

```bash
make deps         # 安装 mkdocs-material
make serve        # 本地预览真实内容
make blog-local   # 真实内容 → build/www
make blog         # 演示内容 → build/www
make verify       # 演示内容 → build/verify-www（CI 用）
```

`mkdocs build --strict` 会把相对链接和锚点 warning 当 error。

## 5. 发布

```bash
make publish
```

流程：

```text
mkdocs build --strict
→ test -s build/www/index.html
→ ssh: cp -al www www.prev
→ rsync --delete-after --delay-updates --chmod=D755,F644
→ ssh: curl / 和 /sitemap.xml
→ 失败: rsync www.prev/ → www/
```

不重启容器。v0.4 的 LRU 按 mtime/size 失效，文件更新后下一次请求会重新读取。

## 6. 内容与镜像

- 镜像里带 `blog/demo` 构建出的 `/app/www`，只用于 CI smoke 和独立运行镜像；
- 生产 compose 用 `/opt/sjtweb-runtime/www` 只读挂载盖住它；
- bind mount 不复制镜像内容，容器内只读，不会覆盖宿主机文件；
- 如果配成 named volume，Docker 会把 demo 复制进 volume，宿主机 rsync 不会被看到。

## 7. 已知限制

- 只有一份 `www.prev` 快照，连续两次坏发布会覆盖好版本；
- `--strict` 主要校验链接和锚点，图片缺失不一定失败；
- 中文搜索效果取决于 Material 的分词；
- 没有 push-to-publish，内容发布在本机执行。
