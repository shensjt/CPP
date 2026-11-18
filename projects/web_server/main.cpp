#include "http/http_server.h"
#include "log/logger.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

namespace {

void PrintUsage() {
    std::cerr << "usage: web_server [--host IP] [--port N] [--root DIR]"
                 " [--threads N] [--timeout SEC]\n";
}

}  // namespace

// 启动参数默认值：
// - 只监听 127.0.0.1，公网入口由 Caddy 反向代理；
// - 4 个 worker；60 秒空闲超时。
int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    std::uint16_t port = 8080;
    std::string root = "build/www";
    std::size_t workers = 4;
    int timeout = 60;

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
        HttpServer server(host, port, root, workers, timeout);
        std::cout << "listening on http://" << host << ":" << server.Port()
                  << " root=" << root << " threads=" << workers
                  << " timeout=" << timeout << "s" << std::endl;
        LOG_INFO("server listening on %s:%u root=%s threads=%zu timeout=%ds",
                 host.c_str(), static_cast<unsigned>(server.Port()), root.c_str(),
                 workers, timeout);
        // Start() 阻塞在 epoll 事件循环；当前没有接信号处理，
        // 生产上由 systemd 发 SIGTERM 终止，属于已知限制。
        server.Start();
    } catch (const std::exception& e) {
        LOG_ERROR("server error: %s", e.what());
        std::cerr << "server error: " << e.what() << std::endl;
        Logger::Instance().Stop();
        return 1;
    }

    Logger::Instance().Stop();
    return 0;
}
