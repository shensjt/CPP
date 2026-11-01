#include "http/http_parser.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <string>
#include <system_error>

namespace {

constexpr std::size_t kMaxRequestLine = 8 * 1024;
constexpr std::size_t kMaxHeaderBytes = 32 * 1024;
constexpr std::size_t kMaxBodyBytes = 1024 * 1024;

const char* FindCrlf(const char* data, std::size_t len) {
    for (std::size_t i = 0; i + 1 < len; ++i) {
        if (data[i] == '\r' && data[i + 1] == '\n') {
            return data + i;
        }
    }
    return nullptr;
}

std::string ToLower(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

std::string Trim(const std::string& text) {
    const auto begin = text.find_first_not_of(" \t");
    if (begin == std::string::npos) return {};
    const auto end = text.find_last_not_of(" \t");
    return text.substr(begin, end - begin + 1);
}

bool ParseSize(const std::string& text, std::size_t& value) {
    if (text.empty()) return false;
    std::size_t parsed = 0;
    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc() || result.ptr != end) return false;
    value = parsed;
    return true;
}

HttpRequest::Method ParseMethod(const std::string& method) {
    if (method == "GET") return HttpRequest::Method::kGet;
    if (method == "POST") return HttpRequest::Method::kPost;
    if (method == "HEAD") return HttpRequest::Method::kHead;
    return HttpRequest::Method::kUnknown;
}

}  // namespace

HttpParser::Result HttpParser::Parse(Buffer& buffer, HttpRequest& request) {
    while (true) {
        if (state_ == State::kError) return Result::kError;
        if (state_ == State::kComplete) return Result::kComplete;

        if (state_ == State::kBody) {
            if (buffer.ReadableBytes() < content_length_) {
                return Result::kNeedMore;
            }
            request.body = buffer.RetrieveAsString(content_length_);
            state_ = State::kComplete;
            return Result::kComplete;
        }

        const char* data = buffer.Peek();
        const std::size_t readable = buffer.ReadableBytes();
        const char* crlf = FindCrlf(data, readable);
        if (crlf == nullptr) {
            const std::size_t limit =
                state_ == State::kRequestLine ? kMaxRequestLine : kMaxHeaderBytes;
            if (readable > limit) {
                state_ = State::kError;
                return Result::kError;
            }
            return Result::kNeedMore;
        }

        const std::size_t line_size = static_cast<std::size_t>(crlf - data) + 2;
        if (state_ == State::kRequestLine && line_size > kMaxRequestLine) {
            state_ = State::kError;
            return Result::kError;
        }

        std::string line = buffer.RetrieveAsString(line_size);
        line.resize(line.size() - 2);  // 去掉 CRLF

        if (state_ == State::kRequestLine) {
            const auto first_space = line.find(' ');
            const auto second_space =
                first_space == std::string::npos ? std::string::npos
                                                 : line.find(' ', first_space + 1);
            if (first_space == std::string::npos || second_space == std::string::npos) {
                state_ = State::kError;
                return Result::kError;
            }

            const std::string method = line.substr(0, first_space);
            const std::string target =
                line.substr(first_space + 1, second_space - first_space - 1);
            const std::string version = line.substr(second_space + 1);

            if (target.empty() || target.front() != '/' ||
                (version != "HTTP/1.0" && version != "HTTP/1.1")) {
                state_ = State::kError;
                return Result::kError;
            }

            request.method = ParseMethod(method);
            request.target = target;
            request.version = version;
            state_ = State::kHeaders;
            continue;
        }

        // kHeaders
        header_bytes_ += line_size;
        if (header_bytes_ > kMaxHeaderBytes) {
            state_ = State::kError;
            return Result::kError;
        }

        if (line.empty()) {
            request.keep_alive = request.version == "HTTP/1.1";
            const std::string connection = ToLower(request.GetHeader("connection"));
            if (connection == "close") {
                request.keep_alive = false;
            } else if (connection == "keep-alive") {
                request.keep_alive = true;
            }
            state_ = State::kBody;
            continue;
        }

        const auto colon = line.find(':');
        if (colon == std::string::npos || colon == 0) {
            state_ = State::kError;
            return Result::kError;
        }

        const std::string name = ToLower(Trim(line.substr(0, colon)));
        const std::string value = Trim(line.substr(colon + 1));
        if (name.empty()) {
            state_ = State::kError;
            return Result::kError;
        }

        if (name == "content-length") {
            std::size_t length = 0;
            if (!ParseSize(value, length) || length > kMaxBodyBytes) {
                state_ = State::kError;
                return Result::kError;
            }
            const auto existing = request.headers.find(name);
            if (existing != request.headers.end() && existing->second != value) {
                state_ = State::kError;
                return Result::kError;
            }
            content_length_ = length;
        } else if (name == "transfer-encoding" &&
                   ToLower(value).find("chunked") != std::string::npos) {
            state_ = State::kError;
            return Result::kError;
        }

        request.headers[name] = value;
    }
}

void HttpParser::Reset() {
    state_ = State::kRequestLine;
    content_length_ = 0;
    header_bytes_ = 0;
}
