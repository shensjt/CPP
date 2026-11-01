// HTTP 请求解析测试（HP 系列）

#include "http/http_parser.h"

#include <cstddef>
#include <string>

#include "net/buffer.h"
#include "test_framework.h"

namespace {

std::string GetRequest(HttpRequest::Method method, const std::string& target,
                       const std::string& version = "HTTP/1.1") {
    std::string method_text;
    switch (method) {
        case HttpRequest::Method::kGet: method_text = "GET"; break;
        case HttpRequest::Method::kPost: method_text = "POST"; break;
        case HttpRequest::Method::kHead: method_text = "HEAD"; break;
        default: method_text = "UNKNOWN"; break;
    }
    return method_text + " " + target + " " + version + "\r\n";
}

}  // namespace

// HP1：完整 GET，请求行、Headers、路径和 query 都正确。
TEST(HttpParser_SimpleGet) {
    const std::string raw =
        "GET /index.html?x=1 HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "User-Agent: test\r\n"
        "\r\n";

    Buffer buffer;
    buffer.Append(raw);
    HttpParser parser;
    HttpRequest request;

    CHECK(parser.Parse(buffer, request) == HttpParser::Result::kComplete);
    CHECK(request.method == HttpRequest::Method::kGet);
    CHECK_EQ(request.target, std::string("/index.html?x=1"));
    CHECK_EQ(request.version, std::string("HTTP/1.1"));
    CHECK_EQ(request.Path(), std::string("/index.html"));
    CHECK_EQ(request.Query(), std::string("x=1"));
    CHECK_EQ(request.GetHeader("HOST"), std::string("example.com"));
    CHECK(request.keep_alive);
    CHECK_EQ(buffer.ReadableBytes(), std::size_t{0});
}

// HP2：没有 query 时 Path 是完整 target，Query 为空。
TEST(HttpParser_NoQuery) {
    Buffer buffer;
    buffer.Append(GetRequest(HttpRequest::Method::kGet, "/") + "\r\n");

    HttpParser parser;
    HttpRequest request;
    CHECK(parser.Parse(buffer, request) == HttpParser::Result::kComplete);
    CHECK_EQ(request.Path(), std::string("/"));
    CHECK_EQ(request.Query(), std::string());
}

// HP3：Header 名大小写不敏感，值两边空白被裁掉。
TEST(HttpParser_HeaderCaseInsensitiveAndTrim) {
    Buffer buffer;
    buffer.Append(
        "GET / HTTP/1.1\r\n"
        "X-Test:   value  \r\n"
        "Host: example.com\r\n"
        "\r\n");

    HttpParser parser;
    HttpRequest request;
    CHECK(parser.Parse(buffer, request) == HttpParser::Result::kComplete);
    CHECK_EQ(request.GetHeader("x-test"), std::string("value"));
    CHECK_EQ(request.GetHeader("X-TEST"), std::string("value"));
    CHECK_EQ(request.GetHeader("host"), std::string("example.com"));
}

// HP4：请求分两次到达，第一次 NeedMore，第二次 Complete。
TEST(HttpParser_HalfRequest) {
    const std::string raw =
        "GET /hello HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "\r\n";

    Buffer buffer;
    buffer.Append(raw.substr(0, 12));

    HttpParser parser;
    HttpRequest request;
    CHECK(parser.Parse(buffer, request) == HttpParser::Result::kNeedMore);

    buffer.Append(raw.substr(12));
    CHECK(parser.Parse(buffer, request) == HttpParser::Result::kComplete);
    CHECK_EQ(request.target, std::string("/hello"));
}

// HP5：POST 带 Content-Length，Body 分两次到达。
TEST(HttpParser_PostBodySplit) {
    const std::string headers =
        "POST /submit HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Content-Length: 5\r\n"
        "\r\n";

    Buffer buffer;
    buffer.Append(headers + "he");

    HttpParser parser;
    HttpRequest request;
    CHECK(parser.Parse(buffer, request) == HttpParser::Result::kNeedMore);

    buffer.Append("llo");
    CHECK(parser.Parse(buffer, request) == HttpParser::Result::kComplete);
    CHECK_EQ(request.body, std::string("hello"));
}

// HP6：一个 Buffer 里有两个请求，Reset 后能继续解析第二个。
TEST(HttpParser_PipelinedRequests) {
    const std::string first = GetRequest(HttpRequest::Method::kGet, "/one") + "\r\n";
    const std::string second = GetRequest(HttpRequest::Method::kGet, "/two") + "\r\n";

    Buffer buffer;
    buffer.Append(first + second);

    HttpParser parser;
    HttpRequest request;
    CHECK(parser.Parse(buffer, request) == HttpParser::Result::kComplete);
    CHECK_EQ(request.target, std::string("/one"));

    parser.Reset();
    request.Clear();
    CHECK(parser.Parse(buffer, request) == HttpParser::Result::kComplete);
    CHECK_EQ(request.target, std::string("/two"));
    CHECK_EQ(buffer.ReadableBytes(), std::size_t{0});
}

// HP7：请求行格式错误返回 Error。
TEST(HttpParser_BadRequestLine) {
    Buffer buffer;
    buffer.Append("BAD\r\n\r\n");

    HttpParser parser;
    HttpRequest request;
    CHECK(parser.Parse(buffer, request) == HttpParser::Result::kError);
}

// HP8：Content-Length 不是数字返回 Error。
TEST(HttpParser_BadContentLength) {
    Buffer buffer;
    buffer.Append(
        "POST / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Content-Length: abc\r\n"
        "\r\n");

    HttpParser parser;
    HttpRequest request;
    CHECK(parser.Parse(buffer, request) == HttpParser::Result::kError);
}

// HP9：只有请求行和 Header，缺空行，返回 NeedMore。
TEST(HttpParser_HeadersNotFinished) {
    Buffer buffer;
    buffer.Append(
        "GET / HTTP/1.1\r\n"
        "Host: example.com\r\n");

    HttpParser parser;
    HttpRequest request;
    CHECK(parser.Parse(buffer, request) == HttpParser::Result::kNeedMore);
}

// HP10：keep-alive 由版本和 Connection 决定。
TEST(HttpParser_KeepAliveRules) {
    {
        Buffer buffer;
        buffer.Append("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
        HttpParser parser;
        HttpRequest request;
        CHECK(parser.Parse(buffer, request) == HttpParser::Result::kComplete);
        CHECK(request.keep_alive);
    }
    {
        Buffer buffer;
        buffer.Append("GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        HttpParser parser;
        HttpRequest request;
        CHECK(parser.Parse(buffer, request) == HttpParser::Result::kComplete);
        CHECK(!request.keep_alive);
    }
    {
        Buffer buffer;
        buffer.Append("GET / HTTP/1.0\r\nHost: x\r\n\r\n");
        HttpParser parser;
        HttpRequest request;
        CHECK(parser.Parse(buffer, request) == HttpParser::Result::kComplete);
        CHECK(!request.keep_alive);
    }
    {
        Buffer buffer;
        buffer.Append("GET / HTTP/1.0\r\nHost: x\r\nConnection: keep-alive\r\n\r\n");
        HttpParser parser;
        HttpRequest request;
        CHECK(parser.Parse(buffer, request) == HttpParser::Result::kComplete);
        CHECK(request.keep_alive);
    }
}

// HP11：请求行超过上限且没有 CRLF，返回 Error。
TEST(HttpParser_RequestLineTooLong) {
    Buffer buffer;
    buffer.Append(std::string(9000, 'A'));

    HttpParser parser;
    HttpRequest request;
    CHECK(parser.Parse(buffer, request) == HttpParser::Result::kError);
}

// HP12：Body 超过上限返回 Error。
TEST(HttpParser_BodyTooLarge) {
    Buffer buffer;
    buffer.Append(
        "POST / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Content-Length: 2000000\r\n"
        "\r\n");

    HttpParser parser;
    HttpRequest request;
    CHECK(parser.Parse(buffer, request) == HttpParser::Result::kError);
}

// HP13：chunked 暂不支持，直接拒绝。
TEST(HttpParser_ChunkedRejected) {
    Buffer buffer;
    buffer.Append(
        "POST / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n");

    HttpParser parser;
    HttpRequest request;
    CHECK(parser.Parse(buffer, request) == HttpParser::Result::kError);
}

// HP14：未知方法解析为 kUnknown，由上层返回 405。
TEST(HttpParser_UnknownMethod) {
    Buffer buffer;
    buffer.Append("PATCH / HTTP/1.1\r\nHost: x\r\n\r\n");

    HttpParser parser;
    HttpRequest request;
    CHECK(parser.Parse(buffer, request) == HttpParser::Result::kComplete);
    CHECK(request.method == HttpRequest::Method::kUnknown);
}
