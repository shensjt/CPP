#pragma once

#include <cstddef>

#include "http/http_request.h"
#include "net/buffer.h"

// 增量解析一个 HTTP/1.x 请求。
//
// 状态机：
//   kRequestLine -> kHeaders -> kBody（有 Content-Length 时）-> kComplete
//   任意阶段出错 -> kError；Body 超过上限 -> kTooLarge
//
// 半包约定：
// - 找不到完整的行 / Body 不够 Content-Length 时返回 kNeedMore；
// - 返回 kNeedMore 时**不消费** Buffer，数据留到下一次；
// - 返回 kComplete 后调用 Reset()，再解析流水线里的下一个请求。
//
// 解析器是有状态的：同一个连接必须复用同一个实例，不能多个 worker 并发调用。
class HttpParser {
public:
    enum class Result { kNeedMore, kComplete, kError, kTooLarge };

    Result Parse(Buffer& buffer, HttpRequest& request);
    void Reset();

private:
    enum class State { kRequestLine, kHeaders, kBody, kComplete, kError };

    State state_ = State::kRequestLine;
    std::size_t content_length_ = 0;  // 当前请求的 Content-Length
    std::size_t header_bytes_ = 0;    // 已解析的 Headers 累计字节数，用于限额
};
