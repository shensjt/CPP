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

// 最小 TCP 服务器：listen fd + epoll 事件循环 + 可选线程池。
//
// 两种模式：
// - worker_threads == 0：IO 线程直接处理连接读写；
// - worker_threads > 0：IO 线程只 accept/epoll_wait/分发，worker 处理连接；
//   连接注册 EPOLLONESHOT，worker 处理完调用 Rearm。
//
// 线程与生命周期：
// - Start() 阻塞在事件循环；Stop() 只把 running_ 置 false，
//   最多一个 epoll_wait 超时（100ms）后退出；
// - 约定：Stop() 后先 join Start() 所在线程，再析构本对象；
// - 析构时 pool_.reset() 会 join worker，之后才清空连接表。
//
// 连接表：
// - fd -> shared_ptr<TcpConnection>，connections_mutex_ 保护；
// - worker 任务捕获 shared_ptr，即使 IO 线程从表里删除，对象也不会提前析构；
// - CloseConnection 负责移除表项、关闭 fd、减计数、调用 close_callback_。
class TcpServer {
public:
    TcpServer(const std::string& ip, uint16_t port, std::size_t worker_threads = 0,
              int idle_timeout_seconds = 60);
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    void Start();
    void Stop();

    // 在 Start() 之前设置；不设置时默认 Echo。
    // MessageCallback 由 worker 调用；CloseCallback 在连接从表中移除后调用。
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
    void SweepIdle();
    std::shared_ptr<TcpConnection> FindConnection(int fd);

    int listen_fd_ = -1;
    uint16_t port_ = 0;
    Epoll epoll_;
    const bool use_pool_;  // 构造期固定，worker 只读，避免和析构竞态
    std::unique_ptr<ThreadPool> pool_;
    int idle_timeout_seconds_ = 60;
    std::atomic<bool> running_{false};
    std::atomic<std::size_t> connection_count_{0};
    std::mutex connections_mutex_;
    std::unordered_map<int, std::shared_ptr<TcpConnection>> connections_;
    MessageCallback message_callback_;
    CloseCallback close_callback_;
};
