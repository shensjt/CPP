// LRU 缓存测试（LRC 系列）

#include "lru_cache/lru_cache.h"

#include <cstddef>
#include <string>
#include <thread>
#include <vector>

#include "test_framework.h"

// LRC1：Put 后能 Get，未命中返回 false。
TEST(LruCache_PutGet) {
    LruCache<std::string, int> cache(10);
    cache.Put("a", 1, 1);

    int value = 0;
    CHECK(cache.Get("a", value));
    CHECK_EQ(value, 1);
    CHECK(!cache.Get("b", value));
}

// LRC2：按 cost 淘汰最久未使用的项。
TEST(LruCache_EvictsByCost) {
    LruCache<std::string, int> cache(4);
    cache.Put("a", 1, 2);
    cache.Put("b", 2, 2);
    cache.Put("c", 3, 2);  // 总 cost 6，淘汰 a

    int value = 0;
    CHECK(!cache.Get("a", value));
    CHECK(cache.Get("b", value));
    CHECK(cache.Get("c", value));
    CHECK_EQ(cache.UsedBytes(), std::size_t{4});
}

// LRC3：Get 会把项移到链表头，影响淘汰顺序。
TEST(LruCache_GetRefreshes) {
    LruCache<std::string, int> cache(4);
    cache.Put("a", 1, 2);
    cache.Put("b", 2, 2);

    int value = 0;
    CHECK(cache.Get("a", value));
    cache.Put("c", 3, 2);  // b 最久未使用，被淘汰

    CHECK(cache.Get("a", value));
    CHECK(cache.Get("c", value));
    CHECK(!cache.Get("b", value));
}

// LRC4：单项超过容量时不缓存。
TEST(LruCache_ItemLargerThanCapacity) {
    LruCache<std::string, int> cache(4);
    cache.Put("big", 1, 8);

    int value = 0;
    CHECK(!cache.Get("big", value));
    CHECK_EQ(cache.Size(), std::size_t{0});
}

// LRC5：同一个 key 覆盖时更新 value 和 cost。
TEST(LruCache_UpdateExisting) {
    LruCache<std::string, int> cache(10);
    cache.Put("a", 1, 2);
    cache.Put("a", 2, 3);

    int value = 0;
    CHECK(cache.Get("a", value));
    CHECK_EQ(value, 2);
    CHECK_EQ(cache.Size(), std::size_t{1});
    CHECK_EQ(cache.UsedBytes(), std::size_t{3});
}

// LRC6：多线程 Put/Get 不崩，配合 TSan 检查数据竞争。
TEST(LruCache_ThreadSafe) {
    LruCache<std::string, int> cache(1024);
    std::vector<std::thread> threads;

    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&cache, t] {
            for (int i = 0; i < 1000; ++i) {
                const std::string key =
                    "k" + std::to_string(t) + "-" + std::to_string(i % 64);
                cache.Put(key, i, 1);
                int value = 0;
                cache.Get(key, value);
            }
        });
    }
    for (auto& thread : threads) thread.join();

    CHECK(cache.Size() <= std::size_t{256});
}
