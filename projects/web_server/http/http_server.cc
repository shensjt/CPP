#include "http/http_server.h"

#include <string>
#include <utility>

#include "log/logger.h"

namespace {

// 错误响应后关闭连接：协议/请求已经不可信，继续解析可能错位。
// CloseAfterWrite 保证响应写进内核后再关闭，不丢字节。
void SendError(TcpConnection& conn, int status, std::string body) {
    HttpResponse response(status);
    response.SetContentType("text/plain; charset=utf-8");
    response.SetBody(std::move(body));
    response.SetCloseConnection(true);
    conn.Send(response.ToString());
    conn.CloseAfterWrite();
}

}  // namespace

// 两个回调：
// - message：连接可读，进入 HTTP 解析/处理；
// - close：连接从 TcpServer 连接表移除，清理 per-fd parser。
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

// 解析循环支持 keep-alive 和流水线：
// - NeedMore：半个请求，保留 Buffer，等下一次数据；
// - Error：400，发完关闭；
// - Complete：处理，发送响应；keep-alive 则 Reset 继续解析下一个请求。
// 每个连接一个 parser：请求可能分多次到达，状态必须跨回调保留。
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
            // 静态文件处理异常不应打死 worker；回 500 并关闭。
            LOG_ERROR("handler exception on fd %d", conn.Fd());
            SendError(conn, 500, "500 Internal Server Error\n");
            return;
        }

        // 响应头里的 Connection 和实际关闭行为保持一致。
        response.SetCloseConnection(!request.keep_alive);
        const bool head_only = request.method == HttpRequest::Method::kHead;
        conn.Send(response.ToString(head_only));

        if (!conn.Connected()) {
            return;
        }
        if (!request.keep_alive) {
            // 不能 Send 完立刻 Close：可能还有字节在内核/写 Buffer 里。
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

// parser 是有状态的，必须和连接同生命周期；用 unique_ptr 保证 map rehash 后地址稳定。
// 同一连接由 EPOLLONESHOT 保证一次只有一个 worker，所以 parser 不会被并发访问。
HttpParser* HttpServer::GetParser(int fd) {
    std::lock_guard<std::mutex> lock(parsers_mutex_);
    auto& parser = parsers_[fd];
    if (!parser) {
        parser = std::make_unique<HttpParser>();
    }
    return parser.get();
}
