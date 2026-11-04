#include "protocol/resp.h"

#include <charconv>
#include <cstddef>
#include <string>
#include <string_view>
#include <system_error>

namespace {

constexpr std::size_t kMaxHeaderLine = 64;

bool ParseInteger(std::string_view text, long long& value) {
    if (text.empty()) {
        return false;
    }
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    return ec == std::errc{} && ptr == end;
}

}  // namespace

namespace resp {

Parser::Result Parser::Fail(const char* message) {
    error_ = message;
    return Result::kError;
}

Parser::Result Parser::Parse(Buffer& buffer, Command& command) {
    error_.clear();
    command.args.clear();

    if (buffer.ReadableBytes() == 0) {
        return Result::kNeedMore;
    }

    const std::string_view input(buffer.Peek(), buffer.ReadableBytes());
    std::size_t consumed = 0;
    const Result result = input.front() == '*'
                              ? ParseArray(input, command, consumed)
                              : ParseInline(input, command, consumed);
    if (result == Result::kComplete) {
        buffer.Retrieve(consumed);
    }
    return result;
}

Parser::Result Parser::ParseArray(std::string_view input, Command& command,
                                  std::size_t& consumed) {
    const std::size_t count_end = input.find("\r\n");
    if (count_end == std::string_view::npos) {
        return input.size() > kMaxHeaderLine ? Fail("invalid multibulk length")
                                             : Result::kNeedMore;
    }

    long long count = 0;
    if (!ParseInteger(input.substr(1, count_end - 1), count) || count <= 0 ||
        static_cast<std::size_t>(count) > kMaxArgs) {
        return Fail("invalid multibulk length");
    }

    std::size_t pos = count_end + 2;
    std::size_t total = 0;
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(count));

    for (long long i = 0; i < count; ++i) {
        if (pos >= input.size()) {
            return Result::kNeedMore;
        }
        if (input[pos] != '$') {
            return Fail("expected '$'");
        }

        const std::size_t length_end = input.find("\r\n", pos + 1);
        if (length_end == std::string_view::npos) {
            return input.size() - pos > kMaxHeaderLine
                       ? Fail("invalid bulk length")
                       : Result::kNeedMore;
        }

        long long length = 0;
        if (!ParseInteger(input.substr(pos + 1, length_end - pos - 1), length) ||
            length < 0 || static_cast<std::size_t>(length) > kMaxBulkLength) {
            return Fail("invalid bulk length");
        }

        const std::size_t data_start = length_end + 2;
        const std::size_t data_size = static_cast<std::size_t>(length);
        if (data_start > input.size() ||
            input.size() - data_start < data_size + 2) {
            return Result::kNeedMore;
        }
        const std::size_t data_end = data_start + data_size;
        if (input[data_end] != '\r' || input[data_end + 1] != '\n') {
            return Fail("invalid bulk terminator");
        }

        if (total > kMaxCommandBytes || data_size > kMaxCommandBytes - total) {
            return Fail("command too large");
        }
        total += data_size;

        args.emplace_back(input.substr(data_start, data_size));
        pos = data_end + 2;
    }

    command.args = std::move(args);
    consumed = pos;
    return Result::kComplete;
}

Parser::Result Parser::ParseInline(std::string_view input, Command& command,
                                   std::size_t& consumed) {
    const std::size_t newline = input.find('\n');
    if (newline == std::string_view::npos) {
        return input.size() > kMaxInlineLength ? Fail("inline command too long")
                                               : Result::kNeedMore;
    }

    std::size_t line_end = newline;
    if (line_end > 0 && input[line_end - 1] == '\r') {
        --line_end;
    }
    if (line_end > kMaxInlineLength) {
        return Fail("inline command too long");
    }

    const std::string_view line = input.substr(0, line_end);
    std::vector<std::string> args;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
            ++i;
        }
        if (i >= line.size()) {
            break;
        }
        const std::size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t') {
            ++i;
        }
        if (args.size() >= kMaxArgs) {
            return Fail("too many arguments");
        }
        args.emplace_back(line.substr(start, i - start));
    }

    command.args = std::move(args);
    consumed = newline + 1;
    return Result::kComplete;
}

std::string SimpleString(std::string_view text) {
    return "+" + std::string(text) + "\r\n";
}

std::string Error(std::string_view text) {
    return "-" + std::string(text) + "\r\n";
}

std::string Integer(long long value) {
    return ":" + std::to_string(value) + "\r\n";
}

std::string BulkString(std::string_view data) {
    return "$" + std::to_string(data.size()) + "\r\n" + std::string(data) +
           "\r\n";
}

std::string NullBulkString() { return "$-1\r\n"; }

std::string ArrayHeader(std::size_t count) {
    return "*" + std::to_string(count) + "\r\n";
}

}  // namespace resp
