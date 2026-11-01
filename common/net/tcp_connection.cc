#include "net/tcp_connection.h"

#include <cerrno>
#include <fcntl.h>
#include <system_error>
#include <sys/socket.h>
#include <unistd.h>

namespace {

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

TcpConnection::TcpConnection(Epoll* epoll, int fd)
    : epoll_(epoll), fd_(fd), connected_(true) {
    SetNonBlocking(fd_);
}

TcpConnection::~TcpConnection() {
    Close();
}

void TcpConnection::SetMessageCallback(std::function<void(TcpConnection&)> cb) {
    on_message_ = std::move(cb);
}

void TcpConnection::Send(const std::string& data) {
    if (!connected_) return;

    write_buffer_.Append(data);
    Flush();
    UpdateInterest();
}

bool TcpConnection::HandleRead() {
    if (!connected_) return false;

    while (true) {
        int saved_errno = 0;
        const ssize_t n = read_buffer_.ReadFd(fd_, &saved_errno);

        if (n > 0) continue;
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

bool TcpConnection::HandleWrite() {
    if (!connected_) return false;

    Flush();
    UpdateInterest();
    return connected_;
}

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
}

void TcpConnection::UpdateInterest() {
    if (!connected_) return;

    std::uint32_t events = EPOLLIN;
    if (write_buffer_.ReadableBytes() > 0) {
        events |= EPOLLOUT;
    }

    try {
        epoll_->Mod(fd_, events);
    } catch (...) {
        Close();
    }
}

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
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}
