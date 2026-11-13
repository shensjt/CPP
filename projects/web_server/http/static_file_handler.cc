#include "http/static_file_handler.h"

#include <cctype>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace {

std::string ToLower(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

bool HexValue(char c, int& value) {
    if (c >= '0' && c <= '9') {
        value = c - '0';
        return true;
    }
    if (c >= 'a' && c <= 'f') {
        value = c - 'a' + 10;
        return true;
    }
    if (c >= 'A' && c <= 'F') {
        value = c - 'A' + 10;
        return true;
    }
    return false;
}

// 解码 %XX；非法编码或解码后出现 NUL 都返回 nullopt。
// NUL 会被文件系统当成字符串结束，可能绕过后续的路径检查。
std::optional<std::string> UrlDecode(const std::string& input) {
    std::string output;
    output.reserve(input.size());

    for (std::size_t i = 0; i < input.size(); ++i) {
        if (input[i] != '%') {
            output.push_back(input[i]);
            continue;
        }
        if (i + 2 >= input.size()) return std::nullopt;

        int high = 0;
        int low = 0;
        if (!HexValue(input[i + 1], high) || !HexValue(input[i + 2], low)) {
            return std::nullopt;
        }
        output.push_back(static_cast<char>((high << 4) | low));
        i += 2;
    }

    if (output.find('\0') != std::string::npos) return std::nullopt;
    return output;
}

// 按路径组件逐段比较，而不是字符串前缀：
// /www 和 /www-evil 有共同前缀，但不是同一棵目录树。
bool IsInside(const fs::path& root, const fs::path& candidate) {
    auto root_it = root.begin();
    auto candidate_it = candidate.begin();
    for (; root_it != root.end(); ++root_it, ++candidate_it) {
        if (candidate_it == candidate.end() || *candidate_it != *root_it) {
            return false;
        }
    }
    return true;
}

// 只覆盖博客用到的类型；未知扩展名回退 application/octet-stream。
std::string MimeType(const std::string& extension) {
    const std::string ext = ToLower(extension);
    if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".js") return "application/javascript; charset=utf-8";
    if (ext == ".json") return "application/json; charset=utf-8";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".gif") return "image/gif";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".ico") return "image/x-icon";
    if (ext == ".txt") return "text/plain; charset=utf-8";
    if (ext == ".xml") return "application/xml; charset=utf-8";
    return "application/octet-stream";
}

HttpResponse MakeError(int status, std::string message) {
    HttpResponse response(status);
    response.SetContentType("text/plain; charset=utf-8");
    response.SetBody(std::move(message));
    return response;
}

bool ReadFile(const fs::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;

    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !in.bad();
}

}  // namespace

// 启动时就把 document root 规范化成绝对路径；
// 之后所有请求都相对这个 root 检查，不依赖进程当前工作目录。
StaticFileHandler::StaticFileHandler(std::string root, std::size_t cache_bytes)
    : root_(fs::weakly_canonical(fs::path(std::move(root)))),
      cache_(cache_bytes) {
    std::error_code ec;
    if (!fs::is_directory(root_, ec)) {
        throw std::invalid_argument("invalid document root");
    }
}

// 处理顺序很重要：先解码，再规范化，最后做 root 内检查。
// 如果先检查再解码，%2e%2e 这类编码可以绕过。
HttpResponse StaticFileHandler::Handle(const HttpRequest& request) const {
    if (request.method != HttpRequest::Method::kGet &&
        request.method != HttpRequest::Method::kHead) {
        HttpResponse response = MakeError(405, "405 Method Not Allowed\n");
        response.SetHeader("Allow", "GET, HEAD");
        return response;
    }

    // 1. URL 解码；非法编码/NUL 直接 400。
    const auto decoded = UrlDecode(request.Path());
    if (!decoded || decoded->empty() || decoded->front() != '/') {
        return MakeError(400, "400 Bad Request\n");
    }

    // 2. 拼接并规范化；weakly_canonical 会消掉 .. 和符号链接。
    // 3. 检查结果仍在 root_ 内，否则 403。
    std::error_code ec;
    const std::string relative = decoded->substr(1);
    fs::path candidate = fs::weakly_canonical(root_ / fs::path(relative), ec);
    if (ec || !IsInside(root_, candidate)) {
        return MakeError(403, "403 Forbidden\n");
    }

    // 4. 目录补 index.html；再确认是普通文件。
    if (fs::is_directory(candidate, ec)) {
        candidate /= "index.html";
    }
    if (ec || !fs::is_regular_file(candidate, ec)) {
        return MakeError(404, "404 Not Found\n");
    }

    // 5. 先查 LRU；未命中读文件并回填。
    // key 用规范化后的绝对路径，避免不同 URL 写法重复缓存同一文件。
    const std::string key = candidate.string();
    std::string body;
    if (!cache_.Get(key, body)) {
        if (!ReadFile(candidate, body)) {
            return MakeError(500, "500 Internal Server Error\n");
        }
        cache_.Put(key, body, body.size());
    }

    HttpResponse response(200);
    response.SetContentType(MimeType(candidate.extension().string()));
    response.SetBody(std::move(body));
    return response;
}
