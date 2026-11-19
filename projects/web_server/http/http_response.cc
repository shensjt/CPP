#include "http/http_response.h"

#include <cctype>
#include <sstream>
#include <string>

namespace {

std::string ToLower(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

// 只覆盖项目实际用到的状态码；未知状态码输出 "Unknown"。
std::string ReasonPhrase(int status_code) {
    switch (status_code) {
        case 200: return "OK";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        default: return "Unknown";
    }
}

}  // namespace

// 默认 Content-Type 是 HTML；返回文件/文本时由调用方覆盖。
HttpResponse::HttpResponse(int status_code)
    : status_code_(status_code), reason_(ReasonPhrase(status_code)) {
    SetContentType("text/html; charset=utf-8");
}

void HttpResponse::SetStatus(int status_code) {
    status_code_ = status_code;
    reason_ = ReasonPhrase(status_code);
}

// Header 名大小写不敏感：同名覆盖 value，但保留第一次写入的 key 写法。
// 用 vector 而不是 map，保证输出顺序和插入顺序一致。
void HttpResponse::SetHeader(const std::string& name, const std::string& value) {
    const std::string lower = ToLower(name);
    for (auto& [key, existing] : headers_) {
        if (ToLower(key) == lower) {
            existing = value;
            return;
        }
    }
    headers_.emplace_back(name, value);
}

void HttpResponse::SetContentType(const std::string& type) {
    SetHeader("Content-Type", type);
}

void HttpResponse::SetBody(std::string body) {
    body_ = std::move(body);
    shared_body_.reset();
}

void HttpResponse::SetBodyShared(std::shared_ptr<const std::string> body) {
    shared_body_ = std::move(body);
    body_.clear();
}

// Content-Length / Connection 由这里统一生成，调用方手动设置的同名头会被忽略，
// 避免出现两个 Content-Length 或连接语义冲突。
std::string HttpResponse::HeaderToString() const {
    std::ostringstream out;
    out << "HTTP/1.1 " << status_code_ << ' ' << reason_ << "\r\n";

    for (const auto& [name, value] : headers_) {
        const std::string lower = ToLower(name);
        if (lower == "content-length" || lower == "connection") {
            continue;
        }
        out << name << ": " << value << "\r\n";
    }

    // 304 不允许带消息体；也不应发送与 200 不同的 Content-Length。
    if (status_code_ != 304) {
        out << "Content-Length: " << Body().size() << "\r\n";
    }
    out << "Connection: " << (close_ ? "close" : "keep-alive") << "\r\n";
    out << "\r\n";
    return out.str();
}

// head_only 只影响 Body 输出，Content-Length 仍然是 Body() 的真实长度。
std::string HttpResponse::ToString(bool head_only) const {
    std::string out = HeaderToString();
    if (!head_only && status_code_ != 304) {
        out += Body();
    }
    return out;
}
