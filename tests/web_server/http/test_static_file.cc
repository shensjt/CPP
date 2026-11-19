// StaticFileHandler 测试（SF 系列）

#include "http/static_file_handler.h"

#include <unistd.h>

#include <chrono>
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

std::string ResponseHeader(const HttpResponse& response,
                           const std::string& name) {
    const std::string wire = response.ToString();
    const std::string needle = "\r\n" + name + ": ";
    const std::size_t pos = wire.find(needle);
    if (pos == std::string::npos) return {};

    const std::size_t begin = pos + needle.size();
    const std::size_t end = wire.find("\r\n", begin);
    return wire.substr(begin, end - begin);
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

// SF12：文件内容更新后，缓存应失效。
TEST(StaticFile_CacheInvalidatedOnContentChange) {
    TempDir dir;
    dir.Write("index.html", "old-content");

    StaticFileHandler handler(dir.path.string());
    CHECK_EQ(handler.Handle(MakeRequest("/index.html")).Body(),
             std::string("old-content"));

    dir.Write("index.html", "new-content");
    CHECK_EQ(handler.Handle(MakeRequest("/index.html")).Body(),
             std::string("new-content"));
}

// SF14：大小不变、只推进 mtime，缓存也应失效。
TEST(StaticFile_CacheInvalidatedOnMtimeChange) {
    TempDir dir;
    const fs::path file = dir.path / "index.html";
    dir.Write("index.html", "aaaa");

    StaticFileHandler handler(dir.path.string());
    CHECK_EQ(handler.Handle(MakeRequest("/index.html")).Body(),
             std::string("aaaa"));

    std::error_code ec;
    const auto mtime = fs::last_write_time(file, ec);
    dir.Write("index.html", "bbbb");
    fs::last_write_time(file, mtime + std::chrono::seconds(2), ec);
    CHECK_EQ(handler.Handle(MakeRequest("/index.html")).Body(),
             std::string("bbbb"));
}

// SF15：mtime 不变、只改变大小，缓存也应失效。
TEST(StaticFile_CacheInvalidatedOnSizeChange) {
    TempDir dir;
    const fs::path file = dir.path / "index.html";
    dir.Write("index.html", "old");

    StaticFileHandler handler(dir.path.string());
    CHECK_EQ(handler.Handle(MakeRequest("/index.html")).Body(),
             std::string("old"));

    std::error_code ec;
    const auto mtime = fs::last_write_time(file, ec);
    dir.Write("index.html", "new-longer");
    fs::last_write_time(file, mtime, ec);
    CHECK_EQ(handler.Handle(MakeRequest("/index.html")).Body(),
             std::string("new-longer"));
}

// SF21：路径缓存命中后文件被删，再重建，仍能正确 404 / 200。
TEST(StaticFile_PathCacheAfterDeleteRecreate) {
    TempDir dir;
    dir.Write("file.txt", "one");

    StaticFileHandler handler(dir.path.string());
    CHECK_EQ(handler.Handle(MakeRequest("/file.txt")).Body(), std::string("one"));

    std::error_code ec;
    fs::remove(dir.path / "file.txt", ec);
    CHECK_EQ(handler.Handle(MakeRequest("/file.txt")).StatusCode(), 404);

    dir.Write("file.txt", "two");
    const HttpResponse response = handler.Handle(MakeRequest("/file.txt"));
    CHECK_EQ(response.StatusCode(), 200);
    CHECK_EQ(response.Body(), std::string("two"));
}

// SF22：路径缓存命中后，文件被替换成指向 root 外的符号链接，仍应 403。
TEST(StaticFile_PathCacheSymlinkSwap) {
    TempDir dir;
    dir.Write("file.txt", "safe");
    const fs::path file = dir.path / "file.txt";

    StaticFileHandler handler(dir.path.string());
    CHECK_EQ(handler.Handle(MakeRequest("/file.txt")).Body(), std::string("safe"));

    std::error_code ec;
    fs::remove(file, ec);
    fs::create_symlink("/etc/passwd", file, ec);
    if (ec) {
        return;  // 环境不支持符号链接时跳过
    }
    CHECK_EQ(handler.Handle(MakeRequest("/file.txt")).StatusCode(), 403);
}

// SF13：指向 document root 外的符号链接被拦截。
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

// SF16：200 响应带缓存头。
TEST(StaticFile_CacheHeaders) {
    TempDir dir;
    dir.Write("index.html", "home");

    StaticFileHandler handler(dir.path.string());
    const HttpResponse response = handler.Handle(MakeRequest("/index.html"));

    CHECK_EQ(response.StatusCode(), 200);
    CHECK(!ResponseHeader(response, "ETag").empty());
    CHECK(!ResponseHeader(response, "Last-Modified").empty());
    CHECK_EQ(ResponseHeader(response, "Cache-Control"), std::string("no-cache"));
}

// SF17：If-None-Match 命中返回 304。
TEST(StaticFile_NotModifiedEtag) {
    TempDir dir;
    dir.Write("index.html", "home");

    StaticFileHandler handler(dir.path.string());
    const HttpResponse first = handler.Handle(MakeRequest("/index.html"));
    const std::string etag = ResponseHeader(first, "ETag");
    CHECK(!etag.empty());

    HttpRequest request = MakeRequest("/index.html");
    // 多值 + 弱 ETag 语法都要能匹配。
    request.headers["if-none-match"] = "\"other\", W/" + etag;
    const HttpResponse second = handler.Handle(request);

    CHECK_EQ(second.StatusCode(), 304);
    CHECK(second.Body().empty());
    CHECK_EQ(ResponseHeader(second, "ETag"), etag);
    const std::string wire = second.ToString();
    CHECK(wire.find("304 Not Modified") != std::string::npos);
    CHECK(wire.find("Content-Length") == std::string::npos);
}

// SF18：If-Modified-Since 命中返回 304。
TEST(StaticFile_NotModifiedSince) {
    TempDir dir;
    dir.Write("index.html", "home");

    StaticFileHandler handler(dir.path.string());
    const HttpResponse first = handler.Handle(MakeRequest("/index.html"));
    const std::string last_modified = ResponseHeader(first, "Last-Modified");
    CHECK(!last_modified.empty());

    HttpRequest request = MakeRequest("/index.html");
    request.headers["if-modified-since"] = last_modified;
    CHECK_EQ(handler.Handle(request).StatusCode(), 304);
}

// SF19：文件更新后旧 ETag 不再命中。
TEST(StaticFile_EtagChangesAfterWrite) {
    TempDir dir;
    dir.Write("index.html", "old");

    StaticFileHandler handler(dir.path.string());
    const std::string old_etag =
        ResponseHeader(handler.Handle(MakeRequest("/index.html")), "ETag");
    CHECK(!old_etag.empty());

    dir.Write("index.html", "new-content");

    HttpRequest request = MakeRequest("/index.html");
    request.headers["if-none-match"] = old_etag;
    const HttpResponse response = handler.Handle(request);

    CHECK_EQ(response.StatusCode(), 200);
    CHECK_EQ(response.Body(), std::string("new-content"));
    CHECK(ResponseHeader(response, "ETag") != old_etag);
}

// SF20：HEAD 条件请求同样返回 304，且没有 body/Content-Length。
TEST(StaticFile_HeadNotModified) {
    TempDir dir;
    dir.Write("index.html", "home");

    StaticFileHandler handler(dir.path.string());
    const HttpResponse first = handler.Handle(MakeRequest("/index.html"));
    const std::string etag = ResponseHeader(first, "ETag");
    CHECK(!etag.empty());

    HttpRequest request =
        MakeRequest("/index.html", HttpRequest::Method::kHead);
    request.headers["if-none-match"] = etag;
    const HttpResponse second = handler.Handle(request);

    CHECK_EQ(second.StatusCode(), 304);
    const std::string wire = second.ToString(true);
    CHECK(wire.find("Content-Length") == std::string::npos);
    CHECK(wire.find("\r\n\r\n") == wire.size() - 4);
}
