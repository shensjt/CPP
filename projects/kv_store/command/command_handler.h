#pragma once

#include <string>
#include <vector>

#include "store/kv_store.h"

// 命令层：把 RESP 参数翻译成 KvStore 调用，再编码成 RESP 响应。
// 不碰 socket，方便单测；网络接入在 M3。
class CommandHandler {
public:
    explicit CommandHandler(KvStore& store) : store_(store) {}

    std::string Execute(const std::vector<std::string>& args);

private:
    KvStore& store_;
};
