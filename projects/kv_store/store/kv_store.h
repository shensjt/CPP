#pragma once

#include <cstddef>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "store/zset.h"

class Aof;

// KV 存储：string 和 zset 共用一个 key 空间，类型冲突返回 kWrongType。
//
// 两级 key：
// - 外层：data_ 的 key（用户 key，如 "name"、"z"）；
// - 内层：zset 的 member（如 "alice"），以及跳表的排序 key (score, member)。
//
// 并发：
// - 读（Get/ZScore/ZRange）用 shared_lock，可并行；
// - 写（Set/Del/ZAdd/ZRem）用 unique_lock，串行；
// - 写锁内同时完成“改内存 + Aof::Append”，保证落盘顺序 = 生效顺序。
//
// 状态：
// - kNotFound：key/member 不存在（正常情况）；
// - kWrongType：key 存在但类型不对（客户端错误）。
class KvStore {
public:
    enum class Status { kOk, kNotFound, kWrongType };

    // 写入/覆盖字符串；key 已是 zset 时返回 kWrongType。
    Status Set(const std::string& key, std::string value);
    // 读取字符串；缺失返回 kNotFound，类型不对返回 kWrongType。
    Status Get(const std::string& key, std::string* value) const;
    // 删除任意类型的 key；返回是否真的删除了。
    bool Del(const std::string& key);
    bool Exists(const std::string& key) const;

    // 批量 ZADD；返回新增 member 的数量（更新 score 不计入）。
    // 只要有一项新增或更新，就会在写锁内追加一条 AOF。
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

    // 传 nullptr 表示不持久化（单测、AOF replay 期间）。
    // 必须在服务启动/回放结束后再挂 Aof，否则回放的写命令会再次追加。
    void SetAof(Aof* aof) { aof_ = aof; }

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
    Aof* aof_ = nullptr;
    // 读多写少：GET/ZRANGE 用 shared_lock，SET/ZADD 用 unique_lock。
    mutable std::shared_mutex mutex_;
};
