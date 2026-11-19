#include "http/http_server.h"
#include "log/logger.h"

#include <pthread.h>
#include <signal.h>
#include <time.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <thread>

namespace {

void PrintUsage() {
    std::cerr << "usage: web_server [--host IP] [--port N] [--root DIR]"
                 " [--threads N] [--timeout SEC] [--cache-bytes N]\n";
}

}  // namespace

// 启动参数默认值：
// - 只监听 127.0.0.1，公网入口由 Caddy 反向代理；
// - 4 个 worker；60 秒空闲超时；16MB LRU 缓存。
int main(int argc, char** argv) {
    // 先屏蔽信号，保证 Logger/ThreadPool 创建的线程都继承该掩码，
    // 只由主线程的 sigtimedwait 消费，避免信号打断任意 worker。
    sigset_t stop_signals;
    sigemptyset(&stop_signals);
    sigaddset(&stop_signals, SIGINT);
    sigaddset(&stop_signals, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &stop_signals, nullptr) != 0) {
        std::cerr << "sigprocmask failed" << std::endl;
        return 1;
    }

    std::string host = "127.0.0.1";
    std::uint16_t port = 8080;
    std::string root = "build/www";
    std::size_t workers = 4;
    int timeout = 60;
    std::size_t cache_bytes = 16 * 1024 * 1024;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) {
            host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--root" && i + 1 < argc) {
            root = argv[++i];
        } else if (arg == "--threads" && i + 1 < argc) {
            workers = static_cast<std::size_t>(std::atol(argv[++i]));
        } else if (arg == "--timeout" && i + 1 < argc) {
            timeout = std::atoi(argv[++i]);
        } else if (arg == "--cache-bytes" && i + 1 < argc) {
            cache_bytes = static_cast<std::size_t>(
                std::strtoull(argv[++i], nullptr, 10));
        } else {
            PrintUsage();
            return 1;
        }
    }

    // 日志启动失败不阻塞服务：Logger 内部会回退到 stderr。
    try {
        Logger::Instance().Start("logs/server.log");
    } catch (const std::exception& e) {
        std::cerr << "logger init failed, fallback to stderr: " << e.what()
                  << std::endl;
    }

    try {
        HttpServer server(host, port, root, workers, timeout, cache_bytes);
        std::cout << "listening on http://" << host << ":" << server.Port()
                  << " root=" << root << " threads=" << workers
                  << " timeout=" << timeout << "s cache=" << cache_bytes
                  << "B" << std::endl;
        LOG_INFO(
            "server listening on %s:%u root=%s threads=%zu timeout=%ds cache=%zuB",
            host.c_str(), static_cast<unsigned>(server.Port()), root.c_str(),
            workers, timeout, cache_bytes);
        // Start() 阻塞在 epoll 事件循环；主线程等 SIGINT/SIGTERM，
        // 收到后 Stop()，等 Start 返回、worker join，再退出。
        std::atomic<bool> done{false};
        std::exception_ptr server_error;
        std::thread server_thread([&] {
            // 只命名线程，便于 pidstat 区分 IO/worker。
            pthread_setname_np(pthread_self(), "io");
            try {
                server.Start();
            } catch (...) {
                server_error = std::current_exception();
            }
            done.store(true, std::memory_order_release);
        });

        while (!done.load(std::memory_order_acquire)) {
            timespec wait{0, 100 * 1000 * 1000};
            const int sig = ::sigtimedwait(&stop_signals, nullptr, &wait);
            if (sig == SIGINT || sig == SIGTERM) {
                LOG_INFO("received signal %d, stopping", sig);
                server.Stop();
                break;
            }
        }

        server_thread.join();
        if (server_error) {
            std::rethrow_exception(server_error);
        }
    } catch (const std::exception& e) {
        LOG_ERROR("server error: %s", e.what());
        std::cerr << "server error: " << e.what() << std::endl;
        Logger::Instance().Stop();
        return 1;
    }

    Logger::Instance().Stop();
    return 0;
}
