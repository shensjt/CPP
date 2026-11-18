#include "net/tcp_server.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <utility>
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

// 创建监听 socket：socket -> SO_REUSEADDR -> bind -> listen。
// SO_REUSEADDR 允许服务器重启时复用 TIME_WAIT 的端口，避免 bind 失败。
// getsockname 取出实际端口：传 0 时内核会分配一个空闲端口，测试用得到。
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

TcpServer::TcpServer(const std::string& ip, uint16_t port, std::size_t worker_threads,
                     int idle_timeout_seconds)
    : use_pool_(worker_threads > 0),
      idle_timeout_seconds_(idle_timeout_seconds) {
    // 监听 socket 在构造期就 bind/listen，Start() 只负责事件循环。
    listen_fd_ = CreateListenSocket(ip, port, &port_);
    try {
        if (use_pool_) {
            pool_ = std::make_unique<ThreadPool>(worker_threads);
        }
        epoll_.Add(listen_fd_, EPOLLIN);
    } catch (...) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        throw;
    }
}

TcpServer::~TcpServer() {
    Stop();
    // 先等 worker 退出，之后连接表和 this 才能安全销毁。
    // pool_.reset() 会 join 所有 worker：此时不能再有任务访问 this。
    pool_.reset();

    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        for (auto& [fd, conn] : connections_) {
            conn->Close();
        }
        connections_.clear();
    }

    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
    }
}

// 事件循环：IO 线程在这里阻塞。
// 100ms 超时不是业务需要，而是为了定期检查 running_ 和空闲连接：
// epoll_wait 没有事件时不会返回，靠超时把控制权交回来。
void TcpServer::Start() {
    running_ = true;
    std::vector<epoll_event> events(64);
    auto last_sweep = std::chrono::steady_clock::now();

    while (running_) {
        const int n = epoll_.Wait(events, 100);
        for (int i = 0; i < n && running_; ++i) {
            const int fd = events[i].data.fd;
            const uint32_t ev = events[i].events;

            if (fd == listen_fd_) {
                // 监听 fd 只处理新连接；真正的读写在连接 fd 上。
                HandleAccept();
                continue;
            }
            Dispatch(fd, ev);
        }

        // 每秒扫一次空闲连接；正在被 worker 处理的连接由 InFlight 排除。
        const auto now = std::chrono::steady_clock::now();
        if (idle_timeout_seconds_ > 0 && now - last_sweep >= std::chrono::seconds(1)) {
            SweepIdle();
            last_sweep = now;
        }
    }
}

// 空闲连接清理：
// - 在锁内只做“收集”，不关闭；避免持锁调用 Close 造成长时间阻塞；
// - 跳过 InFlight：worker 正在处理时不能关，否则会把它手里的连接抽走；
// - 收集完在锁外逐个 CloseConnection。
// IO 线程是唯一的扫描者，收集和关闭之间不会有新的 Dispatch 插入。
void TcpServer::SweepIdle() {
    if (idle_timeout_seconds_ <= 0) return;

    const std::int64_t now_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    const std::int64_t limit_ms =
        static_cast<std::int64_t>(idle_timeout_seconds_) * 1000;

    std::vector<int> idle;
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        for (const auto& [fd, conn] : connections_) {
            if (!conn->InFlight() && conn->IdleForMs(now_ms) > limit_ms) {
                idle.push_back(fd);
            }
        }
    }

    for (int fd : idle) {
        CloseConnection(fd);
    }
}

// 只置退出标志，不 join；Start() 所在线程负责在下一轮检查后返回。
// 调用方必须先 join Start() 线程，再让本对象析构。
void TcpServer::Stop() {
    running_ = false;
}

void TcpServer::SetMessageCallback(MessageCallback cb) {
    message_callback_ = std::move(cb);
}

void TcpServer::SetCloseCallback(CloseCallback cb) {
    close_callback_ = std::move(cb);
}

// listen fd 非阻塞，所以 accept 要循环到 EAGAIN：
// 一次 EPOLLIN 可能已经排了多个连接，能接就一次接完。
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

        // 连接 fd 也必须非阻塞，并且 exec 后不继承。
        try {
            SetNonBlocking(fd);
            SetCloexec(fd);
        } catch (...) {
            ::close(fd);
            throw;
        }

        std::shared_ptr<TcpConnection> conn;
        try {
            conn = std::make_shared<TcpConnection>(&epoll_, fd);
        } catch (...) {
            ::close(fd);
            throw;
        }
        // 线程池模式才用 ONESHOT：保证同一 fd 同一时刻只有一个 worker。
        conn->SetOneShot(use_pool_);
        if (message_callback_) {
            conn->SetMessageCallback(message_callback_);
        } else {
            // 没设置回调时退化成 Echo，方便单独跑 TcpServer 测试。
            conn->SetMessageCallback([](TcpConnection& c) {
                c.Send(c.ReadBuffer().RetrieveAllAsString());
            });
        }

        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            connections_.emplace(fd, conn);
        }

        std::uint32_t events = EPOLLIN;
        if (use_pool_) {
            events |= EPOLLONESHOT;
        }
        try {
            epoll_.Add(fd, events);
        } catch (...) {
            CloseConnection(fd);
            throw;
        }
        connection_count_.fetch_add(1);
    }
}

// 两种模式的分叉点：
// - 单线程：直接在 IO 线程处理；
// - 线程池：先找连接，置 InFlight，再把任务投递出去。
// InFlight 在 Post 前置 true、worker 结束后置 false，空闲扫描据此跳过正在处理的连接。
// lambda 捕获 conn 的 shared_ptr，保证即使表项被删除，worker 手里的对象仍然有效。
void TcpServer::Dispatch(int fd, std::uint32_t events) {
    if (!use_pool_) {
        HandleEvents(fd, events);
        return;
    }

    auto conn = FindConnection(fd);
    if (!conn) return;

    conn->SetInFlight(true);
    const bool posted = pool_->Post([this, fd, events, conn] {
        std::lock_guard<std::recursive_mutex> lock(conn->Mutex());
        if (conn->Connected()) {
            HandleEvents(fd, events);
        }
        // 顺序不能反：先清 InFlight，再 Rearm。
        // 如果先 Rearm，fd 会立刻重新激活，IO 线程可能在本 worker
        // 清 InFlight 之前再派一个 worker，两个线程并发读写同一个 Buffer。
        conn->SetInFlight(false);
        if (conn->Connected()) {
            conn->Rearm();
            if (!conn->Connected()) {
                CloseConnection(fd);
            }
        }
    });
    if (!posted) {
        // 队列已关闭，投递失败：恢复状态并关闭连接，避免泄漏。
        conn->SetInFlight(false);
        CloseConnection(fd);
    }
}

// 事件处理顺序：读 -> 错误/挂断 -> 写。
// Rearm 不在这里做，由 Dispatch 的 worker 任务在清 InFlight 之后统一调用。
// 每一步都可能关闭连接，所以每次之后都要重新检查 Connected()。
void TcpServer::HandleEvents(int fd, std::uint32_t events) {
    auto conn = FindConnection(fd);
    if (!conn || !conn->Connected()) return;

    if (events & EPOLLIN) {
        if (!conn->HandleRead()) {
            CloseConnection(fd);
            return;
        }
    }
    if (!conn->Connected()) {
        CloseConnection(fd);
        return;
    }

    // EPOLLERR / EPOLLHUP 即使没有主动注册也可能出现。
    if (events & (EPOLLERR | EPOLLHUP)) {
        conn->Close();
        CloseConnection(fd);
        return;
    }

    if (events & EPOLLOUT) {
        if (!conn->HandleWrite()) {
            CloseConnection(fd);
            return;
        }
    }
    if (!conn->Connected()) {
        CloseConnection(fd);
        return;
    }

    // ONESHOT 的重新注册由 Dispatch 在 SetInFlight(false) 之后执行。
    // 在这里做会让 fd 在 InFlight 仍为 true 时提前激活，导致并发处理。
}

// 返回 shared_ptr 而不是裸指针：调用方持有期间对象不会被析构。
std::shared_ptr<TcpConnection> TcpServer::FindConnection(int fd) {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    auto it = connections_.find(fd);
    return it == connections_.end() ? nullptr : it->second;
}

// 关闭路径统一入口：
// 1. 在锁内把 shared_ptr 从表里摘出来；
// 2. 锁外 Close（避免持锁做 epoll_ctl/close）；
// 3. 减连接计数；
// 4. 通知上层（HttpServer 用它清理 per-fd parser）。
// 如果 worker 还持有 shared_ptr，对象会活到任务结束；它再访问时
// Connected() 已是 false，不会碰已关闭的 fd。
void TcpServer::CloseConnection(int fd) {
    std::shared_ptr<TcpConnection> conn;
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = connections_.find(fd);
        if (it == connections_.end()) return;
        conn = std::move(it->second);
        connections_.erase(it);
    }

    if (conn) {
        std::lock_guard<std::recursive_mutex> lock(conn->Mutex());
        conn->Close();
    }
    connection_count_.fetch_sub(1);

    if (close_callback_) {
        close_callback_(fd);
    }
}
