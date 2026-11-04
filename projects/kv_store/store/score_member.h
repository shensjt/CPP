#pragma once

#include <string>

// ZSet 的排序键：(score, member)。
// score 相同时按 member 字典序，保证 key 唯一，范围查询结果稳定。
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
