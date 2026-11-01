#pragma once

#include <cstdint>
#include <sys/epoll.h>
#include <vector>

// epoll 的薄封装，只做事件注册和等待。
// Add/Mod 会把 data.fd 设为传入的 fd；Wait 的 vector 大小即单次最多返回的事件数。
// Wait 遇 EINTR 返回 0，其余错误抛 std::system_error。
class Epoll {
public:
    Epoll();
    ~Epoll();

    Epoll(const Epoll&) = delete;
    Epoll& operator=(const Epoll&) = delete;

    void Add(int fd, std::uint32_t events);
    void Mod(int fd, std::uint32_t events);
    void Del(int fd);

    // timeout_ms: -1 阻塞，0 立即返回，>0 毫秒。
    // 返回就绪事件数，有效范围 events[0, n)。
    int Wait(std::vector<epoll_event>& events, int timeout_ms);

private:
    int epoll_fd_;
};
