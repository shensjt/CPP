#pragma once

#include <cstdint>
#include <sys/epoll.h>
#include <vector>

// epoll 的薄封装：只做 fd 注册、修改、删除和等待，不含业务逻辑。
//
// 线程模型：
// - 通常由一个 IO 线程调用 Wait；
// - Add/Mod/Del 可能由 worker 调用（例如 TcpConnection::UpdateInterest），
//   内核允许对同一个 epoll fd 并发 epoll_ctl；封装本身没有额外的可变状态。
//
// 事件约定：
// - Add/Mod 会把 data.fd 设为传入的 fd，Wait 返回时可从 data.fd 取回；
// - 默认是水平触发（LT），没有设置 EPOLLET；
// - 需要“一次通知”时，由调用方在 events 里带 EPOLLONESHOT。
//
// 错误约定：
// - Add/Mod/Del 失败抛 std::system_error；
// - Wait 遇到 EINTR 返回 0，让上层检查退出标志；其他错误抛异常。
class Epoll {
public:
    Epoll();
    ~Epoll();

    Epoll(const Epoll&) = delete;
    Epoll& operator=(const Epoll&) = delete;

    // events 是 EPOLLIN / EPOLLOUT / EPOLLONESHOT 等位掩码。
    // 同一个 fd 只能 Add 一次；重复 Add 会得到 EEXIST。
    void Add(int fd, std::uint32_t events);

    // 修改已注册 fd 的关注事件；不会改变 data.fd。
    // 常用于 EPOLLONESHOT 处理完后重新注册，或动态增删 EPOLLOUT。
    void Mod(int fd, std::uint32_t events);

    // 从 epoll 移除 fd；event 参数由内核忽略（传 nullptr）。
    // 关闭 fd 前调用，避免 fd 复用后旧事件串到新连接。
    void Del(int fd);

    // timeout_ms：-1 阻塞，0 立即返回，>0 毫秒。
    // events 是输出参数，容量决定单次最多返回多少事件。
    // 返回 [0, n)：n 个就绪事件；0 表示超时或被 EINTR 打断。
    int Wait(std::vector<epoll_event>& events, int timeout_ms);

private:
    int epoll_fd_;
};
