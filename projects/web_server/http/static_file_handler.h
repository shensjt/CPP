#pragma once

#include <filesystem>
#include <string>

#include "http/http_request.h"
#include "http/http_response.h"

// 把 URL 路径映射到 document root 下的文件，只允许 GET/HEAD。
class StaticFileHandler {
public:
    explicit StaticFileHandler(std::string root);

    HttpResponse Handle(const HttpRequest& request) const;

private:
    std::filesystem::path root_;
};
