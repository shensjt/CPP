#pragma once

#include <string>

// ZSet 的排序键：(score, member)。
//
// 为什么不是一个 double：
// - score 可以重复（多个成员同分），跳表需要唯一 key；
// - 带上 member 后，同分再按 member 字典序，顺序稳定、可复现；
// - 和 Redis zskiplist 的排序规则一致：先比 score，再比 member。
struct ScoreMember {
    double score = 0.0;
    std::string member;
};

struct ScoreMemberLess {
    bool operator()(const ScoreMember& a, const ScoreMember& b) const {
        if (a.score < b.score) return true;
        if (b.score < a.score) return false;
        return a.member < b.member;
    }
};

inline bool operator==(const ScoreMember& a, const ScoreMember& b) {
    return a.score == b.score && a.member == b.member;
}
