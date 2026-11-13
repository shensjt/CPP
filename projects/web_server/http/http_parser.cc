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

// 在连续内存里找 CRLF；返回指向 '\r' 的指针，找不到返回 nullptr。
// 只处理完整的一行；不跨 Buffer 边界拼接，半包交给调用方等下一次数据。
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

// 去掉首尾空格和 tab；Header 值两侧的空白不参与语义。
std::string Trim(const std::string& text) {
    const auto begin = text.find_first_not_of(" \t");
    if (begin == std::string::npos) return {};
    const auto end = text.find_last_not_of(" \t");
    return text.substr(begin, end - begin + 1);
}

// 严格解析非负十进制整数：空串、负数、尾随字符、溢出都失败。
// 不用 atoi，是因为 atoi("12abc") 会静默返回 12。
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

// 状态机主循环：每次尽量推进一个状态；数据不够就原样保留。
// 注意：kNeedMore 时不能消费 Buffer，否则下次会从请求中间开始解析。
HttpParser::Result HttpParser::Parse(Buffer& buffer, HttpRequest& request) {
    while (true) {
        if (state_ == State::kError) return Result::kError;
        if (state_ == State::kComplete) return Result::kComplete;

        if (state_ == State::kBody) {
            // Body 按 Content-Length 等够；不够就 NeedMore，一点不消费。
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
            // 没有完整一行：先看是否已经超过该状态的限额，避免恶意客户端
            // 一直发不带 CRLF 的数据把内存撑爆。
            const std::size_t limit =
                state_ == State::kRequestLine ? kMaxRequestLine : kMaxHeaderBytes;
            if (readable > limit) {
                state_ = State::kError;
                return Result::kError;
            }
            return Result::kNeedMore;
        }

        // 找到完整一行：连 CRLF 一起消费，再按状态解析。
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
            // 空行表示 Headers 结束。keep-alive 规则：
            //   HTTP/1.1 默认保持，HTTP/1.0 默认关闭；
            //   Connection: close 覆盖为关闭，keep-alive 覆盖为保持。
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

        // Content-Length 必须严格合法且不超过 1MB；
        // 重复出现但值不一致视为请求走私风险，直接拒绝。
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
            // 本项目不实现 chunked；明确拒绝，避免把 chunk 当 Body 读错。
            state_ = State::kError;
            return Result::kError;
        }

        request.headers[name] = value;
    }
}

// 处理完一个请求后复位；keep-alive 的流水线下，下一次 Parse 从请求行开始。
void HttpParser::Reset() {
    state_ = State::kRequestLine;
    content_length_ = 0;
    header_bytes_ = 0;
}
