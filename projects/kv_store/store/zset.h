#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "skiplist/skiplist.h"
#include "store/score_member.h"

// 有序集合：dict(member -> score) + skiplist(score, member)。
// dict 负责 O(1) 查 score 和判重，skiplist 负责按 score 有序和范围查询。
// 与 Redis zset 的分工一致；本身不加锁，由 KvStore 外层保护。
class ZSet {
public:
    // 新增 member 返回 true；更新已有 member 的 score 返回 false。
    bool Add(double score, const std::string& member);
    bool Remove(const std::string& member);
    std::optional<double> Score(const std::string& member) const;

    std::size_t Size() const { return scores_.size(); }
    bool Empty() const { return scores_.empty(); }

    // start / stop 支持负数，-1 表示最后一个。
    void RangeByRank(long long start, long long stop,
                     std::vector<std::string>& out) const;

    // min / max 支持闭区间和开区间（exclusive 由调用方给出）。
    void RangeByScore(double min, bool min_exclusive, double max,
                      bool max_exclusive, std::vector<std::string>& out) const;

private:
    std::unordered_map<std::string, double> scores_;
    SkipList<ScoreMember, ScoreMemberLess> ordered_;
};
