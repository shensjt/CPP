#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "net/buffer.h"

// 简化版 RESP：
//   请求：*N\r\n$len\r\narg\r\n...，也支持 nc 用的 inline 命令
//   响应：+OK / -ERR / :int / $len / *N
//
// 解析器不保存跨连接状态：
// - 只有完整命令才消费 Buffer；
// - NeedMore 时 Buffer 原封不动，下次从同一位置继续；
// - Error 时也不消费，由上层关闭连接。
//
// 限额用于防攻击：参数个数 1024、单 bulk 64MB、单命令总大小 64MB、
// inline 行 64KB。超过直接 Error。
namespace resp {

struct Command {
    std::vector<std::string> args;
};

class Parser {
public:
    enum class Result { kNeedMore, kComplete, kError };

    static constexpr std::size_t kMaxArgs = 1024;
    static constexpr std::size_t kMaxBulkLength = 64 * 1024 * 1024;
    static constexpr std::size_t kMaxCommandBytes = 64 * 1024 * 1024;
    static constexpr std::size_t kMaxInlineLength = 64 * 1024;

    // 完整解析出一条命令才消费 Buffer；NeedMore / Error 时 Buffer 不变。
    Result Parse(Buffer& buffer, Command& command);

    const std::string& ErrorMessage() const { return error_; }

private:
    Result ParseArray(std::string_view input, Command& command,
                      std::size_t& consumed);
    Result ParseInline(std::string_view input, Command& command,
                       std::size_t& consumed);
    Result Fail(const char* message);

    std::string error_;
};

std::string SimpleString(std::string_view text);
std::string Error(std::string_view text);
std::string Integer(long long value);
std::string BulkString(std::string_view data);
std::string NullBulkString();
std::string ArrayHeader(std::size_t count);

// 把一组参数编码成 RESP 数组，AOF 追加和测试都用它。
std::string EncodeCommand(const std::vector<std::string>& args);

}  // namespace resp
