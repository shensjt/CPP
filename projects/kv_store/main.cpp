#include "log/logger.h"
#include "server/kv_server.h"

#include <signal.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace {

void PrintUsage() {
    std::cerr << "usage: kv_store [--host IP] [--port N] [--threads N]"
                 " [--timeout SEC]\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    std::uint16_t port = 6380;
    std::size_t workers = 4;
    int timeout = 60;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) {
            host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--threads" && i + 1 < argc) {
            workers = static_cast<std::size_t>(std::atol(argv[++i]));
        } else if (arg == "--timeout" && i + 1 < argc) {
            timeout = std::atoi(argv[++i]);
        } else {
            PrintUsage();
            return 1;
        }
    }

    // 先屏蔽信号，再创建日志/worker 线程，让它们继承同一份阻塞集合。
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &signals, nullptr) != 0) {
        std::cerr << "sigprocmask failed" << std::endl;
        return 1;
    }

    try {
        Logger::Instance().Start("logs/kv.log");
    } catch (const std::exception& e) {
        std::cerr << "logger init failed, fallback to stderr: " << e.what()
                  << std::endl;
    }

    std::thread signal_thread;
    try {
        auto server = std::make_shared<KvServer>(host, port, workers, timeout);
        std::cout << "kv_store listening on " << host << ":" << server->Port()
                  << " threads=" << workers << " timeout=" << timeout << "s"
                  << std::endl;
        LOG_INFO("kv_store listening on %s:%u threads=%zu timeout=%ds",
                 host.c_str(), static_cast<unsigned>(server->Port()), workers,
                 timeout);

        signal_thread = std::thread([server, signals]() mutable {
            int sig = 0;
            if (sigwait(&signals, &sig) == 0) {
                server->Stop();
            }
        });

        server->Start();
        signal_thread.join();
        LOG_INFO("kv_store stopped");
    } catch (const std::exception& e) {
        if (signal_thread.joinable()) {
            signal_thread.detach();
        }
        LOG_ERROR("server error: %s", e.what());
        std::cerr << "server error: " << e.what() << std::endl;
        Logger::Instance().Stop();
        return 1;
    }

    Logger::Instance().Stop();
    return 0;
}
