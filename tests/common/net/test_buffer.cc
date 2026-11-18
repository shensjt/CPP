// Buffer 单元测试（BF 系列）

#include "net/buffer.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <string>

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

void SetNonBlocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    CHECK(flags >= 0);
    CHECK_EQ(::fcntl(fd, F_SETFL, flags | O_NONBLOCK), 0);
}

}  // namespace

// BF1：初始状态：可读 0，可写等于初始容量，prepend 区为 0。
TEST(Buffer_InitialState) {
    Buffer buffer(16);
    CHECK_EQ(buffer.ReadableBytes(), std::size_t{0});
    CHECK_EQ(buffer.PrependableBytes(), std::size_t{0});
    CHECK_EQ(buffer.WritableBytes(), std::size_t{16});
}

// BF2：Append 后可读，Retrieve 只移动读下标，不搬数据。
TEST(Buffer_AppendRetrieve) {
    Buffer buffer(16);
    buffer.Append("hello", 5);

    CHECK_EQ(buffer.ReadableBytes(), std::size_t{5});
    CHECK_EQ(std::string(buffer.Peek(), buffer.ReadableBytes()), std::string("hello"));

    buffer.Retrieve(2);
    CHECK_EQ(buffer.ReadableBytes(), std::size_t{3});
    CHECK_EQ(std::string(buffer.Peek(), buffer.ReadableBytes()), std::string("llo"));
    CHECK_EQ(buffer.PrependableBytes(), std::size_t{2});
}

// BF3：RetrieveAll 把读写下标都归零。
TEST(Buffer_RetrieveAllResets) {
    Buffer buffer(16);
    buffer.Append("abcdef", 6);
    buffer.Retrieve(2);
    buffer.RetrieveAll();

    CHECK_EQ(buffer.ReadableBytes(), std::size_t{0});
    CHECK_EQ(buffer.PrependableBytes(), std::size_t{0});
    CHECK_EQ(buffer.WritableBytes(), std::size_t{16});
}

// BF4：RetrieveAsString / RetrieveAllAsString 返回内容并消费。
TEST(Buffer_RetrieveAsString) {
    Buffer buffer(16);
    buffer.Append("abcdef", 6);

    CHECK_EQ(buffer.RetrieveAsString(3), std::string("abc"));
    CHECK_EQ(buffer.ReadableBytes(), std::size_t{3});
    CHECK_EQ(buffer.RetrieveAllAsString(), std::string("def"));
    CHECK_EQ(buffer.ReadableBytes(), std::size_t{0});
}

// BF5：EnsureWritableBytes 空间不够时扩容。
TEST(Buffer_EnsureWritableGrows) {
    Buffer buffer(16);
    buffer.EnsureWritableBytes(1000);
    CHECK(buffer.WritableBytes() >= std::size_t{1000});

    const std::string data(1000, 'x');
    buffer.Append(data);
    CHECK_EQ(buffer.ReadableBytes(), data.size());
    CHECK(buffer.RetrieveAllAsString() == data);
}

// BF6：空闲空间足够时 MakeSpace 复用 prepend 区，而不是一味扩容。
TEST(Buffer_MakeSpaceCompacts) {
    Buffer buffer(64);
    const std::string data(4096, 'a');
    buffer.Append(data);
    buffer.Retrieve(2048);

    const std::size_t readable_before = buffer.ReadableBytes();
    buffer.EnsureWritableBytes(32);

    CHECK_EQ(buffer.ReadableBytes(), readable_before);
    CHECK_EQ(buffer.PrependableBytes(), std::size_t{0});
    CHECK(buffer.WritableBytes() >= std::size_t{32});
    CHECK_EQ(std::string(buffer.Peek(), buffer.ReadableBytes()),
             std::string(2048, 'a'));
}

// BF7：ReadFd 的数据不超过可写区，直接写进 buffer。
TEST(Buffer_ReadFdWithinWritable) {
    FdPair pair;
    Buffer buffer(64);
    CHECK(WriteAll(pair.fds[1], "hello", 5));

    int saved_errno = 0;
    const ssize_t n = buffer.ReadFd(pair.fds[0], &saved_errno);

    CHECK_EQ(n, static_cast<ssize_t>(5));
    CHECK_EQ(saved_errno, 0);
    CHECK_EQ(buffer.ReadableBytes(), std::size_t{5});
    CHECK_EQ(buffer.RetrieveAllAsString(), std::string("hello"));
}

// BF8：数据超过可写区，走栈上 extrabuf，再由 Append 接上。
TEST(Buffer_ReadFdUsesExtrabuf) {
    FdPair pair;
    Buffer buffer(64);
    const std::string big(8192, 'y');
    CHECK(WriteAll(pair.fds[1], big.data(), big.size()));

    int saved_errno = 0;
    const ssize_t n = buffer.ReadFd(pair.fds[0], &saved_errno);

    CHECK_EQ(n, static_cast<ssize_t>(big.size()));
    CHECK_EQ(saved_errno, 0);
    CHECK_EQ(buffer.ReadableBytes(), big.size());
    CHECK(buffer.RetrieveAllAsString() == big);
}

// BF9：非阻塞 fd 没数据时返回 -1，并带回 EAGAIN。
TEST(Buffer_ReadFdEagain) {
    FdPair pair;
    SetNonBlocking(pair.fds[0]);
    Buffer buffer(64);

    int saved_errno = 0;
    const ssize_t n = buffer.ReadFd(pair.fds[0], &saved_errno);

    CHECK_EQ(n, static_cast<ssize_t>(-1));
    CHECK(saved_errno == EAGAIN || saved_errno == EWOULDBLOCK);
    CHECK_EQ(buffer.ReadableBytes(), std::size_t{0});
}

// BF10：Append(string) 与 Append(data, len) 等价。
TEST(Buffer_AppendString) {
    Buffer buffer(8);
    const std::string data = "buffer-string";
    buffer.Append(data);
    CHECK(buffer.RetrieveAllAsString() == data);
}

// BF11：Append / Retrieve 交替，已读区域可以被后续 Append 复用。
TEST(Buffer_AppendRetrieveMixed) {
    Buffer buffer(8);
    buffer.Append("abc", 3);
    buffer.Retrieve(1);
    buffer.Append("def", 3);

    CHECK_EQ(buffer.RetrieveAllAsString(), std::string("bcdef"));
}

// BF12：一次 Append 超过初始容量，自动扩容且内容完整。
TEST(Buffer_LargeAppend) {
    Buffer buffer(16);
    const std::string data(100000, 'z');
    buffer.Append(data);

    CHECK_EQ(buffer.ReadableBytes(), data.size());
    CHECK(buffer.RetrieveAllAsString() == data);
}
