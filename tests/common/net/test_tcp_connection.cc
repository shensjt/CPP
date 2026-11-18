// TcpConnection 单元测试（TC 系列）

#include "net/tcp_connection.h"

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <string>
#include <vector>

#include "net/epoll.h"
#include "test_framework.h"

namespace {

struct FdPair {
    int fds[2]{-1, -1};

    FdPair() {
        CHECK_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    }

    ~FdPair() {
        if (fds[0] >= 0) ::close(fds[0]);
        if (fds[1] >= 0) ::close(fds[1]);
    }

    int Release(int index) {
        const int fd = fds[index];
        fds[index] = -1;
        return fd;
    }
};

bool WriteAll(int fd, const char* data, std::size_t len) {
    std::size_t sent = 0;
    while (sent < len) {
        const ssize_t n = ::write(fd, data + sent, len - sent);
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
    timeval tv{2, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

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

}  // namespace

// TC1：读事件触发 message 回调，回调里 Echo 回对端。
TEST(TcpConnection_ReadAndEcho) {
    FdPair pair;
    Epoll epoll;
    TcpConnection conn(&epoll, pair.Release(0));
    conn.SetMessageCallback([](TcpConnection& c) {
        c.Send(c.ReadBuffer().RetrieveAllAsString());
    });
    epoll.Add(conn.Fd(), EPOLLIN);

    CHECK(WriteAll(pair.fds[1], "hello", 5));

    std::vector<epoll_event> events(8);
    CHECK_EQ(epoll.Wait(events, 500), 1);
    CHECK(conn.HandleRead());

    std::string echoed;
    CHECK(RecvAll(pair.fds[1], echoed, 5));
    CHECK_EQ(echoed, std::string("hello"));
}

// TC2：对端关闭后 HandleRead 返回 false，连接进入关闭状态。
TEST(TcpConnection_PeerClose) {
    FdPair pair;
    Epoll epoll;
    TcpConnection conn(&epoll, pair.Release(0));
    epoll.Add(conn.Fd(), EPOLLIN);

    ::close(pair.fds[1]);
    pair.fds[1] = -1;

    std::vector<epoll_event> events(8);
    CHECK_EQ(epoll.Wait(events, 500), 1);
    CHECK(!conn.HandleRead());
    CHECK(!conn.Connected());
    CHECK_EQ(conn.Fd(), -1);
}

// TC3：关闭后 Send 不崩溃，也不会重新打开 fd。
TEST(TcpConnection_SendAfterClose) {
    FdPair pair;
    Epoll epoll;
    TcpConnection conn(&epoll, pair.Release(0));

    conn.Close();
    CHECK(!conn.Connected());

    conn.Send("ignored");
    CHECK(!conn.Connected());
    CHECK_EQ(conn.Fd(), -1);
}

// TC4：同一个连接多次读写，数据按顺序返回。
TEST(TcpConnection_MultipleRoundTrips) {
    FdPair pair;
    Epoll epoll;
    TcpConnection conn(&epoll, pair.Release(0));
    conn.SetMessageCallback([](TcpConnection& c) {
        c.Send(c.ReadBuffer().RetrieveAllAsString());
    });
    epoll.Add(conn.Fd(), EPOLLIN);

    std::vector<epoll_event> events(8);
    for (int round = 0; round < 3; ++round) {
        const std::string msg = "round-" + std::to_string(round);
        CHECK(WriteAll(pair.fds[1], msg.data(), msg.size()));

        CHECK_EQ(epoll.Wait(events, 500), 1);
        CHECK(conn.HandleRead());

        std::string echoed;
        CHECK(RecvAll(pair.fds[1], echoed, msg.size()));
        CHECK_EQ(echoed, msg);
    }
}
