#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include "net/buffer.h"
#include "net/epoll.h"

// 一条已建立的 TCP 连接：fd + 读/写 Buffer + 回调 + 状态。
//
// 生命周期与所有权：
// - 构造时把 fd 设为非阻塞；fd 归本对象所有，Close() 负责 epoll Del + close；
// - 同一时刻只应有一个 worker 操作本对象（TcpServer 用 EPOLLONESHOT 保证）；
// - Close() 幂等；析构也会 Close；
// - 本对象不负责从 TcpServer 的 connections_ 表移除，那是 TcpServer 的职责。
//
// one_shot_：
// - true 时 Send/HandleWrite 不主动 UpdateInterest，
//   由 worker 在事件处理结束时统一 Rearm；
// - 目的是避免处理期间提前重新激活 fd，导致另一个 worker 并发处理同一连接。
class TcpConnection {
public:
    TcpConnection(Epoll* epoll, int fd);
    ~TcpConnection();

    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    int Fd() const { return fd_; }
    bool Connected() const { return connected_; }
    Buffer& ReadBuffer() { return read_buffer_; }

    // 设置可读回调；TcpServer 在 accept 后注入，HttpServer/KvServer 在此解析。
    void SetMessageCallback(std::function<void(TcpConnection&)> cb);
    void SetOneShot(bool enabled) { one_shot_ = enabled; }

    // 追加到写 Buffer 并尽量发送。发不完时：
    // - one_shot_ == false：立即 UpdateInterest 关注 EPOLLOUT；
    // - one_shot_ == true：留给 worker 最后的 Rearm。
    // 写错误会 Close。
    void Send(const std::string& data);

    // 循环 readv 直到 EAGAIN；n == 0 表示对端关闭。
    // 读完后若仍有可读数据，调用一次 on_message_。
    // 返回 false 表示连接已关闭。
    bool HandleRead();

    // EPOLLOUT 到来时调用：Flush 剩余数据；
    // 非 one_shot 时更新关注事件（写空后去掉 EPOLLOUT）。
    bool HandleWrite();

    // 重新注册关注事件：EPOLLIN，必要时加 EPOLLOUT，one_shot 时加 EPOLLONESHOT。
    // 处理完一次事件后必须调用，否则 ONESHOT 下连接不再收到通知。
    void Rearm();

    // 标记“写 Buffer 发完后关闭”；不是立即关闭。
    // 用于 Connection: close 或协议错误：先保证响应发完。
    void CloseAfterWrite();

    // 幂等关闭：epoll Del + close + connected_ = false。
    // 不删除 TcpServer 的连接表项，避免在回调栈上删除自己。
    void Close();

    // 活跃时间：读/写/发送时刷新，空闲扫描据此判断超时。
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
