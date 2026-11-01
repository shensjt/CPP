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

std::string ReasonPhrase(int status_code) {
    switch (status_code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        default: return "Unknown";
    }
}

}  // namespace

HttpResponse::HttpResponse(int status_code)
    : status_code_(status_code), reason_(ReasonPhrase(status_code)) {
    SetContentType("text/html; charset=utf-8");
}

void HttpResponse::SetStatus(int status_code) {
    status_code_ = status_code;
    reason_ = ReasonPhrase(status_code);
}

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
}

std::string HttpResponse::ToString(bool head_only) const {
    std::ostringstream out;
    out << "HTTP/1.1 " << status_code_ << ' ' << reason_ << "\r\n";

    for (const auto& [name, value] : headers_) {
        const std::string lower = ToLower(name);
        if (lower == "content-length" || lower == "connection") {
            continue;
        }
        out << name << ": " << value << "\r\n";
    }

    out << "Content-Length: " << body_.size() << "\r\n";
    out << "Connection: " << (close_ ? "close" : "keep-alive") << "\r\n";
    out << "\r\n";

    if (!head_only) {
        out << body_;
    }
    return out.str();
}
