#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include "net/buffer.h"
#include "net/epoll.h"

// 一个已建立的连接：fd + 读 Buffer + 写 Buffer + 回调。
// 构造时把 fd 设为非阻塞；HandleRead/HandleWrite 返回 false 表示已关闭。
// OneShot 模式下 Send/HandleWrite 不自动重新注册，由 worker 最后调用 Rearm()。
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
    void SetOneShot(bool enabled) { one_shot_ = enabled; }

    // 追加到写缓冲并尽量发送；发不完时自动关注 EPOLLOUT。
    void Send(const std::string& data);

    bool HandleRead();
    bool HandleWrite();
    void Rearm();
    void CloseAfterWrite();
    void Close();

    void Touch();
    void SetInFlight(bool in_flight) {
        in_flight_.store(in_flight, std::memory_order_relaxed);
    }
    bool InFlight() const {
        return in_flight_.load(std::memory_order_relaxed);
    }
    std::int64_t IdleForMs(std::int64_t now_ms) const;

private:
    void Flush();
    void UpdateInterest();

    Epoll* epoll_;
    int fd_;
    bool connected_;
    Buffer read_buffer_;
    Buffer write_buffer_;
    std::function<void(TcpConnection&)> on_message_;
    bool one_shot_ = false;
    bool close_after_write_ = false;
    std::atomic<std::int64_t> last_active_ms_{0};
    std::atomic<bool> in_flight_{false};
};
