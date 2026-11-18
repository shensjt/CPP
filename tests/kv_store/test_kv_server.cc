// KV 服务集成测试（KV 系列）

#include "server/kv_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "test_framework.h"

namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;

    TempDir() {
        path = fs::temp_directory_path() /
               ("sjt_kv_server_" +
                std::to_string(static_cast<long>(::getpid())));
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

struct ServerGuard {
    KvServer server;
    std::thread thread;

    explicit ServerGuard(std::string aof_path = "")
        : server("127.0.0.1", 0, 4, 60, std::move(aof_path)),
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

    bool ReadBytes(std::size_t count, std::string& out) {
        while (buffer.size() < count) {
            if (!Fill()) return false;
        }
        out = buffer.substr(0, count);
        buffer.erase(0, count);
        return true;
    }

    bool ReadLine(std::string& line) {
        std::size_t pos = std::string::npos;
        while ((pos = buffer.find("\r\n")) == std::string::npos) {
            if (!Fill()) return false;
        }
        line = buffer.substr(0, pos + 2);
        buffer.erase(0, pos + 2);
        return true;
    }

    bool ReadArrayHeader(long long& count) {
        std::string line;
        if (!ReadLine(line) || line.size() < 3 || line[0] != '*') {
            return false;
        }
        count = std::stoll(line.substr(1));
        return true;
    }

    bool ReadBulk(std::string& value) {
        std::string header;
        if (!ReadLine(header) || header.size() < 3 || header[0] != '$') {
            return false;
        }
        const long long length = std::stoll(header.substr(1));
        if (length < 0) {
            value.clear();
            return true;
        }
        std::string payload;
        if (!ReadBytes(static_cast<std::size_t>(length) + 2, payload)) {
            return false;
        }
        value = payload.substr(0, static_cast<std::size_t>(length));
        return true;
    }

    bool WaitEof() {
        char c = 0;
        return ::recv(fd, &c, 1, 0) == 0;
    }
};

bool Expect(Client& client, const std::string& wire,
            const std::string& expected) {
    if (!client.Send(wire)) return false;
    std::string actual;
    if (!client.ReadBytes(expected.size(), actual)) return false;
    return actual == expected;
}

const char kPing[] = "*1\r\n$4\r\nPING\r\n";
const char kSetKHello[] = "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$5\r\nhello\r\n";
const char kGetK[] = "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n";

}  // namespace

// KV1：PING / SET / GET。
TEST(KvServer_PingSetGet) {
    ServerGuard guard;
    Client client;
    CHECK(client.Connect(guard.server.Port()));

    CHECK(Expect(client, kPing, "+PONG\r\n"));
    CHECK(Expect(client, kSetKHello, "+OK\r\n"));
    CHECK(Expect(client, kGetK, "$5\r\nhello\r\n"));
}

// KV2：inline 命令，模拟 nc 手测。
TEST(KvServer_Inline) {
    ServerGuard guard;
    Client client;
    CHECK(client.Connect(guard.server.Port()));

    CHECK(Expect(client, "SET k v\r\nGET k\r\n", "+OK\r\n$1\r\nv\r\n"));
}

// KV3：流水线，一次写入三条命令，按顺序读回三个响应。
TEST(KvServer_Pipelined) {
    ServerGuard guard;
    Client client;
    CHECK(client.Connect(guard.server.Port()));

    const std::string wire = std::string(kPing) + kSetKHello + kGetK;
    CHECK(Expect(client, wire, "+PONG\r\n+OK\r\n$5\r\nhello\r\n"));
}

// KV4：ZADD / ZRANGE / ZRANGEBYSCORE。
TEST(KvServer_ZSet) {
    ServerGuard guard;
    Client client;
    CHECK(client.Connect(guard.server.Port()));

    CHECK(Expect(client,
                 "*8\r\n$4\r\nZADD\r\n$1\r\nz\r\n$1\r\n1\r\n$1\r\na\r\n"
                 "$1\r\n2\r\n$1\r\nb\r\n$1\r\n3\r\n$1\r\nc\r\n",
                 ":3\r\n"));
    CHECK(Expect(client,
                 "*4\r\n$6\r\nZRANGE\r\n$1\r\nz\r\n$1\r\n0\r\n$2\r\n-1\r\n",
                 "*3\r\n$1\r\na\r\n$1\r\nb\r\n$1\r\nc\r\n"));
    CHECK(Expect(client,
                 "*4\r\n$13\r\nZRANGEBYSCORE\r\n$1\r\nz\r\n$2\r\n(1\r\n"
                 "$1\r\n3\r\n",
                 "*2\r\n$1\r\nb\r\n$1\r\nc\r\n"));
}

// KV5：类型冲突返回 WRONGTYPE。
TEST(KvServer_WrongType) {
    ServerGuard guard;
    Client client;
    CHECK(client.Connect(guard.server.Port()));

    CHECK(Expect(client, kSetKHello, "+OK\r\n"));
    CHECK(client.Send("*4\r\n$4\r\nZADD\r\n$1\r\nk\r\n$1\r\n1\r\n$1\r\nm\r\n"));

    std::string line;
    CHECK(client.ReadLine(line));
    CHECK(line.find("WRONGTYPE") != std::string::npos);
}

// KV6：协议错误返回错误并关闭连接。
TEST(KvServer_ProtocolError) {
    ServerGuard guard;
    Client client;
    CHECK(client.Connect(guard.server.Port()));

    CHECK(client.Send("*1\r\n$x\r\n"));
    std::string line;
    CHECK(client.ReadLine(line));
    CHECK(line.find("Protocol error") != std::string::npos);
    CHECK(client.WaitEof());
}

// KV7：多客户端并发写同一个 zset，最后能读回全部 member。
TEST(KvServer_ConcurrentClients) {
    ServerGuard guard;
    constexpr int kClients = 4;
    constexpr int kPerClient = 50;
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < kClients; ++t) {
        threads.emplace_back([&, t] {
            Client client;
            if (!client.Connect(guard.server.Port())) {
                failures.fetch_add(1);
                return;
            }
            for (int i = 0; i < kPerClient; ++i) {
                const std::string score = std::to_string(t * 1000 + i);
                const std::string member =
                    "m" + std::to_string(t) + "-" + std::to_string(i);
                const std::string wire =
                    "*4\r\n$4\r\nZADD\r\n$1\r\nz\r\n$" +
                    std::to_string(score.size()) + "\r\n" + score + "\r\n$" +
                    std::to_string(member.size()) + "\r\n" + member + "\r\n";
                if (!client.Send(wire)) {
                    failures.fetch_add(1);
                    return;
                }
                std::string reply;
                if (!client.ReadLine(reply) || reply != ":1\r\n") {
                    failures.fetch_add(1);
                    return;
                }
            }
        });
    }
    for (auto& thread : threads) thread.join();
    CHECK_EQ(failures.load(), 0);

    Client client;
    CHECK(client.Connect(guard.server.Port()));
    CHECK(client.Send(
        "*4\r\n$6\r\nZRANGE\r\n$1\r\nz\r\n$1\r\n0\r\n$2\r\n-1\r\n"));

    long long count = 0;
    CHECK(client.ReadArrayHeader(count));
    CHECK_EQ(count, static_cast<long long>(kClients * kPerClient));

    std::set<std::string> members;
    for (long long i = 0; i < count; ++i) {
        std::string value;
        CHECK(client.ReadBulk(value));
        members.insert(value);
    }
    CHECK_EQ(members.size(), static_cast<std::size_t>(kClients * kPerClient));
}

// KV8：写入 AOF 后重启，数据恢复。
TEST(KvServer_AofRecovery) {
    TempDir dir;
    const std::string aof = (dir.path / "kv.aof").string();

    {
        ServerGuard guard(aof);
        Client client;
        CHECK(client.Connect(guard.server.Port()));
        CHECK(Expect(client, kSetKHello, "+OK\r\n"));
        CHECK(Expect(client,
                     "*4\r\n$4\r\nZADD\r\n$1\r\nz\r\n$1\r\n1\r\n$1\r\na\r\n",
                     ":1\r\n"));
    }

    {
        ServerGuard guard(aof);
        Client client;
        CHECK(client.Connect(guard.server.Port()));
        CHECK(Expect(client, kGetK, "$5\r\nhello\r\n"));
        CHECK(Expect(client,
                     "*4\r\n$6\r\nZRANGE\r\n$1\r\nz\r\n$1\r\n0\r\n$2\r\n-1\r\n",
                     "*1\r\n$1\r\na\r\n"));
    }
}

// KV9：AOF 尾部有半条命令时，重启应截断尾部，之后的写入仍能恢复。
TEST(KvServer_AofTruncatedTail) {
    TempDir dir;
    const std::string aof = (dir.path / "kv.aof").string();

    {
        ServerGuard guard(aof);
        Client client;
        CHECK(client.Connect(guard.server.Port()));
        CHECK(Expect(client,
                     "*3\r\n$3\r\nSET\r\n$1\r\na\r\n$1\r\n1\r\n", "+OK\r\n"));
    }

    {
        std::ofstream out(aof, std::ios::binary | std::ios::app);
        out << "*3\r\n$3\r\nSET\r\n$1\r\nb";
    }

    {
        ServerGuard guard(aof);
        Client client;
        CHECK(client.Connect(guard.server.Port()));
        CHECK(Expect(client, "*2\r\n$3\r\nGET\r\n$1\r\na\r\n",
                     "$1\r\n1\r\n"));
        CHECK(Expect(client,
                     "*3\r\n$3\r\nSET\r\n$1\r\nb\r\n$1\r\n2\r\n", "+OK\r\n"));
    }

    {
        ServerGuard guard(aof);
        Client client;
        CHECK(client.Connect(guard.server.Port()));
        CHECK(Expect(client, "*2\r\n$3\r\nGET\r\n$1\r\nb\r\n",
                     "$1\r\n2\r\n"));
        CHECK(Expect(client, "*2\r\n$3\r\nGET\r\n$1\r\na\r\n",
                     "$1\r\n1\r\n"));
    }
}
