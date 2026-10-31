// ThreadPool 单元/集成测试（对应 线程池-04-测试用例设计.md 的 T 系列）
//
// 关键技巧：
//   - 用“作用域结束时析构”作为“所有任务完成”的屏障；
//   - 每个任务只写自己的槽位，避免测试自身制造数据竞争；
//   - 并发度测试用条件变量做屏障，并带超时，避免挂死。

#include "thread_pool/thread_pool.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

#include "test_framework.h"

using namespace std::chrono_literals;

// T1：1000 个任务全部执行，且每个恰好一次。
TEST(ThreadPool_AllTasksRunExactlyOnce) {
    constexpr int kTasks = 1000;
    std::vector<int> hits(kTasks, 0);

    {
        ThreadPool pool(4);
        for (int i = 0; i < kTasks; ++i) {
            CHECK(pool.Post([&hits, i] { ++hits[i]; }));
        }
    }  // 析构：Close + join，所有任务完成

    int total = 0;
    for (int i = 0; i < kTasks; ++i) {
        CHECK_EQ(hits[i], 1);
        total += hits[i];
    }
    CHECK_EQ(total, kTasks);
}

// T2：带参数任务的参数绑定正确。
TEST(ThreadPool_PostWithArgs) {
    constexpr int kTasks = 100;
    std::vector<int> results(kTasks, 0);

    {
        ThreadPool pool(2);
        for (int i = 0; i < kTasks; ++i) {
            CHECK(pool.Post(
                [&results](int a, int b, int index) {
                    results[index] = a + b;
                },
                i, i * 2, i));
        }
    }

    for (int i = 0; i < kTasks; ++i) {
        CHECK_EQ(results[i], i + i * 2);
    }
}

// T3：线程数不超过构造参数，且线程被复用。
TEST(ThreadPool_UsesAtMostNThreads) {
    constexpr int kWorkers = 4;
    constexpr int kTasks = 2000;

    std::vector<std::thread::id> ids(kTasks);

    {
        ThreadPool pool(kWorkers);
        for (int i = 0; i < kTasks; ++i) {
            CHECK(pool.Post([&ids, i] { ids[i] = std::this_thread::get_id(); }));
        }
    }

    for (const auto& id : ids) {
        CHECK(id != std::thread::id());
    }

    const std::set<std::thread::id> unique_ids(ids.begin(), ids.end());
    CHECK(unique_ids.size() <= static_cast<std::size_t>(kWorkers));
    CHECK(unique_ids.size() >= 1);
}

// T4：最多 N 个任务真正并发执行。
// 用条件变量做屏障：N 个任务必须同时到达，否则超时并判失败。
TEST(ThreadPool_RunsNTasksConcurrently) {
    constexpr int kWorkers = 4;

    std::mutex mutex;
    std::condition_variable cv;
    int arrived = 0;
    int max_active = 0;
    bool timed_out = false;
    std::atomic<int> active{0};

    {
        ThreadPool pool(kWorkers);
        for (int i = 0; i < kWorkers; ++i) {
            CHECK(pool.Post([&] {
                const int now = active.fetch_add(1) + 1;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    if (now > max_active) {
                        max_active = now;
                    }
                    ++arrived;
                    cv.notify_all();
                    if (!cv.wait_for(lock, std::chrono::seconds(2),
                                     [&] { return arrived >= kWorkers; })) {
                        timed_out = true;
                    }
                }
                active.fetch_sub(1);
            }));
        }
    }

    CHECK(!timed_out);
    CHECK_EQ(max_active, kWorkers);
}

// T5：析构等待正在执行的任务完成。
TEST(ThreadPool_DestructorWaitsForRunningTask) {
    std::atomic<bool> done{false};
    const auto start = std::chrono::steady_clock::now();

    {
        ThreadPool pool(1);
        CHECK(pool.Post([&done] {
            std::this_thread::sleep_for(100ms);
            done.store(true, std::memory_order_release);
        }));
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    CHECK(done.load(std::memory_order_acquire));
    CHECK(elapsed.count() >= 90);  // 宽松阈值，验证确实等待了
}

// T6：任务异常不会杀死 worker，后续任务继续执行。
TEST(ThreadPool_TaskExceptionDoesNotKillWorker) {
    std::atomic<bool> done{false};

    {
        ThreadPool pool(1);  // 单 worker：若异常杀死 worker，后续任务不会执行
        CHECK(pool.Post([] { throw std::runtime_error("boom"); }));
        CHECK(pool.Post([&done] { done.store(true); }));
    }

    CHECK(done.load());
}

// T8：任务内部可以再提交任务（前提是池尚未进入关闭流程）。
//
// 注意：本测试必须等外层任务真正提交完内层任务后，再离开作用域。
// 否则析构会先把队列 Close，外层任务里的 Post 会被拒绝——这是停机契约，
// 不是 bug。
TEST(ThreadPool_TaskCanPostAnotherTask) {
    std::atomic<bool> inner_ran{false};
    std::atomic<bool> inner_post_ok{false};

    std::promise<void> inner_posted;
    auto inner_posted_future = inner_posted.get_future();

    {
        ThreadPool pool(2);
        CHECK(pool.Post([&] {
            inner_post_ok.store(
                pool.Post([&inner_ran] { inner_ran.store(true); }));
            inner_posted.set_value();
        }));

        // 等外层任务确认“已经提交了内层任务”，再让析构关闭队列。
        CHECK(inner_posted_future.wait_for(1s) == std::future_status::ready);
    }
    // 析构：Close + join；已入队的内层任务会被执行完。

    CHECK(inner_post_ok.load());
    CHECK(inner_ran.load());
}

// T9：10 万任务压力测试（配合 TSan 构建运行）。
TEST(ThreadPool_Stress100kTasks) {
    constexpr int kTasks = 100000;

    std::atomic<int> executed{0};
    std::atomic<int> post_failures{0};

    {
        ThreadPool pool(4);
        for (int i = 0; i < kTasks; ++i) {
            if (!pool.Post([&executed] {
                    executed.fetch_add(1, std::memory_order_relaxed);
                })) {
                post_failures.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    CHECK_EQ(post_failures.load(), 0);
    CHECK_EQ(executed.load(), kTasks);
}
