#pragma once

#include <cctype>
#include <string>
#include <unordered_map>

// 一个解析完成的 HTTP 请求。
//
// 字段约定：
// - method：只识别 GET / POST / HEAD，其余记为 kUnknown，由上层回 405；
// - target：原始请求目标，例如 "/a/b?x=1"，不做 URL 解码；
// - headers：key 已统一转小写，value 去掉首尾空白；
// - body：按 Content-Length 读出的请求体；
// - keep_alive：由 HTTP 版本和 Connection 头共同决定，解析器写入。
struct HttpRequest {
    enum class Method { kGet, kPost, kHead, kUnknown };

    Method method = Method::kUnknown;
    std::string target;
    std::string version;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    bool keep_alive = true;

    // 去掉 ?query 后的路径；不负责 URL 解码，解码在 StaticFileHandler 里做。
    std::string Path() const {
        const auto pos = target.find('?');
        return pos == std::string::npos ? target : target.substr(0, pos);
    }

    // ? 后面的原始 query；没有 query 时返回空串。
    std::string Query() const {
        const auto pos = target.find('?');
        return pos == std::string::npos ? std::string() : target.substr(pos + 1);
    }

    // Header 名大小写不敏感：调用方可以传 "Host" 或 "host"。
    // 解析器已经把 key 存成小写，这里再把查询名转小写。
    std::string GetHeader(const std::string& name) const {
        std::string key = name;
        for (char& c : key) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        const auto it = headers.find(key);
        return it == headers.end() ? std::string() : it->second;
    }

    // 复用对象前清空；keep_alive 恢复默认 true，由下一次解析重新决定。
    void Clear() {
        method = Method::kUnknown;
        target.clear();
        version.clear();
        headers.clear();
        body.clear();
        keep_alive = true;
    }
};
