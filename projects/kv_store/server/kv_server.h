#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "command/command_handler.h"
#include "net/tcp_server.h"
#include "store/kv_store.h"

// KV 服务：TcpServer（epoll + 线程池） + RESP 解析 + 命令执行。
// 同一连接的半包由 Buffer 保留；不同连接由线程池并发处理。
class KvServer {
public:
    KvServer(std::string ip, uint16_t port, std::size_t workers = 4,
             int idle_timeout_seconds = 60);
    ~KvServer();

    KvServer(const KvServer&) = delete;
    KvServer& operator=(const KvServer&) = delete;

    void Start();
    void Stop();

    uint16_t Port() const { return server_.Port(); }
    std::size_t ConnectionCount() const { return server_.ConnectionCount(); }

private:
    void OnMessage(TcpConnection& conn);

    KvStore store_;
    CommandHandler handler_;
    TcpServer server_;  // 最后声明，最先析构；析构时先 join worker
};
