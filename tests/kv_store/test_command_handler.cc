// 命令分发测试（CH 系列）

#include "command/command_handler.h"

#include <initializer_list>
#include <string>
#include <vector>

#include "net/buffer.h"
#include "protocol/resp.h"
#include "test_framework.h"

namespace {

std::string Run(CommandHandler& handler, std::initializer_list<std::string> args) {
    return handler.Execute(std::vector<std::string>(args));
}

// 模拟服务端循环：解析 Buffer 里的所有完整命令并执行。
std::string RunWire(CommandHandler& handler, Buffer& buffer) {
    resp::Parser parser;
    resp::Command command;
    std::string out;

    while (true) {
        const auto result = parser.Parse(buffer, command);
        if (result == resp::Parser::Result::kNeedMore) {
            break;
        }
        if (result == resp::Parser::Result::kError) {
            out += resp::Error("ERR Protocol error: " + parser.ErrorMessage());
            break;
        }
        out += handler.Execute(command.args);
    }
    return out;
}

}  // namespace

// CH1：PING。
TEST(CommandHandler_Ping) {
    KvStore store;
    CommandHandler handler(store);
    CHECK_EQ(Run(handler, {"PING"}), std::string("+PONG\r\n"));
}

// CH2：SET / GET。
TEST(CommandHandler_SetGet) {
    KvStore store;
    CommandHandler handler(store);

    CHECK_EQ(Run(handler, {"SET", "name", "alice"}),
             std::string("+OK\r\n"));
    CHECK_EQ(Run(handler, {"GET", "name"}),
             std::string("$5\r\nalice\r\n"));
}

// CH3：GET 不存在的 key 返回 null bulk。
TEST(CommandHandler_GetMissing) {
    KvStore store;
    CommandHandler handler(store);
    CHECK_EQ(Run(handler, {"GET", "missing"}), std::string("$-1\r\n"));
}

// CH4：参数个数不对。
TEST(CommandHandler_WrongArity) {
    KvStore store;
    CommandHandler handler(store);
    CHECK_EQ(Run(handler, {"SET", "k"}),
             std::string(
                 "-ERR wrong number of arguments for 'set' command\r\n"));
}

// CH5：未知命令。
TEST(CommandHandler_UnknownCommand) {
    KvStore store;
    CommandHandler handler(store);
    CHECK_EQ(Run(handler, {"FOO"}), std::string("-ERR unknown command 'FOO'\r\n"));
}

// CH6：ZADD 返回值、ZSCORE 格式。
TEST(CommandHandler_ZAddZScore) {
    KvStore store;
    CommandHandler handler(store);

    CHECK_EQ(Run(handler, {"ZADD", "z", "1.5", "a"}),
             std::string(":1\r\n"));
    CHECK_EQ(Run(handler, {"ZSCORE", "z", "a"}),
             std::string("$3\r\n1.5\r\n"));

    // 更新已有 member 的 score，新增数为 0。
    CHECK_EQ(Run(handler, {"ZADD", "z", "2.5", "a"}),
             std::string(":0\r\n"));
    CHECK_EQ(Run(handler, {"ZSCORE", "z", "a"}),
             std::string("$3\r\n2.5\r\n"));
    CHECK_EQ(Run(handler, {"ZSCORE", "z", "missing"}),
             std::string("$-1\r\n"));
}

// CH7：ZRANGE 按名次返回数组。
TEST(CommandHandler_ZRange) {
    KvStore store;
    CommandHandler handler(store);
    Run(handler, {"ZADD", "z", "3", "c", "1", "a", "2", "b"});

    CHECK_EQ(Run(handler, {"ZRANGE", "z", "0", "-1"}),
             std::string("*3\r\n$1\r\na\r\n$1\r\nb\r\n$1\r\nc\r\n"));
    CHECK_EQ(Run(handler, {"ZRANGE", "z", "1", "1"}),
             std::string("*1\r\n$1\r\nb\r\n"));
}

// CH8：ZRANGEBYSCORE 闭区间与开区间。
TEST(CommandHandler_ZRangeByScore) {
    KvStore store;
    CommandHandler handler(store);
    Run(handler, {"ZADD", "z", "1", "a", "2", "b", "3", "c"});

    CHECK_EQ(Run(handler, {"ZRANGEBYSCORE", "z", "2", "3"}),
             std::string("*2\r\n$1\r\nb\r\n$1\r\nc\r\n"));
    CHECK_EQ(Run(handler, {"ZRANGEBYSCORE", "z", "(2", "3"}),
             std::string("*1\r\n$1\r\nc\r\n"));
}

// CH9：string 和 zset 类型冲突。
TEST(CommandHandler_WrongType) {
    KvStore store;
    CommandHandler handler(store);
    Run(handler, {"SET", "k", "value"});

    const std::string response = Run(handler, {"ZADD", "k", "1", "m"});
    CHECK(response.find("WRONGTYPE") != std::string::npos);
}

// CH10：DEL / EXISTS。
TEST(CommandHandler_DelExists) {
    KvStore store;
    CommandHandler handler(store);
    Run(handler, {"SET", "a", "1"});
    Run(handler, {"SET", "b", "2"});

    CHECK_EQ(Run(handler, {"EXISTS", "a", "b", "c"}),
             std::string(":2\r\n"));
    CHECK_EQ(Run(handler, {"DEL", "a", "c"}),
             std::string(":1\r\n"));
    CHECK_EQ(Run(handler, {"EXISTS", "a"}),
             std::string(":0\r\n"));
}

// CH11：ZREM。
TEST(CommandHandler_ZRem) {
    KvStore store;
    CommandHandler handler(store);
    Run(handler, {"ZADD", "z", "1", "a", "2", "b", "3", "c"});

    CHECK_EQ(Run(handler, {"ZREM", "z", "a", "c", "x"}),
             std::string(":2\r\n"));
    CHECK_EQ(Run(handler, {"ZRANGE", "z", "0", "-1"}),
             std::string("*1\r\n$1\r\nb\r\n"));
}

// CH12：RESP 请求 -> 解析 -> 执行，模拟服务端主循环。
TEST(CommandHandler_EndToEndResp) {
    KvStore store;
    CommandHandler handler(store);

    Buffer buffer;
    buffer.Append(
        "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$5\r\nhello\r\n"
        "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n");

    CHECK_EQ(RunWire(handler, buffer),
             std::string("+OK\r\n$5\r\nhello\r\n"));
    CHECK_EQ(buffer.ReadableBytes(), std::size_t{0});
}

// CH13：inline 命令也能端到端执行。
TEST(CommandHandler_EndToEndInline) {
    KvStore store;
    CommandHandler handler(store);

    Buffer buffer;
    buffer.Append("SET k v\r\nGET k\r\n");

    CHECK_EQ(RunWire(handler, buffer), std::string("+OK\r\n$1\r\nv\r\n"));
}
