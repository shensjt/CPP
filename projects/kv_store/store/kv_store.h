#pragma once

#include <cstddef>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "store/zset.h"

// KV 存储：string 和 zset 共用一个 key 空间，类型冲突返回 kWrongType。
// 本身不加锁；并发版本在 M3 用 shared_mutex 包一层。
class KvStore {
public:
    enum class Status { kOk, kNotFound, kWrongType };

    Status Set(const std::string& key, std::string value);
    Status Get(const std::string& key, std::string* value) const;
    bool Del(const std::string& key);
    bool Exists(const std::string& key) const;

    // 返回新增 member 的数量；更新已有 member 的 score 不计入 added。
    Status ZAdd(const std::string& key,
                const std::vector<std::pair<double, std::string>>& items,
                std::size_t* added);
    Status ZScore(const std::string& key, const std::string& member,
                  double* score) const;
    Status ZRem(const std::string& key,
                const std::vector<std::string>& members,
                std::size_t* removed);
    Status ZRangeByRank(const std::string& key, long long start, long long stop,
                        std::vector<std::string>* out) const;
    Status ZRangeByScore(const std::string& key, double min,
                         bool min_exclusive, double max, bool max_exclusive,
                         std::vector<std::string>* out) const;

    std::size_t Size() const;

private:
    struct Entry {
        enum class Type { kString, kZSet };
        Type type = Type::kString;
        std::string str;
        std::unique_ptr<ZSet> zset;
    };

    // 下面两个函数不加锁，调用方必须已持有 mutex_。
    Entry* FindOrCreateZSet(const std::string& key, Status* status);
    const Entry* Find(const std::string& key) const;

    std::unordered_map<std::string, Entry> data_;
    // 读多写少：GET/ZRANGE 用 shared_lock，SET/ZADD 用 unique_lock。
    mutable std::shared_mutex mutex_;
};
