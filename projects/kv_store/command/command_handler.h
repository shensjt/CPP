#pragma once

#include <string>
#include <vector>

#include "store/kv_store.h"

// 命令层：把 RESP 参数翻译成 KvStore 调用，再编码成 RESP 响应。
//
// 职责：
// - 校验参数个数和格式（score 浮点、start/stop 整数、min/max 边界）；
// - 调 KvStore 的强类型接口；
// - 把 kOk / kNotFound / kWrongType 翻译成 +OK / null bulk / -WRONGTYPE。
//
// 不碰 socket、不碰 Buffer，方便单测；网络接入在 KvServer。
class CommandHandler {
public:
    explicit CommandHandler(KvStore& store) : store_(store) {}

    std::string Execute(const std::vector<std::string>& args);

private:
    KvStore& store_;
};
