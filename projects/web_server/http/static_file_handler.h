#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

#include "http/http_request.h"
#include "http/http_response.h"
#include "lru_cache/lru_cache.h"

// 把 URL 路径映射到 document root 下的文件，只允许 GET/HEAD。
// 文件内容按字节数做 LRU 缓存；更新文件后需要重启或清缓存。
class StaticFileHandler {
public:
    explicit StaticFileHandler(std::string root,
                               std::size_t cache_bytes = 16 * 1024 * 1024);

    HttpResponse Handle(const HttpRequest& request) const;

private:
    std::filesystem::path root_;
    mutable LruCache<std::string, std::string> cache_;
};
