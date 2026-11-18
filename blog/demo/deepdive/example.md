# 一篇博客是怎么变成页面的

这是一篇演示文章，用来展示站点的排版和构建方式。

## 从 Markdown 开始

文章用 Markdown 写，放在 `blog/demo/` 或 `blog/content/`，由 MkDocs 生成静态 HTML。

![示例图](example.svg)

- 列表项一
- 列表项二
- [项目仓库](https://github.com/shensjt/CPP)

## 代码块

```cpp
#include <iostream>

int main() {
    std::cout << "hello\n";
    return 0;
}
```

## 引用

> 这是一段引用，用来检查引用块的样式。

MkDocs 会把 Markdown 链接改写成 HTML URL，再交给 `web_server` 按路径返回。
