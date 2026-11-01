// HttpServer 集成测试（HS 系列）

#include "http/http_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "test_framework.h"

namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;

    TempDir() {
        path = fs::temp_directory_path() /
               ("sjt_http_" + std::to_string(static_cast<long>(::getpid())));
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

struct ServerGuard {
    HttpServer server;
    std::thread thread;

    explicit ServerGuard(const std::string& root, std::size_t workers = 4)
        : server("127.0.0.1", 0, root, workers),
          thread([this] { server.Start(); }) {}

    ~ServerGuard() {
        server.Stop();
        if (thread.joinable()) thread.join();
    }
};

struct Client {
    int fd = -1;
    std::string buffer;

    ~Client() {
        if (fd >= 0) ::close(fd);
    }

    bool Connect(uint16_t port) {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return false;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            ::close(fd);
            fd = -1;
            return false;
        }

        timeval tv{2, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        return true;
    }

    bool Send(const std::string& data) {
        std::size_t sent = 0;
        while (sent < data.size()) {
            const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent,
                                     MSG_NOSIGNAL);
            if (n > 0) {
                sent += static_cast<std::size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        return true;
    }

    bool Fill() {
        char buf[4096];
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        buffer.append(buf, static_cast<std::size_t>(n));
        return true;
    }

    bool ReadResponse(bool head_only, std::string& response) {
        std::size_t header_end = std::string::npos;
        while ((header_end = buffer.find("\r\n\r\n")) == std::string::npos) {
            if (!Fill()) return false;
        }

        std::size_t length = 0;
        const auto pos = buffer.find("Content-Length: ");
        if (pos != std::string::npos) {
            length = static_cast<std::size_t>(
                std::stoul(buffer.substr(pos + 16)));
        }

        const std::size_t total = header_end + 4 + (head_only ? 0 : length);
        while (buffer.size() < total) {
            if (!Fill()) return false;
        }

        response = buffer.substr(0, total);
        buffer.erase(0, total);
        return true;
    }

    bool WaitEof() {
        char c = 0;
        return ::recv(fd, &c, 1, 0) == 0;
    }
};

}  // namespace

// HS1：GET / 返回 index.html。
TEST(HttpServer_GetIndex) {
    TempDir dir;
    dir.Write("index.html", "<h1>home</h1>");
    ServerGuard guard(dir.path.string());

    Client client;
    CHECK(client.Connect(guard.server.Port()));
    CHECK(client.Send("GET / HTTP/1.1\r\nHost: x\r\n\r\n"));

    std::string response;
    CHECK(client.ReadResponse(false, response));
    CHECK(response.rfind("HTTP/1.1 200 OK", 0) == 0);
    CHECK(response.find("Content-Type: text/html") != std::string::npos);
    CHECK(response.find("<h1>home</h1>") != std::string::npos);
}

// HS2：文件不存在返回 404。
TEST(HttpServer_NotFound) {
    TempDir dir;
    ServerGuard guard(dir.path.string());

    Client client;
    CHECK(client.Connect(guard.server.Port()));
    CHECK(client.Send("GET /missing HTTP/1.1\r\nHost: x\r\n\r\n"));

    std::string response;
    CHECK(client.ReadResponse(false, response));
    CHECK(response.rfind("HTTP/1.1 404 Not Found", 0) == 0);
}

// HS3：路径穿越返回 403。
TEST(HttpServer_PathTraversal) {
    TempDir dir;
    ServerGuard guard(dir.path.string());

    Client client;
    CHECK(client.Connect(guard.server.Port()));
    CHECK(client.Send("GET /../../etc/passwd HTTP/1.1\r\nHost: x\r\n\r\n"));

    std::string response;
    CHECK(client.ReadResponse(false, response));
    CHECK(response.rfind("HTTP/1.1 403 Forbidden", 0) == 0);
}

// HS4：HEAD 只返回头，Content-Length 仍是 Body 长度。
TEST(HttpServer_Head) {
    TempDir dir;
    dir.Write("index.html", "<h1>home</h1>");
    ServerGuard guard(dir.path.string());

    Client client;
    CHECK(client.Connect(guard.server.Port()));
    CHECK(client.Send("HEAD /index.html HTTP/1.1\r\nHost: x\r\n\r\n"));

    std::string response;
    CHECK(client.ReadResponse(true, response));
    CHECK(response.rfind("HTTP/1.1 200 OK", 0) == 0);
    CHECK(response.find("Content-Length: 13\r\n") != std::string::npos);
    CHECK(response.find("<h1>") == std::string::npos);
}

// HS5：HTTP/1.1 默认 keep-alive，同一连接可以发多个请求。
TEST(HttpServer_KeepAlive) {
    TempDir dir;
    dir.Write("index.html", "home");
    dir.Write("style.css", "body{}");
    ServerGuard guard(dir.path.string());

    Client client;
    CHECK(client.Connect(guard.server.Port()));

    CHECK(client.Send("GET / HTTP/1.1\r\nHost: x\r\n\r\n"));
    std::string first;
    CHECK(client.ReadResponse(false, first));
    CHECK(first.rfind("HTTP/1.1 200 OK", 0) == 0);

    CHECK(client.Send("GET /style.css HTTP/1.1\r\nHost: x\r\n\r\n"));
    std::string second;
    CHECK(client.ReadResponse(false, second));
    CHECK(second.rfind("HTTP/1.1 200 OK", 0) == 0);
    CHECK(second.find("Content-Type: text/css") != std::string::npos);
}

// HS6：流水线，一次写入两个请求，读回两个响应。
TEST(HttpServer_Pipelined) {
    TempDir dir;
    dir.Write("index.html", "home");
    dir.Write("style.css", "body{}");
    ServerGuard guard(dir.path.string());

    Client client;
    CHECK(client.Connect(guard.server.Port()));
    CHECK(client.Send(
        "GET / HTTP/1.1\r\nHost: x\r\n\r\n"
        "GET /style.css HTTP/1.1\r\nHost: x\r\n\r\n"));

    std::string first;
    std::string second;
    CHECK(client.ReadResponse(false, first));
    CHECK(client.ReadResponse(false, second));
    CHECK(first.rfind("HTTP/1.1 200 OK", 0) == 0);
    CHECK(second.rfind("HTTP/1.1 200 OK", 0) == 0);
}

// HS7：Connection: close 后服务器发完响应就关闭连接。
TEST(HttpServer_ConnectionClose) {
    TempDir dir;
    dir.Write("index.html", "home");
    ServerGuard guard(dir.path.string());

    Client client;
    CHECK(client.Connect(guard.server.Port()));
    CHECK(client.Send(
        "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"));

    std::string response;
    CHECK(client.ReadResponse(false, response));
    CHECK(response.find("Connection: close\r\n") != std::string::npos);
    CHECK(client.WaitEof());
}

// HS8：POST 返回 405。
TEST(HttpServer_MethodNotAllowed) {
    TempDir dir;
    ServerGuard guard(dir.path.string());

    Client client;
    CHECK(client.Connect(guard.server.Port()));
    CHECK(client.Send("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n"));

    std::string response;
    CHECK(client.ReadResponse(false, response));
    CHECK(response.rfind("HTTP/1.1 405 Method Not Allowed", 0) == 0);
}

// HS9：畸形请求返回 400 并关闭连接。
TEST(HttpServer_BadRequest) {
    TempDir dir;
    ServerGuard guard(dir.path.string());

    Client client;
    CHECK(client.Connect(guard.server.Port()));
    CHECK(client.Send("BAD\r\n\r\n"));

    std::string response;
    CHECK(client.ReadResponse(false, response));
    CHECK(response.rfind("HTTP/1.1 400 Bad Request", 0) == 0);
    CHECK(client.WaitEof());
}

// HS10：多个客户端并发 GET，全部 200。
TEST(HttpServer_ConcurrentClients) {
    TempDir dir;
    dir.Write("index.html", "home");
    ServerGuard guard(dir.path.string());

    constexpr int kClients = 8;
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;

    for (int i = 0; i < kClients; ++i) {
        threads.emplace_back([&] {
            Client client;
            if (!client.Connect(guard.server.Port()) ||
                !client.Send("GET / HTTP/1.1\r\nHost: x\r\n\r\n")) {
                failures.fetch_add(1);
                return;
            }
            std::string response;
            if (!client.ReadResponse(false, response) ||
                response.rfind("HTTP/1.1 200 OK", 0) != 0) {
                failures.fetch_add(1);
            }
        });
    }
    for (auto& t : threads) t.join();

    CHECK_EQ(failures.load(), 0);
}
