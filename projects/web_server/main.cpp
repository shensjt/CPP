#include "http/http_server.h"

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

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    std::uint16_t port = 8080;
    std::string root = "projects/web_server/www";
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

    try {
        HttpServer server(host, port, root, workers, timeout);
        std::cout << "listening on http://" << host << ":" << server.Port()
                  << " root=" << root << " threads=" << workers
                  << " timeout=" << timeout << "s" << std::endl;
        server.Start();
    } catch (const std::exception& e) {
        std::cerr << "server error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
