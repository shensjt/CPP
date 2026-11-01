// TcpServer 单元/集成测试（TS 系列）
//
// 服务器监听 127.0.0.1 的临时端口，客户端用普通阻塞 socket 做 Echo 校验。

#include "net/tcp_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <exception>
#include <string>
#include <thread>
#include <vector>

#include "test_framework.h"

using namespace std::chrono_literals;

namespace {

struct ServerGuard {
    TcpServer server;
    std::thread thread;

    ServerGuard() : server("127.0.0.1", 0), thread([this] { server.Start(); }) {}

    ~ServerGuard() {
        server.Stop();
        if (thread.joinable()) thread.join();
    }
};

struct ClientFd {
    int fd = -1;
    ~ClientFd() {
        if (fd >= 0) ::close(fd);
    }
};

int ConnectTo(uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }

    timeval tv{2, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return fd;
}

bool SendAll(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

bool RecvAll(int fd, std::string& out, std::size_t n) {
    out.resize(n);
    std::size_t got = 0;
    while (got < n) {
        const ssize_t r = ::recv(fd, out.data() + got, n - got, 0);
        if (r > 0) {
            got += static_cast<std::size_t>(r);
            continue;
        }
        if (r < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

bool WaitForConnectionCount(TcpServer& server, std::size_t expected) {
    for (int i = 0; i < 200; ++i) {
        if (server.ConnectionCount() == expected) return true;
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

}  // namespace

// TS1：单客户端发送，按原样回显。
TEST(TcpServer_EchoSingleClient) {
    ServerGuard guard;
    ClientFd client;
    client.fd = ConnectTo(guard.server.Port());
    CHECK(client.fd >= 0);

    CHECK(SendAll(client.fd, "hello"));
    std::string echoed;
    CHECK(RecvAll(client.fd, echoed, 5));
    CHECK_EQ(echoed, std::string("hello"));

    ::close(client.fd);
    client.fd = -1;
    CHECK(WaitForConnectionCount(guard.server, 0));
}

// TS2：多个客户端并发 Echo，各自数据不串。
TEST(TcpServer_MultipleClients) {
    ServerGuard guard;
    constexpr int kClients = 4;
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;

    for (int i = 0; i < kClients; ++i) {
        threads.emplace_back([&, i] {
            const int fd = ConnectTo(guard.server.Port());
            if (fd < 0) {
                failures.fetch_add(1);
                return;
            }

            const std::string msg = "client-" + std::to_string(i);
            std::string echoed;
            const bool ok = SendAll(fd, msg) && RecvAll(fd, echoed, msg.size()) &&
                            echoed == msg;
            if (!ok) failures.fetch_add(1);
            ::close(fd);
        });
    }
    for (auto& t : threads) t.join();

    CHECK_EQ(failures.load(), 0);
}

// TS3：128KB 数据往返，覆盖多次 read/flush。
TEST(TcpServer_LargeEcho) {
    ServerGuard guard;
    ClientFd client;
    client.fd = ConnectTo(guard.server.Port());
    CHECK(client.fd >= 0);

    const std::string payload(128 * 1024, 'x');
    CHECK(SendAll(client.fd, payload));

    std::string echoed;
    CHECK(RecvAll(client.fd, echoed, payload.size()));
    CHECK(echoed == payload);
}

// TS4：客户端断开后连接数归零，服务器仍可接受新连接。
TEST(TcpServer_Reconnect) {
    ServerGuard guard;

    for (int round = 0; round < 3; ++round) {
        ClientFd client;
        client.fd = ConnectTo(guard.server.Port());
        CHECK(client.fd >= 0);

        CHECK(SendAll(client.fd, "ping"));
        std::string echoed;
        CHECK(RecvAll(client.fd, echoed, 4));
        CHECK_EQ(echoed, std::string("ping"));

        ::close(client.fd);
        client.fd = -1;
        CHECK(WaitForConnectionCount(guard.server, 0));
    }
}

// TS5：端口传 0 时由内核分配，Port() 返回实际端口。
TEST(TcpServer_PortAssigned) {
    TcpServer server("127.0.0.1", 0);
    CHECK(server.Port() != 0);
}

// TS6：非法 IP 在构造期抛异常。
TEST(TcpServer_InvalidIpThrows) {
    bool threw = false;
    try {
        TcpServer server("999.1.1.1", 0);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}
