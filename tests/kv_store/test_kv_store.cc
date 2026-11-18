// KVStore / ZSet 测试（KS 系列）

#include "store/kv_store.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "test_framework.h"

// KS1：SET / GET / 覆盖。
TEST(KvStore_SetGet) {
    KvStore store;
    CHECK(store.Set("name", "alice") == KvStore::Status::kOk);

    std::string value;
    CHECK(store.Get("name", &value) == KvStore::Status::kOk);
    CHECK_EQ(value, std::string("alice"));

    CHECK(store.Set("name", "bob") == KvStore::Status::kOk);
    CHECK(store.Get("name", &value) == KvStore::Status::kOk);
    CHECK_EQ(value, std::string("bob"));

    CHECK(store.Get("missing", &value) == KvStore::Status::kNotFound);
}

// KS2：DEL / EXISTS。
TEST(KvStore_DelExists) {
    KvStore store;
    store.Set("a", "1");
    store.Set("b", "2");

    CHECK(store.Exists("a"));
    CHECK(store.Del("a"));
    CHECK(!store.Del("a"));
    CHECK(!store.Exists("a"));
    CHECK(store.Exists("b"));
    CHECK_EQ(store.Size(), std::size_t{1});
}

// KS3：string 和 zset 共用一个 key 空间，类型冲突返回 kWrongType。
TEST(KvStore_TypeConflict) {
    KvStore store;
    store.Set("k", "value");

    std::size_t added = 0;
    const std::vector<std::pair<double, std::string>> items = {{1.0, "m"}};
    CHECK(store.ZAdd("k", items, &added) == KvStore::Status::kWrongType);

    CHECK(store.Del("k"));
    CHECK(store.ZAdd("k", items, &added) == KvStore::Status::kOk);
    CHECK_EQ(added, std::size_t{1});

    CHECK(store.Set("k", "value") == KvStore::Status::kWrongType);

    std::string value;
    CHECK(store.Get("k", &value) == KvStore::Status::kWrongType);
}

// KS4：ZADD 新增/更新、ZSCORE。
TEST(KvStore_ZAddScore) {
    KvStore store;
    std::size_t added = 0;

    CHECK(store.ZAdd("z", {{1.0, "a"}, {2.0, "b"}}, &added) ==
          KvStore::Status::kOk);
    CHECK_EQ(added, std::size_t{2});

    double score = 0.0;
    CHECK(store.ZScore("z", "a", &score) == KvStore::Status::kOk);
    CHECK_EQ(score, 1.0);

    // 更新已有 member 的 score，不算新增。
    CHECK(store.ZAdd("z", {{3.0, "a"}}, &added) == KvStore::Status::kOk);
    CHECK_EQ(added, std::size_t{0});
    CHECK(store.ZScore("z", "a", &score) == KvStore::Status::kOk);
    CHECK_EQ(score, 3.0);

    CHECK(store.ZScore("z", "missing", &score) == KvStore::Status::kNotFound);
    CHECK(store.ZScore("missing", "a", &score) == KvStore::Status::kNotFound);
}

// KS5：ZREM。
TEST(KvStore_ZRem) {
    KvStore store;
    std::size_t count = 0;
    store.ZAdd("z", {{1.0, "a"}, {2.0, "b"}, {3.0, "c"}}, &count);

    CHECK(store.ZRem("z", {"a", "c", "x"}, &count) == KvStore::Status::kOk);
    CHECK_EQ(count, std::size_t{2});

    double score = 0.0;
    CHECK(store.ZScore("z", "a", &score) == KvStore::Status::kNotFound);
    CHECK(store.ZScore("z", "b", &score) == KvStore::Status::kOk);
    CHECK_EQ(score, 2.0);

    CHECK(store.ZRem("missing", {"a"}, &count) == KvStore::Status::kNotFound);
}

// KS6：ZRANGE 按名次，支持负下标。
TEST(KvStore_ZRangeByRank) {
    KvStore store;
    std::size_t count = 0;
    store.ZAdd("z", {{3.0, "c"}, {1.0, "a"}, {2.0, "b"}}, &count);

    std::vector<std::string> out;
    CHECK(store.ZRangeByRank("z", 0, -1, &out) == KvStore::Status::kOk);
    CHECK(out == std::vector<std::string>({"a", "b", "c"}));

    CHECK(store.ZRangeByRank("z", 1, 2, &out) == KvStore::Status::kOk);
    CHECK(out == std::vector<std::string>({"b", "c"}));

    CHECK(store.ZRangeByRank("z", -2, -1, &out) == KvStore::Status::kOk);
    CHECK(out == std::vector<std::string>({"b", "c"}));

    CHECK(store.ZRangeByRank("z", 5, 10, &out) == KvStore::Status::kOk);
    CHECK(out.empty());
}

// KS7：ZRANGEBYSCORE 闭区间和开区间。
TEST(KvStore_ZRangeByScore) {
    KvStore store;
    std::size_t count = 0;
    store.ZAdd("z", {{1.0, "a"}, {2.0, "b"}, {3.0, "c"}, {4.0, "d"}}, &count);

    std::vector<std::string> out;
    CHECK(store.ZRangeByScore("z", 2.0, false, 3.0, false, &out) ==
          KvStore::Status::kOk);
    CHECK(out == std::vector<std::string>({"b", "c"}));

    CHECK(store.ZRangeByScore("z", 2.0, true, 3.0, false, &out) ==
          KvStore::Status::kOk);
    CHECK(out == std::vector<std::string>({"c"}));

    CHECK(store.ZRangeByScore("z", 2.0, false, 3.0, true, &out) ==
          KvStore::Status::kOk);
    CHECK(out == std::vector<std::string>({"b"}));

    CHECK(store.ZRangeByScore("z", 10.0, false, 20.0, false, &out) ==
          KvStore::Status::kOk);
    CHECK(out.empty());
}

// KS8：同 score 按 member 字典序，保证顺序稳定。
TEST(KvStore_SameScoreTieBreak) {
    KvStore store;
    std::size_t count = 0;
    store.ZAdd("z", {{1.0, "c"}, {1.0, "a"}, {1.0, "b"}}, &count);

    std::vector<std::string> out;
    store.ZRangeByRank("z", 0, -1, &out);
    CHECK(out == std::vector<std::string>({"a", "b", "c"}));
}

// KS9：大量 member 的范围查询。
TEST(KvStore_LargeZSet) {
    KvStore store;
    std::vector<std::pair<double, std::string>> items;
    for (int i = 0; i < 1000; ++i) {
        items.emplace_back(static_cast<double>(i), "m" + std::to_string(i));
    }

    std::size_t added = 0;
    CHECK(store.ZAdd("z", items, &added) == KvStore::Status::kOk);
    CHECK_EQ(added, std::size_t{1000});

    std::vector<std::string> out;
    store.ZRangeByScore("z", 100.0, false, 199.0, false, &out);
    CHECK_EQ(out.size(), std::size_t{100});
    CHECK_EQ(out.front(), std::string("m100"));
    CHECK_EQ(out.back(), std::string("m199"));
}
