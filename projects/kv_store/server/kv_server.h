#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "aof/aof.h"
#include "command/command_handler.h"
#include "net/tcp_server.h"
#include "store/kv_store.h"

// KV 服务：TcpServer（epoll + 线程池） + RESP 解析 + 命令执行。
//
// 请求链路：
//   TcpServer -> worker -> resp::Parser -> CommandHandler -> KvStore -> Aof
//
// 半包：RESP 解析器无状态，NeedMore 时 Buffer 不动，不需要 per-connection parser。
//
// 启动顺序（aof_path 非空时）：
//   Replay -> Aof::Start（截断尾部）-> store_.SetAof -> server_.Start
// 先恢复数据，再对外服务；回放期间不能挂 AOF，否则边读边写。
//
// 停机顺序由成员析构保证：
//   server_（join worker）-> aof_（flush + fsync）-> KvStore
class KvServer {
public:
    KvServer(std::string ip, uint16_t port, std::size_t workers = 4,
             int idle_timeout_seconds = 60, std::string aof_path = "");
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
    Aof aof_;
    std::string aof_path_;
    // 最后声明，最先析构；先 join worker，再停 AOF，避免 worker 写已关闭的文件。
    TcpServer server_;
};
