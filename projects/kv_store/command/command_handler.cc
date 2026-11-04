#include "command/command_handler.h"

#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "protocol/resp.h"
#include "store/score_format.h"

namespace {

std::string ToUpper(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return out;
}

std::string ToLower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

bool ParseLongLong(std::string_view text, long long& value) {
    if (text.empty()) {
        return false;
    }
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    return ec == std::errc{} && ptr == end;
}

bool ParseDouble(std::string_view text, double& value) {
    if (text.empty()) {
        return false;
    }
    const std::string tmp(text);
    char* end = nullptr;
    errno = 0;
    const double parsed = std::strtod(tmp.c_str(), &end);
    if (end != tmp.c_str() + tmp.size() || errno == ERANGE ||
        !std::isfinite(parsed)) {
        return false;
    }
    value = parsed;
    return true;
}

// ZRANGEBYSCORE 的边界：支持 ( 前缀、-inf、+inf。
bool ParseBound(std::string_view text, double& value, bool& exclusive) {
    exclusive = false;
    if (!text.empty() && text.front() == '(') {
        exclusive = true;
        text.remove_prefix(1);
    }
    if (text == "-inf" || text == "-INF") {
        value = -std::numeric_limits<double>::infinity();
        return true;
    }
    if (text == "+inf" || text == "+INF" || text == "inf" || text == "INF") {
        value = std::numeric_limits<double>::infinity();
        return true;
    }
    return ParseDouble(text, value);
}

std::string WrongArity(const std::string& command) {
    return resp::Error("ERR wrong number of arguments for '" +
                       ToLower(command) + "' command");
}

std::string WrongType() {
    return resp::Error(
        "WRONGTYPE Operation against a key holding the wrong kind of value");
}

std::string ArrayOfBulk(const std::vector<std::string>& values) {
    std::string out = resp::ArrayHeader(values.size());
    for (const auto& value : values) {
        out += resp::BulkString(value);
    }
    return out;
}

}  // namespace

std::string CommandHandler::Execute(const std::vector<std::string>& args) {
    if (args.empty()) {
        return resp::Error("ERR empty command");
    }

    const std::string command = ToUpper(args[0]);

    if (command == "PING") {
        if (args.size() != 1) {
            return WrongArity(command);
        }
        return resp::SimpleString("PONG");
    }

    if (command == "COMMAND") {
        // redis-cli 连接时会问一次，返回空数组足够让客户端继续。
        return resp::ArrayHeader(0);
    }

    if (command == "SET") {
        if (args.size() != 3) {
            return WrongArity(command);
        }
        const auto status = store_.Set(args[1], args[2]);
        return status == KvStore::Status::kOk ? resp::SimpleString("OK")
                                              : WrongType();
    }

    if (command == "GET") {
        if (args.size() != 2) {
            return WrongArity(command);
        }
        std::string value;
        const auto status = store_.Get(args[1], &value);
        if (status == KvStore::Status::kOk) {
            return resp::BulkString(value);
        }
        if (status == KvStore::Status::kNotFound) {
            return resp::NullBulkString();
        }
        return WrongType();
    }

    if (command == "DEL") {
        if (args.size() < 2) {
            return WrongArity(command);
        }
        long long removed = 0;
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (store_.Del(args[i])) {
                ++removed;
            }
        }
        return resp::Integer(removed);
    }

    if (command == "EXISTS") {
        if (args.size() < 2) {
            return WrongArity(command);
        }
        long long count = 0;
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (store_.Exists(args[i])) {
                ++count;
            }
        }
        return resp::Integer(count);
    }

    if (command == "ZADD") {
        if (args.size() < 4 || (args.size() - 2) % 2 != 0) {
            return WrongArity(command);
        }
        std::vector<std::pair<double, std::string>> items;
        items.reserve((args.size() - 2) / 2);
        for (std::size_t i = 2; i < args.size(); i += 2) {
            double score = 0.0;
            if (!ParseDouble(args[i], score)) {
                return resp::Error("ERR value is not a valid float");
            }
            items.emplace_back(score, args[i + 1]);
        }

        std::size_t added = 0;
        const auto status = store_.ZAdd(args[1], items, &added);
        if (status == KvStore::Status::kWrongType) {
            return WrongType();
        }
        return resp::Integer(static_cast<long long>(added));
    }

    if (command == "ZSCORE") {
        if (args.size() != 3) {
            return WrongArity(command);
        }
        double score = 0.0;
        const auto status = store_.ZScore(args[1], args[2], &score);
        if (status == KvStore::Status::kOk) {
            return resp::BulkString(FormatScore(score));
        }
        if (status == KvStore::Status::kNotFound) {
            return resp::NullBulkString();
        }
        return WrongType();
    }

    if (command == "ZREM") {
        if (args.size() < 3) {
            return WrongArity(command);
        }
        const std::vector<std::string> members(args.begin() + 2, args.end());
        std::size_t removed = 0;
        const auto status = store_.ZRem(args[1], members, &removed);
        if (status == KvStore::Status::kWrongType) {
            return WrongType();
        }
        return resp::Integer(static_cast<long long>(removed));
    }

    if (command == "ZRANGE") {
        if (args.size() != 4) {
            return WrongArity(command);
        }
        long long start = 0;
        long long stop = 0;
        if (!ParseLongLong(args[2], start) || !ParseLongLong(args[3], stop)) {
            return resp::Error("ERR value is not an integer or out of range");
        }

        std::vector<std::string> members;
        const auto status = store_.ZRangeByRank(args[1], start, stop, &members);
        if (status == KvStore::Status::kWrongType) {
            return WrongType();
        }
        return ArrayOfBulk(members);
    }

    if (command == "ZRANGEBYSCORE") {
        if (args.size() != 4) {
            return WrongArity(command);
        }
        double min = 0.0;
        double max = 0.0;
        bool min_exclusive = false;
        bool max_exclusive = false;
        if (!ParseBound(args[2], min, min_exclusive) ||
            !ParseBound(args[3], max, max_exclusive)) {
            return resp::Error("ERR min or max is not a float");
        }

        std::vector<std::string> members;
        const auto status = store_.ZRangeByScore(args[1], min, min_exclusive,
                                                 max, max_exclusive, &members);
        if (status == KvStore::Status::kWrongType) {
            return WrongType();
        }
        return ArrayOfBulk(members);
    }

    return resp::Error("ERR unknown command '" + command + "'");
}
