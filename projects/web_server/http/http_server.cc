#include "http/http_server.h"

#include <string>
#include <utility>

#include "log/logger.h"

namespace {

void SendError(TcpConnection& conn, int status, std::string body) {
    HttpResponse response(status);
    response.SetContentType("text/plain; charset=utf-8");
    response.SetBody(std::move(body));
    response.SetCloseConnection(true);
    conn.Send(response.ToString());
    conn.CloseAfterWrite();
}

}  // namespace

HttpServer::HttpServer(std::string ip, uint16_t port, std::string root,
                       std::size_t workers, int idle_timeout_seconds)
    : handler_(std::move(root)),
      server_(ip, port, workers, idle_timeout_seconds) {
    server_.SetMessageCallback([this](TcpConnection& conn) { OnMessage(conn); });
    server_.SetCloseCallback([this](int fd) { OnClose(fd); });
}

HttpServer::~HttpServer() = default;

void HttpServer::Start() {
    server_.Start();
}

void HttpServer::Stop() {
    server_.Stop();
}

void HttpServer::OnMessage(TcpConnection& conn) {
    HttpParser* parser = GetParser(conn.Fd());

    while (conn.Connected()) {
        HttpRequest request;
        const auto result = parser->Parse(conn.ReadBuffer(), request);

        if (result == HttpParser::Result::kNeedMore) {
            return;
        }
        if (result == HttpParser::Result::kError) {
            LOG_WARN("bad request from fd %d", conn.Fd());
            SendError(conn, 400, "400 Bad Request\n");
            return;
        }

        HttpResponse response;
        try {
            response = handler_.Handle(request);
        } catch (...) {
            LOG_ERROR("handler exception on fd %d", conn.Fd());
            SendError(conn, 500, "500 Internal Server Error\n");
            return;
        }

        response.SetCloseConnection(!request.keep_alive);
        const bool head_only = request.method == HttpRequest::Method::kHead;
        conn.Send(response.ToString(head_only));

        if (!conn.Connected()) {
            return;
        }
        if (!request.keep_alive) {
            conn.CloseAfterWrite();
            return;
        }

        parser->Reset();
    }
}

void HttpServer::OnClose(int fd) {
    std::lock_guard<std::mutex> lock(parsers_mutex_);
    parsers_.erase(fd);
}

HttpParser* HttpServer::GetParser(int fd) {
    std::lock_guard<std::mutex> lock(parsers_mutex_);
    auto& parser = parsers_[fd];
    if (!parser) {
        parser = std::make_unique<HttpParser>();
    }
    return parser.get();
}
