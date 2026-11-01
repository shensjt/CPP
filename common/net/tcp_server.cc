#include "net/tcp_server.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace {

[[noreturn]] void ThrowSystem(const char* what) {
    throw std::system_error(errno, std::generic_category(), what);
}

void SetNonBlocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) ThrowSystem("fcntl(F_GETFL)");
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) ThrowSystem("fcntl(F_SETFL)");
}

void SetCloexec(int fd) {
    const int flags = ::fcntl(fd, F_GETFD, 0);
    if (flags < 0) ThrowSystem("fcntl(F_GETFD)");
    if (::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) ThrowSystem("fcntl(F_SETFD)");
}

int CreateListenSocket(const std::string& ip, uint16_t port, uint16_t* bound_port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) ThrowSystem("socket");

    const int opt = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        ::close(fd);
        ThrowSystem("setsockopt");
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        throw std::invalid_argument("invalid ip: " + ip);
    }

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        ThrowSystem("bind");
    }
    if (::listen(fd, SOMAXCONN) < 0) {
        ::close(fd);
        ThrowSystem("listen");
    }

    sockaddr_in actual{};
    socklen_t len = sizeof(actual);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&actual), &len) < 0) {
        ::close(fd);
        ThrowSystem("getsockname");
    }
    *bound_port = ntohs(actual.sin_port);

    SetNonBlocking(fd);
    SetCloexec(fd);
    return fd;
}

}  // namespace

TcpServer::TcpServer(const std::string& ip, uint16_t port) {
    listen_fd_ = CreateListenSocket(ip, port, &port_);
    try {
        epoll_.Add(listen_fd_, EPOLLIN);
    } catch (...) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        throw;
    }
}

TcpServer::~TcpServer() {
    Stop();
    for (auto& [fd, conn] : connections_) {
        ::close(fd);
    }
    connections_.clear();
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
    }
}

void TcpServer::Start() {
    running_ = true;
    std::vector<epoll_event> events(64);

    while (running_) {
        const int n = epoll_.Wait(events, 100);
        for (int i = 0; i < n && running_; ++i) {
            const int fd = events[i].data.fd;
            const uint32_t ev = events[i].events;

            if (fd == listen_fd_) {
                HandleAccept();
                continue;
            }

            if (ev & EPOLLIN) {
                HandleRead(fd);
            }
            if (connections_.find(fd) == connections_.end()) {
                continue;
            }
            if (ev & (EPOLLERR | EPOLLHUP)) {
                CloseConnection(fd);
                continue;
            }
            if (ev & EPOLLOUT) {
                Flush(fd);
            }
        }
    }
}

void TcpServer::Stop() {
    running_ = false;
}

void TcpServer::HandleAccept() {
    while (true) {
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        const int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno == EINTR) continue;
            ThrowSystem("accept");
        }

        try {
            SetNonBlocking(fd);
            SetCloexec(fd);
            connections_.emplace(fd, Connection{fd, {}});
            connection_count_.fetch_add(1);
            epoll_.Add(fd, EPOLLIN);
        } catch (...) {
            ::close(fd);
            throw;
        }
    }
}

void TcpServer::HandleRead(int fd) {
    auto it = connections_.find(fd);
    if (it == connections_.end()) return;

    char buf[64 * 1024];
    while (true) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            it->second.out.append(buf, static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0) {
            CloseConnection(fd);
            return;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        CloseConnection(fd);
        return;
    }

    Flush(fd);
}

void TcpServer::Flush(int fd) {
    auto it = connections_.find(fd);
    if (it == connections_.end()) return;

    std::string& out = it->second.out;
    std::size_t sent = 0;
    while (sent < out.size()) {
        const ssize_t n = ::send(fd, out.data() + sent, out.size() - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        CloseConnection(fd);
        return;
    }

    out.erase(0, sent);
    if (out.empty()) {
        epoll_.Mod(fd, EPOLLIN);
    } else {
        epoll_.Mod(fd, EPOLLIN | EPOLLOUT);
    }
}

void TcpServer::CloseConnection(int fd) {
    auto it = connections_.find(fd);
    if (it == connections_.end()) return;

    try {
        epoll_.Del(fd);
    } catch (...) {
        // fd 已经不可用时忽略 Del 失败，继续关闭
    }
    ::close(fd);
    connections_.erase(it);
    connection_count_.fetch_sub(1);
}
