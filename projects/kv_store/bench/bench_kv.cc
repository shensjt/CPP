// 简易 KV 压测客户端：多线程、长连接、同步请求。
// 只用于相对比较，不做延迟分位数统计。
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "protocol/resp.h"

namespace {

struct Config {
    std::string host = "127.0.0.1";
    std::uint16_t port = 6380;
    int clients = 16;
    int requests = 20000;
    int value_size = 64;
    int keys = 1024;
    std::string mode = "mixed";  // set / get / mixed
};

bool SendAll(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n =
            ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

bool ReadExact(int fd, std::size_t count) {
    char buffer[4096];
    while (count > 0) {
        const std::size_t want = count < sizeof(buffer) ? count : sizeof(buffer);
        const ssize_t n = ::recv(fd, buffer, want, 0);
        if (n > 0) {
            count -= static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

int Connect(const Config& config) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config.port);
    if (::inet_pton(AF_INET, config.host.c_str(), &addr.sin_addr) != 1 ||
        ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }

    const int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    timeval timeout{5, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    return fd;
}

std::size_t GetReplySize(int value_size) {
    // $<len>\r\n<value>\r\n
    return static_cast<std::size_t>(value_size) +
           std::to_string(value_size).size() + 5;
}

}  // namespace

int main(int argc, char** argv) {
    Config config;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) {
            config.host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            config.port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--clients" && i + 1 < argc) {
            config.clients = std::atoi(argv[++i]);
        } else if (arg == "--requests" && i + 1 < argc) {
            config.requests = std::atoi(argv[++i]);
        } else if (arg == "--value-size" && i + 1 < argc) {
            config.value_size = std::atoi(argv[++i]);
        } else if (arg == "--keys" && i + 1 < argc) {
            config.keys = std::atoi(argv[++i]);
        } else if (arg == "--mode" && i + 1 < argc) {
            config.mode = argv[++i];
        } else {
            std::cerr << "usage: bench_kv [--host IP] [--port N] [--clients N]"
                         " [--requests N] [--value-size N] [--keys N]"
                         " [--mode set|get|mixed]\n";
            return 1;
        }
    }

    if (config.clients <= 0 || config.requests <= 0 || config.value_size <= 0 ||
        config.keys <= 0 ||
        (config.mode != "set" && config.mode != "get" &&
         config.mode != "mixed")) {
        std::cerr << "invalid arguments\n";
        return 1;
    }

    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::atomic<long long> errors{0};
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(config.clients));

    for (int id = 0; id < config.clients; ++id) {
        threads.emplace_back([&, id] {
            const int fd = Connect(config);
            if (fd < 0) {
                errors.fetch_add(1);
                ready.fetch_add(1);
                return;
            }

            const std::string value(static_cast<std::size_t>(config.value_size),
                                    'x');
            std::vector<std::string> set_wires;
            std::vector<std::string> get_wires;
            set_wires.reserve(static_cast<std::size_t>(config.keys));
            get_wires.reserve(static_cast<std::size_t>(config.keys));
            for (int i = 0; i < config.keys; ++i) {
                const std::string key =
                    "k" + std::to_string(id) + ":" + std::to_string(i);
                set_wires.push_back(resp::EncodeCommand({"SET", key, value}));
                get_wires.push_back(resp::EncodeCommand({"GET", key}));
            }

            // 预热：把 key 先写进去，GET 才有数据可读。
            for (const auto& wire : set_wires) {
                if (!SendAll(fd, wire) || !ReadExact(fd, 5)) {
                    errors.fetch_add(1);
                    ::close(fd);
                    ready.fetch_add(1);
                    return;
                }
            }

            ready.fetch_add(1);
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            const std::size_t get_reply = GetReplySize(config.value_size);
            for (int i = 0; i < config.requests && errors.load() == 0; ++i) {
                const std::size_t index =
                    static_cast<std::size_t>(i % config.keys);
                bool ok = true;
                if (config.mode == "set") {
                    ok = SendAll(fd, set_wires[index]) && ReadExact(fd, 5);
                } else if (config.mode == "get") {
                    ok = SendAll(fd, get_wires[index]) &&
                         ReadExact(fd, get_reply);
                } else if (i % 2 == 0) {
                    ok = SendAll(fd, set_wires[index]) && ReadExact(fd, 5);
                } else {
                    ok = SendAll(fd, get_wires[index]) &&
                         ReadExact(fd, get_reply);
                }
                if (!ok) {
                    errors.fetch_add(1);
                    break;
                }
            }
            ::close(fd);
        });
    }

    while (ready.load() < config.clients) {
        std::this_thread::yield();
    }

    const auto start = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);
    for (auto& thread : threads) {
        thread.join();
    }
    const auto end = std::chrono::steady_clock::now();

    const double seconds =
        std::chrono::duration<double>(end - start).count();
    const long long total =
        static_cast<long long>(config.clients) * config.requests;
    const double qps = seconds > 0 ? static_cast<double>(total) / seconds : 0;

    std::cout << "mode=" << config.mode << " clients=" << config.clients
              << " requests=" << total << " seconds=" << seconds
              << " qps=" << static_cast<long long>(qps)
              << " errors=" << errors.load() << '\n';
    return errors.load() == 0 ? 0 : 1;
}
