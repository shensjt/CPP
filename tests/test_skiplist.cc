// 跳表测试（SL 系列）

#include "skiplist/skiplist.h"
#include "store/score_member.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "test_framework.h"

// SL1：插入后能查到，Size 正确。
TEST(SkipList_InsertFind) {
    SkipList<int> list;
    CHECK(list.Insert(3));
    CHECK(list.Insert(1));
    CHECK(list.Insert(2));
    CHECK_EQ(list.Size(), std::size_t{3});

    CHECK(list.Contains(1));
    CHECK(list.Contains(3));
    CHECK(!list.Contains(4));

    const int* found = list.Find(2);
    CHECK(found != nullptr);
    if (found != nullptr) {
        CHECK_EQ(*found, 2);
    }
}

// SL2：重复 key 插入失败，不改变大小。
TEST(SkipList_DuplicateInsert) {
    SkipList<int> list;
    CHECK(list.Insert(1));
    CHECK(!list.Insert(1));
    CHECK_EQ(list.Size(), std::size_t{1});
}

// SL3：删除头、尾、中间；重复删除返回 false。
TEST(SkipList_Erase) {
    SkipList<int> list;
    for (int i = 0; i < 10; ++i) {
        CHECK(list.Insert(i));
    }

    CHECK(list.Erase(0));
    CHECK(list.Erase(9));
    CHECK(list.Erase(5));
    CHECK(!list.Erase(5));
    CHECK_EQ(list.Size(), std::size_t{7});

    CHECK(!list.Contains(0));
    CHECK(!list.Contains(5));
    CHECK(!list.Contains(9));

    std::vector<int> remaining;
    list.ForEach([&](const int& value) {
        remaining.push_back(value);
        return true;
    });
    CHECK(remaining == std::vector<int>({1, 2, 3, 4, 6, 7, 8}));
}

// SL4：空表和单元素边界。
TEST(SkipList_EmptyAndSingle) {
    SkipList<int> list;
    CHECK(list.Empty());
    CHECK(!list.Contains(1));
    CHECK(!list.Erase(1));

    CHECK(list.Insert(42));
    CHECK(!list.Empty());
    CHECK(list.Erase(42));
    CHECK(list.Empty());
    CHECK_EQ(list.Level(), 1);  // 删空后层高收回
}

// SL5：乱序插入，遍历结果必须有序。
TEST(SkipList_ForEachOrdered) {
    SkipList<int> list;
    const std::vector<int> input = {5, 1, 9, 3, 7, 2, 8, 0, 6, 4};
    for (int value : input) {
        CHECK(list.Insert(value));
    }

    std::vector<int> output;
    list.ForEach([&](const int& value) {
        output.push_back(value);
        return true;
    });

    std::vector<int> expected = input;
    std::sort(expected.begin(), expected.end());
    CHECK(output == expected);
}

// SL6：Range 左闭右开，越界和反向区间返回空。
TEST(SkipList_RangeBounds) {
    SkipList<int> list;
    for (int i = 0; i < 10; ++i) {
        list.Insert(i);
    }

    std::vector<int> output;
    list.Range(3, 7, [&](const int& value) {
        output.push_back(value);
        return true;
    });
    CHECK(output == std::vector<int>({3, 4, 5, 6}));

    output.clear();
    list.Range(20, 30, [&](const int& value) {
        output.push_back(value);
        return true;
    });
    CHECK(output.empty());

    output.clear();
    list.Range(7, 3, [&](const int& value) {
        output.push_back(value);
        return true;
    });
    CHECK(output.empty());
}

// SL7：fn 返回 false 时提前结束。
TEST(SkipList_RangeCanStop) {
    SkipList<int> list;
    for (int i = 0; i < 10; ++i) {
        list.Insert(i);
    }

    std::vector<int> output;
    list.Range(0, 10, [&](const int& value) {
        output.push_back(value);
        return value < 3;  // 收到 3 后停止
    });
    CHECK(output == std::vector<int>({0, 1, 2, 3}));
}

// SL8：一万个整数，插入、查找、隔一个删除。
TEST(SkipList_LargeInsert) {
    SkipList<int> list(std::less<int>(), 12345);
    constexpr int kN = 10000;

    for (int i = 0; i < kN; ++i) {
        CHECK(list.Insert(i));
    }
    CHECK_EQ(list.Size(), std::size_t{kN});
    CHECK(list.Level() > 1);
    CHECK(list.Level() <= SkipList<int>::kMaxLevel);

    for (int i = 0; i < kN; ++i) {
        CHECK(list.Contains(i));
    }

    for (int i = 0; i < kN; i += 2) {
        CHECK(list.Erase(i));
    }
    CHECK_EQ(list.Size(), std::size_t{kN / 2});
    for (int i = 0; i < kN; ++i) {
        CHECK(list.Contains(i) == (i % 2 == 1));
    }
}

// SL9：string key，验证模板对非算术类型也能用。
TEST(SkipList_StringKeys) {
    SkipList<std::string> list;
    CHECK(list.Insert("banana"));
    CHECK(list.Insert("apple"));
    CHECK(list.Insert("cherry"));
    CHECK(list.Insert("apple") == false);

    std::vector<std::string> output;
    list.ForEach([&](const std::string& value) {
        output.push_back(value);
        return true;
    });
    CHECK(output == std::vector<std::string>({"apple", "banana", "cherry"}));
}

// SL10：自定义比较器（降序）。
TEST(SkipList_CustomComparator) {
    SkipList<int, std::greater<int>> list{std::greater<int>(), 7};
    for (int i = 0; i < 5; ++i) {
        CHECK(list.Insert(i));
    }

    std::vector<int> output;
    list.ForEach([&](const int& value) {
        output.push_back(value);
        return true;
    });
    CHECK(output == std::vector<int>({4, 3, 2, 1, 0}));

    CHECK(list.Contains(3));
    CHECK(!list.Contains(5));
}

// SL11：(score, member) 排序，同 score 按 member 升序。
TEST(SkipList_ScoreMemberOrdering) {
    SkipList<ScoreMember, ScoreMemberLess> list;
    CHECK(list.Insert({2.0, "b"}));
    CHECK(list.Insert({1.0, "z"}));
    CHECK(list.Insert({1.0, "a"}));
    CHECK(list.Insert({2.0, "a"}));

    std::vector<std::string> members;
    list.ForEach([&](const ScoreMember& entry) {
        members.push_back(entry.member);
        return true;
    });
    CHECK(members == std::vector<std::string>({"a", "z", "a", "b"}));
}

// SL12：按 score 范围取 [2, 3]，用 RangeFrom + 回调控制上界。
TEST(SkipList_ScoreRange) {
    SkipList<ScoreMember, ScoreMemberLess> list;
    list.Insert({1.0, "a"});
    list.Insert({2.0, "b"});
    list.Insert({2.0, "c"});
    list.Insert({3.0, "d"});
    list.Insert({4.0, "e"});

    std::vector<std::string> members;
    list.RangeFrom(ScoreMember{2.0, ""}, [&](const ScoreMember& entry) {
        if (entry.score > 3.0) {
            return false;
        }
        members.push_back(entry.member);
        return true;
    });
    CHECK(members == std::vector<std::string>({"b", "c", "d"}));
}

// SL13：随机操作和 std::set 差分对比，覆盖插入/删除/查找的组合。
TEST(SkipList_RandomAgainstStdSet) {
    SkipList<int> list(std::less<int>(), 20261104);
    std::set<int> reference;
    std::mt19937 rng(20261104);
    std::uniform_int_distribution<int> op(0, 2);
    std::uniform_int_distribution<int> key(0, 500);

    for (int i = 0; i < 20000; ++i) {
        const int k = key(rng);
        switch (op(rng)) {
            case 0: {
                const bool actual = list.Insert(k);
                const bool expected = reference.insert(k).second;
                CHECK(actual == expected);
                break;
            }
            case 1: {
                const bool actual = list.Erase(k);
                const bool expected = reference.erase(k) > 0;
                CHECK(actual == expected);
                break;
            }
            default:
                CHECK(list.Contains(k) == (reference.count(k) > 0));
                break;
        }
        if (i % 100 == 0) {
            CHECK_EQ(list.Size(), reference.size());
        }
    }

    std::vector<int> actual;
    list.ForEach([&](const int& value) {
        actual.push_back(value);
        return true;
    });
    CHECK(actual == std::vector<int>(reference.begin(), reference.end()));
}
