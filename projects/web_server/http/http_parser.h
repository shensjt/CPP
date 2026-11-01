#pragma once

#include <cstddef>

#include "http/http_request.h"
#include "net/buffer.h"

// 增量解析一个 HTTP/1.x 请求。
// 返回 kNeedMore 时保留未消费数据；返回 kComplete 后调用 Reset() 再解析下一个。
class HttpParser {
public:
    enum class Result { kNeedMore, kComplete, kError };

    Result Parse(Buffer& buffer, HttpRequest& request);
    void Reset();

private:
    enum class State { kRequestLine, kHeaders, kBody, kComplete, kError };

    State state_ = State::kRequestLine;
    std::size_t content_length_ = 0;
    std::size_t header_bytes_ = 0;
};
