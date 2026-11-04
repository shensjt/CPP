// RESP 协议测试（RP 系列）

#include "protocol/resp.h"

#include <cstddef>
#include <string>
#include <vector>

#include "net/buffer.h"
#include "test_framework.h"

namespace {

void ExpectCommand(const std::string& wire,
                   const std::vector<std::string>& expected) {
    Buffer buffer;
    buffer.Append(wire);

    resp::Parser parser;
    resp::Command command;
    CHECK(parser.Parse(buffer, command) == resp::Parser::Result::kComplete);
    CHECK(command.args == expected);
    CHECK_EQ(buffer.ReadableBytes(), std::size_t{0});
}

}  // namespace

// RP1：标准的 RESP 数组请求。
TEST(Resp_ParseArray) {
    ExpectCommand("*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n",
                  {"SET", "k", "v"});
}

// RP2：半包时返回 NeedMore，Buffer 不被消费。
TEST(Resp_PartialArray) {
    Buffer buffer;
    buffer.Append("*3\r\n$3\r\nSET\r\n$1\r\nk");
    const std::size_t before = buffer.ReadableBytes();

    resp::Parser parser;
    resp::Command command;
    CHECK(parser.Parse(buffer, command) == resp::Parser::Result::kNeedMore);
    CHECK_EQ(buffer.ReadableBytes(), before);
    CHECK(command.args.empty());

    buffer.Append("\r\n$1\r\nv\r\n");
    CHECK(parser.Parse(buffer, command) == resp::Parser::Result::kComplete);
    CHECK(command.args == std::vector<std::string>({"SET", "k", "v"}));
    CHECK_EQ(buffer.ReadableBytes(), std::size_t{0});
}

// RP3：inline 命令，兼容 nc 手测；空格和 tab 都算分隔符。
TEST(Resp_ParseInline) {
    ExpectCommand("PING\r\n", {"PING"});
    ExpectCommand("SET k v\n", {"SET", "k", "v"});
    ExpectCommand("  SET   k\tv  \r\n", {"SET", "k", "v"});
}

// RP4：bulk string 按长度读取，内部可以有 \0 和 CRLF。
TEST(Resp_BinaryBulk) {
    const std::string payload("a\0b\r\nc", 6);
    ExpectCommand("*1\r\n$6\r\n" + payload + "\r\n", {payload});
}

// RP5：长度不是数字 -> Error，Buffer 不消费。
TEST(Resp_InvalidLength) {
    Buffer buffer;
    buffer.Append("*1\r\n$x\r\n");
    const std::size_t before = buffer.ReadableBytes();

    resp::Parser parser;
    resp::Command command;
    CHECK(parser.Parse(buffer, command) == resp::Parser::Result::kError);
    CHECK_EQ(buffer.ReadableBytes(), before);
    CHECK(!parser.ErrorMessage().empty());
}

// RP6：bulk 数据结尾不是 CRLF -> Error。
TEST(Resp_InvalidTerminator) {
    Buffer buffer;
    buffer.Append("*1\r\n$1\r\naXX");

    resp::Parser parser;
    resp::Command command;
    CHECK(parser.Parse(buffer, command) == resp::Parser::Result::kError);
}

// RP7：空 inline 行返回空命令，由命令层报错。
TEST(Resp_EmptyInline) {
    Buffer buffer;
    buffer.Append("\r\n");

    resp::Parser parser;
    resp::Command command;
    CHECK(parser.Parse(buffer, command) == resp::Parser::Result::kComplete);
    CHECK(command.args.empty());
    CHECK_EQ(buffer.ReadableBytes(), std::size_t{0});
}

// RP8：一个 Buffer 里两条命令，解析器只消费第一条。
TEST(Resp_Pipelined) {
    const std::string wire =
        "*1\r\n$4\r\nPING\r\n"
        "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n";
    Buffer buffer;
    buffer.Append(wire);

    resp::Parser parser;
    resp::Command command;
    CHECK(parser.Parse(buffer, command) == resp::Parser::Result::kComplete);
    CHECK(command.args == std::vector<std::string>({"PING"}));
    CHECK_EQ(buffer.ReadableBytes(), std::size_t{wire.size() - 14});

    CHECK(parser.Parse(buffer, command) == resp::Parser::Result::kComplete);
    CHECK(command.args == std::vector<std::string>({"GET", "k"}));
    CHECK_EQ(buffer.ReadableBytes(), std::size_t{0});
}

// RP9：参数个数超限 -> Error。
TEST(Resp_TooManyArgs) {
    Buffer buffer;
    buffer.Append("*1025\r\n");

    resp::Parser parser;
    resp::Command command;
    CHECK(parser.Parse(buffer, command) == resp::Parser::Result::kError);
}

// RP10：响应序列化的精确格式。
TEST(Resp_Serialize) {
    CHECK_EQ(resp::SimpleString("OK"), std::string("+OK\r\n"));
    CHECK_EQ(resp::Error("ERR bad"), std::string("-ERR bad\r\n"));
    CHECK_EQ(resp::Integer(-3), std::string(":-3\r\n"));
    CHECK_EQ(resp::BulkString("abc"), std::string("$3\r\nabc\r\n"));
    CHECK_EQ(resp::NullBulkString(), std::string("$-1\r\n"));
    CHECK_EQ(resp::ArrayHeader(2), std::string("*2\r\n"));
}

// RP11：请求里出现 null bulk 长度 -> Error。
TEST(Resp_NegativeBulkLength) {
    Buffer buffer;
    buffer.Append("*1\r\n$-1\r\n");

    resp::Parser parser;
    resp::Command command;
    CHECK(parser.Parse(buffer, command) == resp::Parser::Result::kError);
}

// RP12：bulk 数据只到了一部分 -> NeedMore。
TEST(Resp_PartialBulk) {
    Buffer buffer;
    buffer.Append("*1\r\n$5\r\nhel");

    resp::Parser parser;
    resp::Command command;
    CHECK(parser.Parse(buffer, command) == resp::Parser::Result::kNeedMore);
}

// RP13：0 个参数的数组请求非法。
TEST(Resp_ZeroArgs) {
    Buffer buffer;
    buffer.Append("*0\r\n");

    resp::Parser parser;
    resp::Command command;
    CHECK(parser.Parse(buffer, command) == resp::Parser::Result::kError);
}
