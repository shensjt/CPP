#pragma once

#include <string>
#include <utility>
#include <vector>

// 一个 HTTP/1.1 响应。
//
// 设计要点：
// - headers_ 用 vector 保存插入顺序，输出时顺序稳定；
// - SetHeader 大小写不敏感地替换同名 Header，但保留第一次写入的 key 写法；
// - Content-Length 和 Connection 不在这里手动维护，ToString 时统一生成；
// - head_only 为 true 时只输出状态行和 Headers，用于 HEAD 请求。
class HttpResponse {
public:
    explicit HttpResponse(int status_code = 200);

    void SetStatus(int status_code);
    void SetHeader(const std::string& name, const std::string& value);
    void SetContentType(const std::string& type);
    void SetBody(std::string body);
    void SetCloseConnection(bool close) { close_ = close; }

    int StatusCode() const { return status_code_; }
    const std::string& Body() const { return body_; }

    // 序列化为响应字节流；Content-Length 始终是 body_ 的长度。
    // head_only 为 true 时只输出状态行和 Headers，不输出 Body。
    std::string ToString(bool head_only = false) const;

private:
    int status_code_;
    std::string reason_;
    std::vector<std::pair<std::string, std::string>> headers_;
    std::string body_;
    bool close_ = false;
};
