#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "http/http_parser.h"
#include "http/static_file_handler.h"
#include "net/tcp_server.h"

// HTTP 层：解析请求 → 静态文件处理 → 写回响应。
// 每个连接一个 HttpParser，支持 keep-alive 和流水线。
class HttpServer {
public:
    HttpServer(std::string ip, uint16_t port, std::string root,
               std::size_t workers = 4);
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    void Start();
    void Stop();

    uint16_t Port() const { return server_.Port(); }
    std::size_t ConnectionCount() const { return server_.ConnectionCount(); }

private:
    void OnMessage(TcpConnection& conn);
    void OnClose(int fd);
    HttpParser* GetParser(int fd);

    StaticFileHandler handler_;
    std::mutex parsers_mutex_;
    std::unordered_map<int, std::unique_ptr<HttpParser>> parsers_;
    TcpServer server_;  // 最后声明，最先析构；close 回调时上面的成员仍存活
};
