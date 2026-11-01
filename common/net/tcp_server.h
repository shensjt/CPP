#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

#include "net/epoll.h"

// 最小 TCP Echo 服务器：listen + epoll 事件循环。
// Start() 阻塞运行；Stop() 从其他线程置位，最多一个 Wait 超时后退出。
// 约定：Stop() 之后先 join Start() 所在线程，再析构。
class TcpServer {
public:
    TcpServer(const std::string& ip, uint16_t port);
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    void Start();
    void Stop();

    uint16_t Port() const { return port_; }
    std::size_t ConnectionCount() const { return connection_count_.load(); }

private:
    struct Connection {
        int fd;
        std::string out;  // 还没发完的数据
    };

    void HandleAccept();
    void HandleRead(int fd);
    void Flush(int fd);
    void CloseConnection(int fd);

    int listen_fd_ = -1;
    uint16_t port_ = 0;
    Epoll epoll_;
    std::atomic<bool> running_{false};
    std::atomic<std::size_t> connection_count_{0};
    std::unordered_map<int, Connection> connections_;
};
