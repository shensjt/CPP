#include "net/epoll.h"

#include <cerrno>
#include <system_error>
#include <unistd.h>

namespace {

[[noreturn]] void ThrowSystem(const char* what) {
    throw std::system_error(errno, std::generic_category(), what);
}

}  // namespace

// EPOLL_CLOEXEC：exec 后自动关闭 epoll fd，避免子进程继承。
Epoll::Epoll() : epoll_fd_(::epoll_create1(EPOLL_CLOEXEC)) {
    if (epoll_fd_ < 0) {
        ThrowSystem("epoll_create1");
    }
}

Epoll::~Epoll() {
    if (epoll_fd_ >= 0) {
        ::close(epoll_fd_);
    }
}

void Epoll::Add(int fd, std::uint32_t events) {
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;  // 内核在 Wait 返回时原样带回，上层据此找到连接
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        ThrowSystem("epoll_ctl(ADD)");
    }
}

void Epoll::Mod(int fd, std::uint32_t events) {
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;  // MOD 也要重新填，保持 data 一致
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) < 0) {
        ThrowSystem("epoll_ctl(MOD)");
    }
}

void Epoll::Del(int fd) {
    // DEL 不需要 event 参数
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) < 0) {
        ThrowSystem("epoll_ctl(DEL)");
    }
}

int Epoll::Wait(std::vector<epoll_event>& events, int timeout_ms) {
    if (events.empty()) {
        return 0;
    }

    // epoll_wait 返回就绪事件数；事件内容写进 events[0, n)。
    const int n = ::epoll_wait(epoll_fd_, events.data(),
                               static_cast<int>(events.size()), timeout_ms);
    if (n >= 0) {
        return n;
    }
    if (errno == EINTR) {
        // 被信号打断不是错误：返回 0，让事件循环检查 running_ 等退出标志。
        return 0;
    }
    ThrowSystem("epoll_wait");
}
