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
//
// 结构：
// - TcpServer 负责 epoll/线程池/连接生命周期；
// - HttpServer 通过 message/close 回调接入；
// - 每个连接一个 HttpParser，支持 keep-alive 和流水线；
// - 同一 fd 由 EPOLLONESHOT 保证一次只有一个 worker，parser 无需额外锁。
//
// 成员顺序：
// - server_ 最后声明，最先析构；它的析构会 join worker，
//   之后 handler_/parsers_ 才销毁，close 回调不会访问悬空成员。
class HttpServer {
public:
    HttpServer(std::string ip, uint16_t port, std::string root,
               std::size_t workers = 4, int idle_timeout_seconds = 60);
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
    // fd -> parser；连接关闭时由 close 回调 erase。
    // mutex 保护 map 本身，parser 的内容由单 worker 独占。
    std::mutex parsers_mutex_;
    std::unordered_map<int, std::unique_ptr<HttpParser>> parsers_;
    TcpServer server_;  // 最后声明，最先析构；close 回调时上面的成员仍存活
};
