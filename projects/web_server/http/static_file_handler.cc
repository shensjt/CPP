#include "http/static_file_handler.h"

#include <sys/stat.h>
#include <time.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <ctime>
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

std::string Trim(const std::string& text) {
    std::size_t begin = 0;
    while (begin < text.size() &&
           std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return text.substr(begin, end - begin);
}

// HTTP 日期固定用英文缩写，避免受进程 locale 影响。
std::string FormatHttpDate(std::time_t time) {
    std::tm tm{};
    ::gmtime_r(&time, &tm);

    static const char* kDays[] = {"Sun", "Mon", "Tue", "Wed",
                                  "Thu", "Fri", "Sat"};
    static const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%s, %02d %s %04d %02d:%02d:%02d GMT",
                  kDays[tm.tm_wday], tm.tm_mday, kMonths[tm.tm_mon],
                  tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buffer;
}

bool ParseHttpDate(const std::string& value, std::time_t& out) {
    std::tm tm{};
    const char* end =
        ::strptime(value.c_str(), "%a, %d %b %Y %H:%M:%S GMT", &tm);
    if (end == nullptr) return false;
    while (*end == ' ') ++end;
    if (*end != '\0') return false;

    out = ::timegm(&tm);
    return true;
}

std::string MakeETag(const struct stat& st) {
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "\"%llx-%lx-%llx\"",
                  static_cast<unsigned long long>(st.st_mtim.tv_sec),
                  static_cast<unsigned long>(st.st_mtim.tv_nsec),
                  static_cast<unsigned long long>(st.st_size));
    return buffer;
}

bool EtagMatches(const std::string& header, const std::string& etag) {
    std::size_t start = 0;
    while (start <= header.size()) {
        const std::size_t comma = header.find(',', start);
        const std::string item = Trim(header.substr(
            start, comma == std::string::npos ? std::string::npos : comma - start));

        if (item == "*" || item == etag) return true;
        if (item.size() > 2 && item.compare(0, 2, "W/") == 0 &&
            item.substr(2) == etag) {
            return true;
        }

        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return false;
}

HttpResponse MakeNotModified(const std::string& etag,
                             const std::string& last_modified) {
    HttpResponse response(304);
    response.SetHeader("ETag", etag);
    response.SetHeader("Last-Modified", last_modified);
    response.SetHeader("Cache-Control", "no-cache");
    return response;
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

    // 5. stat 当前文件；LRU 命中后比对 mtime/size。
    // key 用规范化后的绝对路径，避免不同 URL 写法重复缓存同一文件。
    struct stat st {};
    if (::stat(candidate.c_str(), &st) != 0) {
        return MakeError(404, "404 Not Found\n");
    }

    const std::string etag = MakeETag(st);
    const std::string last_modified = FormatHttpDate(st.st_mtime);

    // If-None-Match 优先；只有它不存在时才看 If-Modified-Since。
    const std::string if_none_match = request.GetHeader("If-None-Match");
    if (!if_none_match.empty()) {
        if (EtagMatches(if_none_match, etag)) {
            return MakeNotModified(etag, last_modified);
        }
    } else {
        const std::string if_modified_since =
            request.GetHeader("If-Modified-Since");
        std::time_t since = 0;
        if (!if_modified_since.empty() &&
            ParseHttpDate(if_modified_since, since) &&
            st.st_mtime <= since) {
            return MakeNotModified(etag, last_modified);
        }
    }

    const std::string key = candidate.string();
    CachedFile entry;
    const bool hit = cache_.Get(key, entry);
    const bool fresh =
        hit && entry.size == static_cast<std::uintmax_t>(st.st_size) &&
        entry.mtime_sec == st.st_mtim.tv_sec &&
        entry.mtime_nsec == st.st_mtim.tv_nsec;

    if (!fresh) {
        std::string body;
        if (!ReadFile(candidate, body)) {
            return MakeError(500, "500 Internal Server Error\n");
        }
        entry.body =
            std::make_shared<const std::string>(std::move(body));
        entry.mtime_sec = st.st_mtim.tv_sec;
        entry.mtime_nsec = st.st_mtim.tv_nsec;
        entry.size = static_cast<std::uintmax_t>(st.st_size);
        cache_.Put(key, entry, entry.body->size());
    }

    HttpResponse response(200);
    response.SetContentType(MimeType(candidate.extension().string()));
    response.SetHeader("ETag", etag);
    response.SetHeader("Last-Modified", last_modified);
    response.SetHeader("Cache-Control", "no-cache");
    response.SetBodyShared(entry.body);
    return response;
}
