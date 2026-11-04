#include "server/kv_server.h"

#include <string>
#include <utility>
#include <vector>

#include "log/logger.h"
#include "protocol/resp.h"

KvServer::KvServer(std::string ip, uint16_t port, std::size_t workers,
                   int idle_timeout_seconds, std::string aof_path)
    : handler_(store_),
      aof_path_(std::move(aof_path)),
      server_(std::move(ip), port, workers, idle_timeout_seconds) {
    server_.SetMessageCallback([this](TcpConnection& conn) { OnMessage(conn); });
}

KvServer::~KvServer() = default;

void KvServer::Start() {
    if (!aof_path_.empty()) {
        // 先回放，再挂 AOF；回放期间写命令不能又追加到正在读的文件。
        const auto replay = aof_.Replay(
            aof_path_, [this](const std::vector<std::string>& args) {
                handler_.Execute(args);
            });
        LOG_INFO("aof replay: %zu commands, %zu valid bytes", replay.applied,
                 replay.valid_bytes);

        aof_.Start(aof_path_, replay.valid_bytes);
        store_.SetAof(&aof_);
    }
    server_.Start();
}

void KvServer::Stop() { server_.Stop(); }

void KvServer::OnMessage(TcpConnection& conn) {
    resp::Parser parser;
    resp::Command command;

    while (conn.Connected()) {
        const auto result = parser.Parse(conn.ReadBuffer(), command);
        if (result == resp::Parser::Result::kNeedMore) {
            return;
        }
        if (result == resp::Parser::Result::kError) {
            conn.Send(resp::Error("ERR Protocol error: " +
                                  parser.ErrorMessage()));
            conn.CloseAfterWrite();
            return;
        }
        conn.Send(handler_.Execute(command.args));
    }
}
