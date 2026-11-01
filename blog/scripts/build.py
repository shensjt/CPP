#!/usr/bin/env python3
"""Build the static blog from Markdown into projects/web_server/www."""

import argparse
import html
import math
import re
import shutil
from datetime import date
from pathlib import Path

import yaml
from pygments import highlight
from pygments.formatters import HtmlFormatter
from pygments.lexers import TextLexer, get_lexer_by_name
from pygments.util import ClassNotFound

BLOG = Path(__file__).resolve().parent.parent
ROOT = BLOG.parent

CATEGORY_NAMES = {
    "devlog": "开发日志",
    "deepdive": "技术深潜",
    "career": "校招复盘",
    "release": "版本发布",
}
CATEGORY_ORDER = ["devlog", "deepdive", "career", "release"]


def load_config():
    with (BLOG / "config.yaml").open(encoding="utf-8") as f:
        return yaml.safe_load(f)


def parse_post(path):
    raw = path.read_text(encoding="utf-8")
    if not raw.startswith("---\n"):
        raise ValueError(f"missing front matter: {path}")
    _, front, body = raw.split("---\n", 2)
    meta = yaml.safe_load(front) or {}
    meta["body"] = body.lstrip("\n")
    meta["title"] = str(meta.get("title", path.stem))
    meta["date"] = str(meta.get("date", "1970-01-01"))
    meta["category"] = meta.get("category") or path.parent.name
    meta["tags"] = meta.get("tags") or []
    meta["summary"] = meta.get("summary") or make_summary(meta["body"])
    meta["slug"] = path.stem
    return meta


def make_summary(body):
    for line in body.splitlines():
        line = line.strip()
        if line and not line.startswith(("#", "```", ">", "-", "1.")):
            line = re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", line)
            return re.sub(r"[`*_\[\]]", "", line)[:120]
    return ""


def parse_date(text):
    try:
        return date.fromisoformat(text)
    except ValueError:
        return date.min


def reading_minutes(body):
    plain = re.sub(r"```.*?```", "", body, flags=re.S)
    cjk = len(re.findall(r"[一-鿿]", plain))
    words = len(re.findall(r"[A-Za-z0-9_]+", plain))
    return max(1, math.ceil(cjk / 300 + words / 200))


def inline(text):
    codes = []

    def stash(match):
        codes.append(match.group(1))
        return f"\x00C{len(codes) - 1}\x00"

    text = re.sub(r"`([^`]+)`", stash, text)
    text = html.escape(text, quote=False)
    text = re.sub(r"\*\*(.+?)\*\*", r"<strong>\1</strong>", text)
    text = re.sub(
        r"\[([^\]]+)\]\((https?://[^)\s]+)\)",
        r'<a href="\2" target="_blank" rel="noopener">\1</a>',
        text,
    )
    for index, code in enumerate(codes):
        text = text.replace(
            f"\x00C{index}\x00", f"<code>{html.escape(code, quote=False)}</code>"
        )
    return text


def render_code(lang, code):
    try:
        lexer = get_lexer_by_name(lang) if lang else TextLexer()
    except ClassNotFound:
        lexer = TextLexer()
    formatter = HtmlFormatter(cssclass="codehilite", style="github-dark")
    return highlight(code.rstrip("\n"), lexer, formatter)


def markdown_to_html(body):
    lines = body.splitlines()
    blocks = []
    index = 0

    while index < len(lines):
        line = lines[index]
        if not line.strip():
            index += 1
            continue

        if line.startswith("```"):
            lang = line[3:].strip()
            index += 1
            buffer = []
            while index < len(lines) and not lines[index].startswith("```"):
                buffer.append(lines[index])
                index += 1
            index += 1
            blocks.append(render_code(lang, "\n".join(buffer)))
            continue

        match = re.match(r"^(#{2,3})\s+(.*)$", line)
        if match:
            level = len(match.group(1))
            blocks.append(f"<h{level}>{inline(match.group(2))}</h{level}>")
            index += 1
            continue

        if line.startswith("> "):
            buffer = []
            while index < len(lines) and lines[index].startswith("> "):
                buffer.append(lines[index][2:])
                index += 1
            blocks.append(f"<blockquote>{inline(' '.join(buffer))}</blockquote>")
            continue

        if re.match(r"^-\s+", line):
            items = []
            while index < len(lines) and re.match(r"^-\s+", lines[index]):
                items.append(inline(re.sub(r"^-\s+", "", lines[index])))
                index += 1
            blocks.append("<ul>" + "".join(f"<li>{item}</li>" for item in items) + "</ul>")
            continue

        if re.match(r"^\d+\.\s+", line):
            items = []
            while index < len(lines) and re.match(r"^\d+\.\s+", lines[index]):
                items.append(inline(re.sub(r"^\d+\.\s+", "", lines[index])))
                index += 1
            blocks.append("<ol>" + "".join(f"<li>{item}</li>" for item in items) + "</ol>")
            continue

        buffer = [line]
        index += 1
        while index < len(lines):
            nxt = lines[index]
            if (
                not nxt.strip()
                or nxt.startswith("```")
                or re.match(r"^#{2,3}\s+", nxt)
                or nxt.startswith("> ")
                or re.match(r"^-\s+", nxt)
                or re.match(r"^\d+\.\s+", nxt)
            ):
                break
            buffer.append(nxt)
            index += 1
        blocks.append(f"<p>{inline(' '.join(buffer))}</p>")

    return "\n".join(blocks)


def template(name, **values):
    text = (BLOG / "templates" / name).read_text(encoding="utf-8")
    for key, value in values.items():
        text = text.replace("{{" + key + "}}", value)
    return text


def page(config, title, content):
    return template(
        "base.html",
        page_title=html.escape(title),
        site_title=html.escape(config["site"]["title"]),
        author=html.escape(config["site"]["author"]),
        github=html.escape(config["site"]["github"]),
        description=html.escape(config["site"]["description"]),
        year=str(date.today().year),
        content=content,
    )


def post_card(post):
    name = CATEGORY_NAMES.get(post["category"], post["category"])
    return f"""<article class="post-card">
  <div class="post-meta">{html.escape(post["date"])} · {html.escape(name)}</div>
  <h2><a href="/posts/{post["slug"]}.html">{html.escape(post["title"])}</a></h2>
  <p>{html.escape(post["summary"])}</p>
</article>"""


def render_post(config, post, newer, older):
    name = CATEGORY_NAMES.get(post["category"], post["category"])
    tags = " · ".join(html.escape(tag) for tag in post["tags"])
    meta = f"{post['date']} · {name} · 约 {reading_minutes(post['body'])} 分钟"
    if tags:
        meta += f" · {tags}"

    prev_next = []
    if newer:
        prev_next.append(
            f'<a href="/posts/{newer["slug"]}.html"><span>上一篇</span>{html.escape(newer["title"])}</a>'
        )
    if older:
        prev_next.append(
            f'<a class="next" href="/posts/{older["slug"]}.html"><span>下一篇</span>{html.escape(older["title"])}</a>'
        )

    content = template(
        "post.html",
        title=html.escape(post["title"]),
        meta=meta,
        body=markdown_to_html(post["body"]),
        prev_next="".join(prev_next) if prev_next else "<span></span>",
    )
    return page(config, post["title"], content)


def render_list(config, category, posts):
    name = CATEGORY_NAMES[category]
    cards = "\n".join(post_card(post) for post in posts)
    content = template(
        "list.html",
        category=name,
        post_list=cards or '<p class="empty">还没有文章。</p>',
    )
    return page(config, name, content)


def render_index(config, posts):
    cards = "\n".join(post_card(post) for post in posts[:5])
    categories = "\n".join(
        f'<a class="category-card" href="/{category}.html">'
        f"<h3>{CATEGORY_NAMES[category]}</h3><p>{count} 篇</p></a>"
        for category, count in (
            (category, sum(1 for post in posts if post["category"] == category))
            for category in CATEGORY_ORDER
        )
    )
    content = template(
        "index.html",
        description=html.escape(config["site"]["description"]),
        post_list=cards or '<p class="empty">还没有文章。</p>',
        categories=categories,
    )
    return page(config, "首页", content)


def build(out_dir, content_dir):
    config = load_config()
    posts_dir = content_dir / "posts"
    if not posts_dir.is_dir():
        raise SystemExit(f"content posts dir not found: {posts_dir}")

    post_paths = sorted(posts_dir.glob("**/*.md"))
    posts = [parse_post(path) for path in post_paths]
    posts = [post for post in posts if not post.get("draft", False)]
    posts.sort(key=lambda post: parse_date(post["date"]), reverse=True)

    if out_dir.exists():
        shutil.rmtree(out_dir)
    (out_dir / "posts").mkdir(parents=True)
    (out_dir / "css").mkdir(parents=True)

    for index, post in enumerate(posts):
        newer = posts[index - 1] if index > 0 else None
        older = posts[index + 1] if index + 1 < len(posts) else None
        (out_dir / "posts" / f"{post['slug']}.html").write_text(
            render_post(config, post, newer, older), encoding="utf-8"
        )

    for category in CATEGORY_ORDER:
        category_posts = [post for post in posts if post["category"] == category]
        (out_dir / f"{category}.html").write_text(
            render_list(config, category, category_posts), encoding="utf-8"
        )

    (out_dir / "index.html").write_text(render_index(config, posts), encoding="utf-8")

    about_path = content_dir / "about.md"
    if about_path.exists():
        about = parse_post(about_path)
        content = template(
            "post.html",
            title=html.escape(about["title"]),
            meta="",
            body=markdown_to_html(about["body"]),
            prev_next="<span></span>",
        )
        (out_dir / "about.html").write_text(
            page(config, about["title"], content), encoding="utf-8"
        )

    pygments_css = HtmlFormatter(style="github-dark").get_style_defs(".codehilite")
    css = (BLOG / "templates" / "css" / "style.css").read_text(encoding="utf-8")
    (out_dir / "css" / "style.css").write_text(css + "\n" + pygments_css, encoding="utf-8")

    favicon = BLOG / "templates" / "favicon.svg"
    if favicon.exists():
        shutil.copy2(favicon, out_dir / "favicon.svg")

    assets = BLOG / "assets"
    if assets.exists():
        shutil.copytree(assets, out_dir / "assets", dirs_exist_ok=True)

    print(f"built {len(posts)} posts from {content_dir} into {out_dir}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--out",
        type=Path,
        default=ROOT / "projects" / "web_server" / "www",
        help="output directory",
    )
    parser.add_argument(
        "--content",
        type=Path,
        default=BLOG,
        help="content directory containing posts/ and about.md",
    )
    args = parser.parse_args()
    build(args.out.resolve(), args.content.resolve())


if __name__ == "__main__":
    main()
