// TcpServer + ThreadPool 集成测试（TSP 系列）

#include "net/tcp_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <string>
#include <thread>
#include <vector>

#include "test_framework.h"

using namespace std::chrono_literals;

namespace {

struct ServerGuard {
    TcpServer server;
    std::thread thread;

    explicit ServerGuard(std::size_t workers, int idle_timeout = 60)
        : server("127.0.0.1", 0, workers, idle_timeout),
          thread([this] { server.Start(); }) {}

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
    for (int i = 0; i < 300; ++i) {
        if (server.ConnectionCount() == expected) return true;
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

}  // namespace

// TSP1：4 个 worker，单客户端 Echo。
TEST(TcpServerPooled_SingleClient) {
    ServerGuard guard(4);
    ClientFd client;
    client.fd = ConnectTo(guard.server.Port());
    CHECK(client.fd >= 0);

    CHECK(SendAll(client.fd, "hello"));
    std::string echoed;
    CHECK(RecvAll(client.fd, echoed, 5));
    CHECK_EQ(echoed, std::string("hello"));
}

// TSP2：8 个客户端并发，每个 10 轮，数据不串。
TEST(TcpServerPooled_MultipleClients) {
    ServerGuard guard(4);
    constexpr int kClients = 8;
    constexpr int kRounds = 10;
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;

    for (int i = 0; i < kClients; ++i) {
        threads.emplace_back([&, i] {
            const int fd = ConnectTo(guard.server.Port());
            if (fd < 0) {
                failures.fetch_add(1);
                return;
            }

            for (int round = 0; round < kRounds; ++round) {
                const std::string msg =
                    "c" + std::to_string(i) + "-r" + std::to_string(round);
                std::string echoed;
                if (!SendAll(fd, msg) || !RecvAll(fd, echoed, msg.size()) ||
                    echoed != msg) {
                    failures.fetch_add(1);
                    break;
                }
            }
            ::close(fd);
        });
    }
    for (auto& t : threads) t.join();

    CHECK_EQ(failures.load(), 0);
    CHECK(WaitForConnectionCount(guard.server, 0));
}

// TSP3：128KB 大包，覆盖短写与 EPOLLOUT 续写。
TEST(TcpServerPooled_LargeEcho) {
    ServerGuard guard(4);
    ClientFd client;
    client.fd = ConnectTo(guard.server.Port());
    CHECK(client.fd >= 0);

    const std::string payload(128 * 1024, 'x');
    CHECK(SendAll(client.fd, payload));

    std::string echoed;
    CHECK(RecvAll(client.fd, echoed, payload.size()));
    CHECK(echoed == payload);
}

// TSP4：同一连接 100 轮短消息，验证 EPOLLONESHOT 反复 Rearm。
TEST(TcpServerPooled_ManyRoundTrips) {
    ServerGuard guard(4);
    ClientFd client;
    client.fd = ConnectTo(guard.server.Port());
    CHECK(client.fd >= 0);

    for (int round = 0; round < 100; ++round) {
        const std::string msg = "ping-" + std::to_string(round);
        std::string echoed;
        CHECK(SendAll(client.fd, msg));
        CHECK(RecvAll(client.fd, echoed, msg.size()));
        CHECK_EQ(echoed, msg);
    }
}

// TSP5：空闲连接超过 timeout 后被关闭。
TEST(TcpServerPooled_IdleTimeout) {
    ServerGuard guard(4, 1);
    ClientFd client;
    client.fd = ConnectTo(guard.server.Port());
    CHECK(client.fd >= 0);
    CHECK(WaitForConnectionCount(guard.server, 1));

    CHECK(WaitForConnectionCount(guard.server, 0));
    char c = 0;
    CHECK_EQ(::recv(client.fd, &c, 1, 0), 0);
}

// TSP6：断开后连接数归零，仍可重新连接。
TEST(TcpServerPooled_CloseAndReconnect) {
    ServerGuard guard(4);

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
