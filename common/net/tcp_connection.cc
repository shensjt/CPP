#include "net/tcp_connection.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <system_error>
#include <sys/socket.h>
#include <unistd.h>

namespace {

std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void SetNonBlocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        throw std::system_error(errno, std::generic_category(), "fcntl(F_GETFL)");
    }
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        throw std::system_error(errno, std::generic_category(), "fcntl(F_SETFL)");
    }
}

}  // namespace

// 非阻塞必须在构造时设置：HandleRead 的契约是“循环读到 EAGAIN”，
// 如果 fd 还是阻塞的，第二次 readv 会永远等下去。
TcpConnection::TcpConnection(Epoll* epoll, int fd)
    : epoll_(epoll), fd_(fd), connected_(true), last_active_ms_(NowMs()) {
    SetNonBlocking(fd_);
}

// 读、写、Send 都会刷新活跃时间；空闲扫描只关“长时间没动且不在处理中”的连接。
void TcpConnection::Touch() {
    last_active_ms_.store(NowMs(), std::memory_order_relaxed);
}

std::int64_t TcpConnection::IdleForMs(std::int64_t now_ms) const {
    const std::int64_t last = last_active_ms_.load(std::memory_order_relaxed);
    return now_ms > last ? now_ms - last : 0;
}

// 兜底关闭：正常路径已经 Close 过时，Close 内部会直接返回。
TcpConnection::~TcpConnection() {
    Close();
    CloseFd();
}

void TcpConnection::SetMessageCallback(std::function<void(TcpConnection&)> cb) {
    on_message_ = std::move(cb);
}

// 业务层主动写：先追加到写 Buffer，再尽量 Flush。
// 一次发不完时，剩余数据留在 write_buffer_：
// - 非 ONESHOT：这里立即把 EPOLLOUT 加进关注集合；
// - ONESHOT：等 worker 处理完统一 Rearm，避免提前激活 fd。
void TcpConnection::Send(const std::string& data) {
    if (!connected_) return;

    Touch();
    write_buffer_.Append(data);
    Flush();

    if (write_buffer_.ReadableBytes() > kMaxWriteBuffer) {
        std::fprintf(stderr, "tcp fd=%d write buffer exceeded (%zu bytes)\n",
                     fd_, write_buffer_.ReadableBytes());
        Close();
        return;
    }

    if (!one_shot_) {
        UpdateInterest();
    }
}

// 一次 EPOLLIN 可能对应任意多字节，所以循环读到 EAGAIN：
//   n > 0   继续读
//   n == 0  对端关闭（EOF）
//   EINTR   被信号打断，重试
//   EAGAIN  这一轮读完了，跳出
//   其他    出错，关闭
// 读完后再调用回调，让解析器从 Buffer 里尽可能多地解析请求。
bool TcpConnection::HandleRead() {
    if (!connected_) return false;

    Touch();
    while (true) {
        int saved_errno = 0;
        const ssize_t n = read_buffer_.ReadFd(fd_, &saved_errno);

        if (n > 0) {
            if (read_buffer_.ReadableBytes() > kMaxReadBuffer) {
                std::fprintf(stderr,
                             "tcp fd=%d read buffer exceeded (%zu bytes)\n",
                             fd_, read_buffer_.ReadableBytes());
                Close();
                return false;
            }
            continue;
        }
        if (n == 0) {
            Close();
            return false;
        }
        if (saved_errno == EINTR) continue;
        if (saved_errno == EAGAIN || saved_errno == EWOULDBLOCK) break;

        Close();
        return false;
    }

    if (connected_ && on_message_ && read_buffer_.ReadableBytes() > 0) {
        on_message_(*this);
    }
    return connected_;
}

// EPOLLOUT 到来：继续把写 Buffer 里的剩余数据发给内核。
// 发完后 UpdateInterest 会把 EPOLLOUT 去掉（非 ONESHOT 模式）。
bool TcpConnection::HandleWrite() {
    if (!connected_) return false;

    Touch();
    Flush();
    if (!one_shot_) {
        UpdateInterest();
    }
    return connected_;
}

// ONESHOT 模式下，worker 处理完一次事件后必须重新注册：
// 根据写 Buffer 是否还有数据，决定是否同时关注 EPOLLOUT。
void TcpConnection::Rearm() {
    if (connected_) {
        UpdateInterest();
    }
}

// 尽量把 write_buffer_ 里的数据拷进内核发送缓冲区：
//   n > 0  已发送 n 字节，从 Buffer 消费后继续
//   EINTR  重试
//   EAGAIN 内核发送缓冲区满，保留剩余数据，等 EPOLLOUT
//   其他   关闭
// MSG_NOSIGNAL：对端关闭后 send 返回 EPIPE，而不是触发 SIGPIPE 杀进程。
void TcpConnection::Flush() {
    while (connected_ && write_buffer_.ReadableBytes() > 0) {
        const ssize_t n = ::send(fd_, write_buffer_.Peek(),
                                 write_buffer_.ReadableBytes(), MSG_NOSIGNAL);
        if (n > 0) {
            write_buffer_.Retrieve(static_cast<std::size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;

        Close();
        return;
    }

    // CloseAfterWrite：只有写 Buffer 发空后才真正关闭。
    if (connected_ && close_after_write_ && write_buffer_.ReadableBytes() == 0) {
        Close();
    }
}

// 设置“发完再关”：如果写 Buffer 已经是空的，直接关闭；
// 否则等 Flush 把剩余数据发完，在 Flush 末尾关闭。
void TcpConnection::CloseAfterWrite() {
    close_after_write_ = true;
    if (connected_ && write_buffer_.ReadableBytes() == 0) {
        Close();
    }
}

// 关注集合的三个部分：
//   EPOLLIN    始终关注（读请求/EOF）
//   EPOLLOUT   只有写 Buffer 非空时才关注，避免水平触发空转
//   ONESHOT    线程池模式下一次事件后禁用，处理完由 Rearm 重新注册
void TcpConnection::UpdateInterest() {
    if (!connected_) return;

    std::uint32_t events = EPOLLIN;
    if (write_buffer_.ReadableBytes() > 0) {
        events |= EPOLLOUT;
    }
    if (one_shot_) {
        events |= EPOLLONESHOT;
    }

    try {
        epoll_->Mod(fd_, events);
    } catch (...) {
        Close();
    }
}

// 幂等关闭：可能来自读 EOF、写错误、空闲扫描、析构等多条路径。
// 只做 epoll Del 和状态标记，fd 先暂存；TcpServer 从连接表移除后再 CloseFd。
// 如果这里直接 close，内核可能立刻把同一个 fd 复用给新连接，
// 而旧表项还没删，emplace 会失败并误关新连接。
// 不在这里删除 TcpServer 的连接表项：对象可能正在自己的回调栈上。
void TcpConnection::Close() {
    if (!connected_) return;
    connected_ = false;

    if (epoll_ != nullptr) {
        try {
            epoll_->Del(fd_);
        } catch (...) {
            // fd 已不可用时忽略
        }
    }
    pending_close_fd_ = fd_;
    fd_ = -1;
}

// 由 TcpServer 在表项移除后调用，保证 fd 的复用发生在表项清理之后。
void TcpConnection::CloseFd() {
    if (pending_close_fd_ >= 0) {
        ::close(pending_close_fd_);
        pending_close_fd_ = -1;
    }
}
