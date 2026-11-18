// HttpResponse 序列化测试（HR 系列）

#include "http/http_response.h"

#include <string>

#include "test_framework.h"

// HR1：默认 200，自动生成 Content-Length 和 Content-Type。
TEST(HttpResponse_DefaultOk) {
    HttpResponse response(200);
    response.SetBody("hello");

    const std::string text = response.ToString();
    CHECK(text.find("HTTP/1.1 200 OK\r\n") == 0);
    CHECK(text.find("Content-Length: 5\r\n") != std::string::npos);
    CHECK(text.find("Content-Type: text/html") != std::string::npos);
    CHECK(text.find("\r\n\r\nhello") != std::string::npos);
}

// HR2：404 的 reason phrase 正确。
TEST(HttpResponse_NotFound) {
    HttpResponse response(404);
    response.SetBody("missing");

    const std::string text = response.ToString();
    CHECK(text.find("HTTP/1.1 404 Not Found\r\n") == 0);
    CHECK(text.find("Content-Length: 7\r\n") != std::string::npos);
}

// HR3：HEAD 序列化不带 Body，但 Content-Length 仍是 Body 长度。
TEST(HttpResponse_HeadOnly) {
    HttpResponse response(200);
    response.SetBody("body");

    const std::string text = response.ToString(true);
    CHECK(text.find("Content-Length: 4\r\n") != std::string::npos);
    CHECK(text.find("\r\n\r\n") != std::string::npos);
    CHECK(text.find("\r\n\r\nbody") == std::string::npos);
}

// HR4：同名 Header 大小写不敏感地覆盖。
TEST(HttpResponse_HeaderReplace) {
    HttpResponse response;
    response.SetHeader("X-Test", "first");
    response.SetHeader("x-test", "second");

    const std::string text = response.ToString();
    CHECK(text.find("X-Test: second\r\n") != std::string::npos);
    CHECK(text.find("first") == std::string::npos);
}

// HR5：Connection 由 close_ 决定。
TEST(HttpResponse_Connection) {
    HttpResponse keep;
    CHECK(keep.ToString().find("Connection: keep-alive\r\n") != std::string::npos);

    HttpResponse close;
    close.SetCloseConnection(true);
    CHECK(close.ToString().find("Connection: close\r\n") != std::string::npos);
}

// HR6：空 Body 的 Content-Length 为 0。
TEST(HttpResponse_EmptyBody) {
    HttpResponse response;
    CHECK(response.ToString().find("Content-Length: 0\r\n") != std::string::npos);
}

// HR7：自定义 Content-Type 生效。
TEST(HttpResponse_ContentType) {
    HttpResponse response;
    response.SetContentType("text/css; charset=utf-8");
    CHECK(response.ToString().find("Content-Type: text/css; charset=utf-8\r\n") !=
          std::string::npos);
}

// HR8：用户设置的 Content-Length 会被真实 Body 长度覆盖。
TEST(HttpResponse_ContentLengthOverridden) {
    HttpResponse response;
    response.SetHeader("Content-Length", "999");
    response.SetBody("abc");

    const std::string text = response.ToString();
    CHECK(text.find("Content-Length: 3\r\n") != std::string::npos);
    CHECK(text.find("Content-Length: 999\r\n") == std::string::npos);
}

// HR9：Body 中的 NUL 字节不影响长度。
TEST(HttpResponse_BinaryBody) {
    HttpResponse response;
    const std::string body("a\0b", 3);
    response.SetBody(body);

    CHECK(response.ToString().find("Content-Length: 3\r\n") != std::string::npos);
}
