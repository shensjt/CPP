---
title: "示例：一篇博客是怎么变成页面的"
date: "2026-11-01"
category: "deepdive"
tags: ["示例", "静态站点"]
summary: "用一篇示例文章展示博客的排版和构建流程：标题、列表、代码块、引用和链接。"
---

这是一篇示例文章，用来展示博客的排版和构建方式，不包含任何个人内容。

## 从 Markdown 开始

文章用 Markdown 写，开头是 front matter，描述标题、日期、分类和标签：

```yaml
title: "示例文章"
date: "2026-11-01"
category: "deepdive"
tags: ["示例"]
```

正文支持标题、段落、列表、引用和代码块。

- 列表项一
- 列表项二

## 构建流程

构建脚本会把 Markdown 转成 HTML，再套上模板：

```text
posts/*.md → build.py → post.html → base.html → www/posts/*.html
```

最后，静态文件服务器按 URL 返回对应的 HTML。比如：

```text
GET /posts/example.html
```

> 这是一段引用，用来检查引用块的样式。

更多细节可以看[项目仓库](https://example.com)。
