#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <string>

#include "http/http_request.h"
#include "http/http_response.h"
#include "lru_cache/lru_cache.h"

// 把 URL 路径映射到 document root 下的文件，只允许 GET/HEAD。
//
// 安全流程：
//   1. URL 解码；
//   2. weakly_canonical 规范化（消掉 .. 和符号链接）；
//   3. 按路径组件检查结果仍在 root_ 内；
//   4. 目录则补 index.html，再检查是否普通文件。
//
// 缓存：
// - 按字节数做 LRU，key 是规范化后的绝对路径；
// - 命中后比对 mtime/size，文件更新后自动失效；
// - Handle 是 const，但 cache_ 是 mutable，读时也要写入缓存。
class StaticFileHandler {
public:
    explicit StaticFileHandler(std::string root,
                               std::size_t cache_bytes = 16 * 1024 * 1024);

    HttpResponse Handle(const HttpRequest& request) const;

private:
    struct CachedFile {
        std::string body;
        std::time_t mtime_sec = 0;
        long mtime_nsec = 0;
        std::uintmax_t size = 0;
    };

    std::filesystem::path root_;
    mutable LruCache<std::string, CachedFile> cache_;
};
