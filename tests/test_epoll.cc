// Epoll 封装单元测试（EP 系列）
//
// 用 socketpair 造一对已连接的 fd，不依赖端口和外部网络。

#include "net/epoll.h"

#include <sys/socket.h>
#include <unistd.h>

#include <system_error>
#include <vector>

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
};

int WriteByte(int fd, char c) {
    return static_cast<int>(::write(fd, &c, 1));
}

// 返回读到的字节值；失败返回 -1。
int ReadByte(int fd) {
    char c = 0;
    const ssize_t n = ::read(fd, &c, 1);
    return n == 1 ? static_cast<int>(static_cast<unsigned char>(c)) : -1;
}

}  // namespace

// EP1：注册 EPOLLIN，对端写入后可读，事件带 data.fd。
TEST(Epoll_AddAndWaitReadable) {
    FdPair pair;
    Epoll ep;

    ep.Add(pair.fds[0], EPOLLIN);
    CHECK_EQ(WriteByte(pair.fds[1], 'x'), 1);

    std::vector<epoll_event> events(8);
    const int n = ep.Wait(events, 500);

    CHECK_EQ(n, 1);
    if (n == 1) {
        CHECK((events[0].events & EPOLLIN) != 0);
        CHECK_EQ(events[0].data.fd, pair.fds[0]);
        CHECK_EQ(ReadByte(pair.fds[0]), 'x');
    }
}

// EP2：没有事件时，Wait 在超时后返回 0。
TEST(Epoll_WaitTimeout) {
    FdPair pair;
    Epoll ep;
    ep.Add(pair.fds[0], EPOLLIN);

    std::vector<epoll_event> events(8);
    CHECK_EQ(ep.Wait(events, 20), 0);
}

// EP3：Mod 把监听事件从 EPOLLIN 改成 EPOLLOUT。
TEST(Epoll_ModToWritable) {
    FdPair pair;
    Epoll ep;

    ep.Add(pair.fds[0], EPOLLIN);
    ep.Mod(pair.fds[0], EPOLLOUT);

    std::vector<epoll_event> events(8);
    const int n = ep.Wait(events, 500);

    CHECK_EQ(n, 1);
    if (n == 1) {
        CHECK((events[0].events & EPOLLOUT) != 0);
    }
}

// EP4：Del 之后不再收到该 fd 的事件。
TEST(Epoll_DelRemovesFd) {
    FdPair pair;
    Epoll ep;

    ep.Add(pair.fds[0], EPOLLIN);
    ep.Del(pair.fds[0]);
    CHECK_EQ(WriteByte(pair.fds[1], 'y'), 1);

    std::vector<epoll_event> events(8);
    CHECK_EQ(ep.Wait(events, 50), 0);
}

// EP5：EPOLLONESHOT 只通知一次，重新 Mod 后才能再次收到。
TEST(Epoll_OneShot) {
    FdPair pair;
    Epoll ep;

    ep.Add(pair.fds[0], EPOLLIN | EPOLLONESHOT);
    CHECK_EQ(WriteByte(pair.fds[1], 'z'), 1);

    std::vector<epoll_event> events(8);
    CHECK_EQ(ep.Wait(events, 500), 1);
    // 不读数据，fd 仍然可读，但 ONESHOT 已经解除监听。
    CHECK_EQ(ep.Wait(events, 50), 0);

    ep.Mod(pair.fds[0], EPOLLIN | EPOLLONESHOT);
    CHECK_EQ(ep.Wait(events, 500), 1);

    CHECK_EQ(ReadByte(pair.fds[0]), 'z');
}

// EP6：非法 fd 让 Add 抛 std::system_error。
TEST(Epoll_InvalidFdThrows) {
    Epoll ep;
    bool threw = false;

    try {
        ep.Add(-1, EPOLLIN);
    } catch (const std::system_error&) {
        threw = true;
    }

    CHECK(threw);
}

// EP7：传入空 vector 时直接返回 0，不调用 epoll_wait。
TEST(Epoll_EmptyBufferReturnsZero) {
    Epoll ep;
    std::vector<epoll_event> events;
    CHECK_EQ(ep.Wait(events, 0), 0);
}
