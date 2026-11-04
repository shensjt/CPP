#include "server/kv_server.h"

#include <string>
#include <utility>

#include "protocol/resp.h"

KvServer::KvServer(std::string ip, uint16_t port, std::size_t workers,
                   int idle_timeout_seconds)
    : handler_(store_),
      server_(std::move(ip), port, workers, idle_timeout_seconds) {
    server_.SetMessageCallback([this](TcpConnection& conn) { OnMessage(conn); });
}

KvServer::~KvServer() = default;

void KvServer::Start() { server_.Start(); }

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
