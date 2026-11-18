#pragma once

#include <cstddef>
#include <cstdio>
#include <string>

// ZSCORE 响应和 AOF 里的 score 都用这个格式。
//
// 为什么是 %.17g：
// - double 的十进制往返需要最多 17 位有效数字；
// - 1.5 -> "1.5"，不会像 std::to_string 那样变成 "1.500000"；
// - AOF replay 解析回 double 时能恢复原值。
inline std::string FormatScore(double value) {
    char buffer[64];
    const int written = std::snprintf(buffer, sizeof(buffer), "%.17g", value);
    if (written <= 0) {
        return std::string();
    }
    return std::string(buffer, static_cast<std::size_t>(written));
}
