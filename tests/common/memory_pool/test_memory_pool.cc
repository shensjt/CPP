// MemoryPool 单元/压力测试（M 系列）
//
// 契约来自 内存池-02-设计思路与理论自测.md：
//   - 10 个桶：8B–4096B，按 2 的幂
//   - Deallocate(p, n) 必须传分配时的 n（或同一大小类）
//   - 超过 4096B 走 malloc/free
//   - 每桶一把锁；chunk 只增不减，析构统一 free

#include "memory_pool/memory_pool.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <set>
#include <thread>
#include <utility>
#include <vector>

#include "test_framework.h"

namespace {

// 写入 tag 并逐字节校验；避免在循环里反复调用 CHECK。
bool FillAndCheck(void* p, std::size_t n, unsigned char tag) {
    if (p == nullptr) return false;
    auto* bytes = static_cast<unsigned char*>(p);
    std::memset(bytes, tag, n);
    for (std::size_t i = 0; i < n; ++i) {
        if (bytes[i] != tag) return false;
    }
    return true;
}

constexpr std::size_t kMaxBlock = 4096;

}  // namespace

// M1：64B 分配 1000 次再全部释放（Day 3 验收）。
TEST(MemoryPool_AllocateDeallocate64) {
    MemoryPool pool;
    std::vector<void*> ptrs;
    ptrs.reserve(1000);

    for (int i = 0; i < 1000; ++i) {
        void* p = pool.Allocate(64);
        CHECK(FillAndCheck(p, 64, static_cast<unsigned char>(i)));
        ptrs.push_back(p);
    }
    for (void* p : ptrs) pool.Deallocate(p, 64);
}

// M2：边界尺寸：0、1、7、8、9、15、16、17、4095、4096、4097、1MB。
TEST(MemoryPool_BoundarySizes) {
    MemoryPool pool;
    const std::size_t sizes[] = {0, 1, 7, 8, 9, 15, 16, 17, 63, 64, 65,
                                 4095, 4096, 4097, 1u << 20};

    for (std::size_t n : sizes) {
        const std::size_t writable = n == 0 ? 1 : n;
        void* p = pool.Allocate(n);
        CHECK(FillAndCheck(p, writable, 0x5a));
        pool.Deallocate(p, n);
    }
}

// M3：10 个桶各拿 200 个块，地址不重复，内容可写可读。
TEST(MemoryPool_AllBucketsUniqueBlocks) {
    MemoryPool pool;

    for (std::size_t n = 8; n <= kMaxBlock; n <<= 1) {
        std::set<void*> seen;
        std::vector<void*> ptrs;
        ptrs.reserve(200);

        for (int i = 0; i < 200; ++i) {
            void* p = pool.Allocate(n);
            CHECK(p != nullptr);
            CHECK(seen.insert(p).second);  // 同一块不能发两次
            CHECK(FillAndCheck(p, n, static_cast<unsigned char>(i)));
            ptrs.push_back(p);
        }
        for (void* p : ptrs) pool.Deallocate(p, n);
    }
}

// M4：释放后的块会被复用（不要求具体顺序，只要求来自刚释放的集合）。
TEST(MemoryPool_FreedBlocksReused) {
    MemoryPool pool;
    std::set<void*> freed;
    std::vector<void*> first;

    for (int i = 0; i < 100; ++i) first.push_back(pool.Allocate(128));
    for (void* p : first) {
        freed.insert(p);
        pool.Deallocate(p, 128);
    }

    std::vector<void*> second;
    for (int i = 0; i < 100; ++i) {
        void* p = pool.Allocate(128);
        CHECK(p != nullptr);
        CHECK(freed.count(p) == 1);
        CHECK(FillAndCheck(p, 128, 0x3c));
        second.push_back(p);
    }
    for (void* p : second) pool.Deallocate(p, 128);
}

// M5：超过 4096B 走系统 malloc/free，内容正确。
TEST(MemoryPool_LargeAllocationFallback) {
    MemoryPool pool;
    const std::size_t sizes[] = {4097, 8192, 64 * 1024, 1u << 20};

    for (std::size_t n : sizes) {
        void* p = pool.Allocate(n);
        CHECK(FillAndCheck(p, n, 0xa5));
        pool.Deallocate(p, n);
    }
}

// M6：对齐。所有块至少 8 字节对齐；16 字节及以上的块按 16 字节对齐。
TEST(MemoryPool_Alignment) {
    MemoryPool pool;

    for (std::size_t n = 8; n <= kMaxBlock; n <<= 1) {
        std::vector<void*> ptrs;
        for (int i = 0; i < 50; ++i) {
            void* p = pool.Allocate(n);
            CHECK(p != nullptr);
            const auto addr = reinterpret_cast<std::uintptr_t>(p);
            CHECK_EQ(addr % 8, std::uintptr_t{0});
            if (n >= 16) CHECK_EQ(addr % 16, std::uintptr_t{0});
            ptrs.push_back(p);
        }
        for (void* p : ptrs) pool.Deallocate(p, n);
    }
}

// M7：一个 4MB chunk 放不下的请求会触发第二次 refill。
// 4096B 块每个 chunk 1024 个，拿 1100 个必然跨 chunk。
TEST(MemoryPool_RefillAcrossChunks) {
    MemoryPool pool;
    std::set<void*> seen;
    std::vector<void*> ptrs;
    ptrs.reserve(1100);

    for (int i = 0; i < 1100; ++i) {
        void* p = pool.Allocate(4096);
        CHECK(p != nullptr);
        CHECK(seen.insert(p).second);
        // 只校验首尾，避免 1100 × 4096 次逐字节检查拖慢测试。
        CHECK(FillAndCheck(p, 1, 0x11));
        static_cast<unsigned char*>(p)[4095] = 0x22;
        CHECK_EQ(static_cast<unsigned char*>(p)[4095], 0x22);
        ptrs.push_back(p);
    }
    for (void* p : ptrs) pool.Deallocate(p, 4096);
}

// M8：0 字节按 1 字节处理；1 字节块也能用。
TEST(MemoryPool_ZeroAndTinySizes) {
    MemoryPool pool;

    void* p0 = pool.Allocate(0);
    CHECK(FillAndCheck(p0, 1, 0xaa));
    pool.Deallocate(p0, 0);

    void* p1 = pool.Allocate(1);
    CHECK(FillAndCheck(p1, 1, 0xbb));
    pool.Deallocate(p1, 1);
}

// M9：释放后重新分配，内容不会被旧数据干扰（写入即覆盖）。
TEST(MemoryPool_ReuseAfterFree) {
    MemoryPool pool;

    void* p = pool.Allocate(64);
    CHECK(FillAndCheck(p, 64, 0x01));
    pool.Deallocate(p, 64);

    void* q = pool.Allocate(64);
    CHECK(FillAndCheck(q, 64, 0x02));
    pool.Deallocate(q, 64);
}

// M10：4 线程并发混合大小分配/释放，写读校验。
TEST(MemoryPool_ConcurrentMixedSizes) {
    MemoryPool pool;
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;

    constexpr int kThreads = 4;
    constexpr int kIterations = 10000;

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&pool, &failures, t] {
            for (int i = 0; i < kIterations; ++i) {
                const std::size_t n =
                    static_cast<std::size_t>((i * 37 + t * 101) % 4096) + 1;
                void* p = pool.Allocate(n);
                if (!FillAndCheck(p, n, static_cast<unsigned char>(t + 1))) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
                pool.Deallocate(p, n);
            }
        });
    }
    for (auto& th : threads) th.join();

    CHECK_EQ(failures.load(), 0);
}

// M11：10 万次分配/释放（Day 6 验收口径），循环复用，不一次性持有。
TEST(MemoryPool_HundredThousandOps) {
    MemoryPool pool;
    std::size_t total = 0;

    for (int i = 0; i < 100000; ++i) {
        const std::size_t n =
            (i % 10 == 0) ? 4096 : static_cast<std::size_t>((i * 13) % 512) + 1;
        void* p = pool.Allocate(n);
        CHECK(p != nullptr);
        if (p != nullptr) {
            static_cast<unsigned char*>(p)[0] = 0x7e;
            static_cast<unsigned char*>(p)[n - 1] = 0x7e;
            CHECK_EQ(static_cast<unsigned char*>(p)[0], 0x7e);
            CHECK_EQ(static_cast<unsigned char*>(p)[n - 1], 0x7e);
        }
        pool.Deallocate(p, n);
        ++total;
    }

    CHECK_EQ(total, std::size_t{100000});
}

// M12：反复创建/销毁池，检查析构是否释放所有 chunk（配合 ASan/LSan）。
TEST(MemoryPool_DestructorReleasesChunks) {
    for (int round = 0; round < 5; ++round) {
        MemoryPool pool;
        std::vector<std::pair<void*, std::size_t>> blocks;

        for (int i = 0; i < 100; ++i) {
            const std::size_t n = static_cast<std::size_t>(8 << (i % 10));
            void* p = pool.Allocate(n);
            CHECK(FillAndCheck(p, n, static_cast<unsigned char>(round)));
            blocks.emplace_back(p, n);
        }
        for (auto& [p, n] : blocks) pool.Deallocate(p, n);
    }
}
