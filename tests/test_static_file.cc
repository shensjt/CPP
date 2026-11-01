// StaticFileHandler 测试（SF 系列）

#include "http/static_file_handler.h"

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "http/http_request.h"
#include "test_framework.h"

namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;

    TempDir() {
        path = fs::temp_directory_path() /
               ("sjt_web_" + std::to_string(static_cast<long>(::getpid())));
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    void Write(const std::string& relative, const std::string& content) {
        const fs::path file = path / relative;
        std::error_code ec;
        fs::create_directories(file.parent_path(), ec);
        std::ofstream out(file, std::ios::binary);
        out << content;
    }
};

HttpRequest MakeRequest(const std::string& target,
                        HttpRequest::Method method = HttpRequest::Method::kGet) {
    HttpRequest request;
    request.method = method;
    request.target = target;
    request.version = "HTTP/1.1";
    return request;
}

}  // namespace

// SF1："/" 映射到 index.html。
TEST(StaticFile_RootIndex) {
    TempDir dir;
    dir.Write("index.html", "<h1>home</h1>");

    StaticFileHandler handler(dir.path.string());
    const HttpResponse response = handler.Handle(MakeRequest("/"));

    CHECK_EQ(response.StatusCode(), 200);
    CHECK_EQ(response.Body(), std::string("<h1>home</h1>"));
    CHECK(response.ToString().find("Content-Type: text/html") != std::string::npos);
}

// SF2：按扩展名映射 MIME。
TEST(StaticFile_MimeType) {
    TempDir dir;
    dir.Write("style.css", "body{}");

    StaticFileHandler handler(dir.path.string());
    const HttpResponse response = handler.Handle(MakeRequest("/style.css"));

    CHECK_EQ(response.StatusCode(), 200);
    CHECK(response.ToString().find("Content-Type: text/css") != std::string::npos);
}

// SF3：文件不存在返回 404。
TEST(StaticFile_NotFound) {
    TempDir dir;
    StaticFileHandler handler(dir.path.string());
    CHECK_EQ(handler.Handle(MakeRequest("/missing.html")).StatusCode(), 404);
}

// SF4：明文 .. 路径穿越返回 403。
TEST(StaticFile_PathTraversal) {
    TempDir dir;
    StaticFileHandler handler(dir.path.string());
    CHECK_EQ(handler.Handle(MakeRequest("/../../etc/passwd")).StatusCode(), 403);
}

// SF5：百分号编码的 .. 同样被拦截。
TEST(StaticFile_EncodedTraversal) {
    TempDir dir;
    StaticFileHandler handler(dir.path.string());
    CHECK_EQ(handler.Handle(MakeRequest("/%2e%2e/%2e%2e/etc/passwd")).StatusCode(), 403);
}

// SF6：HEAD 返回 Body，但序列化时可以只输出头。
TEST(StaticFile_Head) {
    TempDir dir;
    dir.Write("index.html", "<h1>home</h1>");

    StaticFileHandler handler(dir.path.string());
    const HttpResponse response =
        handler.Handle(MakeRequest("/index.html", HttpRequest::Method::kHead));

    CHECK_EQ(response.StatusCode(), 200);
    CHECK_EQ(response.Body(), std::string("<h1>home</h1>"));
    const std::string head = response.ToString(true);
    CHECK(head.find("\r\n\r\n<h1>") == std::string::npos);
    CHECK(head.find("Content-Length: 13\r\n") != std::string::npos);
}

// SF7：POST 返回 405，并带 Allow。
TEST(StaticFile_MethodNotAllowed) {
    TempDir dir;
    StaticFileHandler handler(dir.path.string());
    const HttpResponse response =
        handler.Handle(MakeRequest("/index.html", HttpRequest::Method::kPost));

    CHECK_EQ(response.StatusCode(), 405);
    CHECK(response.ToString().find("Allow: GET, HEAD\r\n") != std::string::npos);
}

// SF8：目录默认找 index.html。
TEST(StaticFile_DirectoryIndex) {
    TempDir dir;
    dir.Write("sub/index.html", "sub");

    StaticFileHandler handler(dir.path.string());
    const HttpResponse response = handler.Handle(MakeRequest("/sub/"));

    CHECK_EQ(response.StatusCode(), 200);
    CHECK_EQ(response.Body(), std::string("sub"));
}

// SF9：未知扩展名回退 octet-stream。
TEST(StaticFile_UnknownExtension) {
    TempDir dir;
    dir.Write("data.bin", "abc");

    StaticFileHandler handler(dir.path.string());
    const HttpResponse response = handler.Handle(MakeRequest("/data.bin"));

    CHECK_EQ(response.StatusCode(), 200);
    CHECK(response.ToString().find("Content-Type: application/octet-stream") !=
          std::string::npos);
}

// SF10：非法百分号编码返回 400。
TEST(StaticFile_InvalidPercentEncoding) {
    TempDir dir;
    StaticFileHandler handler(dir.path.string());
    CHECK_EQ(handler.Handle(MakeRequest("/%zz")).StatusCode(), 400);
}

// SF11：query 不影响文件查找。
TEST(StaticFile_QueryIgnored) {
    TempDir dir;
    dir.Write("index.html", "home");

    StaticFileHandler handler(dir.path.string());
    const HttpResponse response = handler.Handle(MakeRequest("/index.html?x=1"));

    CHECK_EQ(response.StatusCode(), 200);
    CHECK_EQ(response.Body(), std::string("home"));
}

// SF12：指向 document root 外的符号链接被拦截。
TEST(StaticFile_SymlinkEscape) {
    TempDir dir;
    std::error_code ec;
    fs::create_symlink("/etc/passwd", dir.path / "link", ec);
    if (ec) {
        return;  // 环境不支持符号链接时跳过
    }

    StaticFileHandler handler(dir.path.string());
    CHECK_EQ(handler.Handle(MakeRequest("/link")).StatusCode(), 403);
}
