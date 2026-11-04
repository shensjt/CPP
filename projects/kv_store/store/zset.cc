#include "store/zset.h"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

bool ZSet::Add(double score, const std::string& member) {
    const auto it = scores_.find(member);
    if (it != scores_.end()) {
        if (it->second == score) {
            return false;
        }
        ordered_.Erase(ScoreMember{it->second, member});
        it->second = score;
        ordered_.Insert(ScoreMember{score, member});
        return false;
    }

    scores_.emplace(member, score);
    ordered_.Insert(ScoreMember{score, member});
    return true;
}

bool ZSet::Remove(const std::string& member) {
    const auto it = scores_.find(member);
    if (it == scores_.end()) {
        return false;
    }
    ordered_.Erase(ScoreMember{it->second, member});
    scores_.erase(it);
    return true;
}

std::optional<double> ZSet::Score(const std::string& member) const {
    const auto it = scores_.find(member);
    if (it == scores_.end()) {
        return std::nullopt;
    }
    return it->second;
}

void ZSet::RangeByRank(long long start, long long stop,
                       std::vector<std::string>& out) const {
    const long long size = static_cast<long long>(scores_.size());
    if (start < 0) start += size;
    if (stop < 0) stop += size;
    if (start < 0) start = 0;
    if (stop >= size) stop = size - 1;
    if (start > stop || start >= size) {
        return;
    }

    long long index = 0;
    ordered_.ForEach([&](const ScoreMember& entry) {
        if (index > stop) {
            return false;
        }
        if (index >= start) {
            out.push_back(entry.member);
        }
        ++index;
        return true;
    });
}

void ZSet::RangeByScore(double min, bool min_exclusive, double max,
                        bool max_exclusive, std::vector<std::string>& out) const {
    if (min > max) {
        return;
    }

    ordered_.RangeFrom(ScoreMember{min, ""}, [&](const ScoreMember& entry) {
        if (entry.score > max) {
            return false;
        }
        if (entry.score == max && max_exclusive) {
            return false;
        }
        if (entry.score == min && min_exclusive) {
            return true;
        }
        out.push_back(entry.member);
        return true;
    });
}
