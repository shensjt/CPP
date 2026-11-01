#pragma once

#include <cctype>
#include <string>
#include <unordered_map>

struct HttpRequest {
    enum class Method { kGet, kPost, kHead, kUnknown };

    Method method = Method::kUnknown;
    std::string target;
    std::string version;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    bool keep_alive = true;

    std::string Path() const {
        const auto pos = target.find('?');
        return pos == std::string::npos ? target : target.substr(0, pos);
    }

    std::string Query() const {
        const auto pos = target.find('?');
        return pos == std::string::npos ? std::string() : target.substr(pos + 1);
    }

    std::string GetHeader(const std::string& name) const {
        std::string key = name;
        for (char& c : key) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        const auto it = headers.find(key);
        return it == headers.end() ? std::string() : it->second;
    }

    void Clear() {
        method = Method::kUnknown;
        target.clear();
        version.clear();
        headers.clear();
        body.clear();
        keep_alive = true;
    }
};
