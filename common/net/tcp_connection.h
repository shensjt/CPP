#pragma once

#include <functional>
#include <string>

#include "net/buffer.h"
#include "net/epoll.h"

// 一个已建立的连接：fd + 读 Buffer + 写 Buffer + 回调。
// 构造时把 fd 设为非阻塞；HandleRead/HandleWrite 返回 false 表示已关闭。
class TcpConnection {
public:
    TcpConnection(Epoll* epoll, int fd);
    ~TcpConnection();

    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    int Fd() const { return fd_; }
    bool Connected() const { return connected_; }
    Buffer& ReadBuffer() { return read_buffer_; }

    void SetMessageCallback(std::function<void(TcpConnection&)> cb);

    // 追加到写缓冲并尽量发送；发不完时自动关注 EPOLLOUT。
    void Send(const std::string& data);

    bool HandleRead();
    bool HandleWrite();
    void Close();

private:
    void Flush();
    void UpdateInterest();

    Epoll* epoll_;
    int fd_;
    bool connected_;
    Buffer read_buffer_;
    Buffer write_buffer_;
    std::function<void(TcpConnection&)> on_message_;
};
