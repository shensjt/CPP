#pragma once

#include <cstddef>
#include <cstdio>
#include <string>

// ZSCORE 响应和 AOF 里的 score 都用这个格式。
// %.17g 能保证 double 往返不丢精度，同时去掉多余的尾零。
inline std::string FormatScore(double value) {
    char buffer[64];
    const int written = std::snprintf(buffer, sizeof(buffer), "%.17g", value);
    if (written <= 0) {
        return std::string();
    }
    return std::string(buffer, static_cast<std::size_t>(written));
}
