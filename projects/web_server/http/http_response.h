#pragma once

#include <string>
#include <utility>
#include <vector>

// 一个 HTTP/1.1 响应。Content-Length 和 Connection 在序列化时统一生成。
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

    // head_only 为 true 时只输出状态行和 Headers，不输出 Body。
    std::string ToString(bool head_only = false) const;

private:
    int status_code_;
    std::string reason_;
    std::vector<std::pair<std::string, std::string>> headers_;
    std::string body_;
    bool close_ = false;
};
