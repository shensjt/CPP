#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "net/epoll.h"
#include "net/tcp_connection.h"
#include "thread_pool/thread_pool.h"

// 最小 TCP Echo 服务器：listen + epoll 事件循环。
// worker_threads == 0：IO 线程直接处理；> 0：IO 线程分发，worker 处理连接。
// 池模式下连接使用 EPOLLONESHOT，worker 处理完调用 Rearm()。
// 约定：Stop() 之后先 join Start() 所在线程，再析构。
class TcpServer {
public:
    TcpServer(const std::string& ip, uint16_t port, std::size_t worker_threads = 0);
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    void Start();
    void Stop();

    // 在 Start() 之前设置；不设置时默认 Echo。
    using MessageCallback = std::function<void(TcpConnection&)>;
    using CloseCallback = std::function<void(int)>;
    void SetMessageCallback(MessageCallback cb);
    void SetCloseCallback(CloseCallback cb);

    uint16_t Port() const { return port_; }
    std::size_t ConnectionCount() const { return connection_count_.load(); }

private:
    void HandleAccept();
    void HandleEvents(int fd, std::uint32_t events);
    void Dispatch(int fd, std::uint32_t events);
    void CloseConnection(int fd);
    std::shared_ptr<TcpConnection> FindConnection(int fd);

    int listen_fd_ = -1;
    uint16_t port_ = 0;
    Epoll epoll_;
    const bool use_pool_;
    std::unique_ptr<ThreadPool> pool_;
    std::atomic<bool> running_{false};
    std::atomic<std::size_t> connection_count_{0};
    std::mutex connections_mutex_;
    std::unordered_map<int, std::shared_ptr<TcpConnection>> connections_;
    MessageCallback message_callback_;
    CloseCallback close_callback_;
};
