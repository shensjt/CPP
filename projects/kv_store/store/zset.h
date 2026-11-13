#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "skiplist/skiplist.h"
#include "store/score_member.h"

// 有序集合：dict(member -> score) + skiplist(score, member)。
//
// 两个结构的分工：
// - scores_：member -> score，ZSCORE / ZADD 判重 O(1)；
// - ordered_：按 (score, member) 排序，ZRANGE / ZRANGEBYSCORE 用。
//
// 和 Redis zset 的分工一致：空间换时间，点查和范围查询都快。
// 线程安全：不加锁，由 KvStore 的 shared_mutex 在更外层保护。
class ZSet {
public:
    // 区分三种结果：
    // - kAdded：新 member，ZADD 返回值 +1；
    // - kUpdated：已有 member 改了 score，返回值不变，但要写 AOF；
    // - kUnchanged：score 没变，不写 AOF。
    enum class AddResult { kAdded, kUpdated, kUnchanged };

    AddResult Add(double score, const std::string& member);
    bool Remove(const std::string& member);
    std::optional<double> Score(const std::string& member) const;

    std::size_t Size() const { return scores_.size(); }
    bool Empty() const { return scores_.empty(); }

    // 按名次取 [start, stop]，支持负数（-1 表示最后一个）。
    // 当前沿底层链表遍历，O(N)；没有 Redis 的 span 优化。
    void RangeByRank(long long start, long long stop,
                     std::vector<std::string>& out) const;

    // 按 score 范围取 member；min/max 的开闭由调用方给出。
    // 用 skiplist 的 RangeFrom 定位起点，再按回调控制上界，O(logN + M)。
    void RangeByScore(double min, bool min_exclusive, double max,
                      bool max_exclusive, std::vector<std::string>& out) const;

private:
    std::unordered_map<std::string, double> scores_;
    SkipList<ScoreMember, ScoreMemberLess> ordered_;
};
