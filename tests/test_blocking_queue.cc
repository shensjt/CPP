// BlockingQueue 单元测试（对应 线程池-04-测试用例设计.md 的 B 系列）
//
// 约定：
//   - 所有“等待”都有超时；测试挂死由外层 timeout 兜底
//   - 断言尽量只在主线程做；worker 线程只写结果
//   - 不要用 vector<bool>（位压缩，并发写不同下标也是数据竞争）

#include "thread_pool/thread_pool.h"

#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "test_framework.h"

using namespace std::chrono_literals;

// B1：Push 后 Pop 取值，且满足 FIFO。
TEST(BlockingQueue_PushPopFifo) {
    BlockingQueue<int> q;
    CHECK(q.Push(1));
    CHECK(q.Push(2));
    CHECK(q.Push(3));

    int value = 0;
    CHECK(q.Pop(value));
    CHECK_EQ(value, 1);
    CHECK(q.Pop(value));
    CHECK_EQ(value, 2);
    CHECK(q.Pop(value));
    CHECK_EQ(value, 3);

    CHECK(!q.TryPop(value));
}

// B2：空队列时 Pop 阻塞，Push 之后被唤醒。
TEST(BlockingQueue_EmptyPopBlocksThenWakes) {
    BlockingQueue<int> q;

    std::promise<void> started;
    auto started_future = started.get_future();

    std::promise<std::pair<bool, int>> result;
    auto result_future = result.get_future();

    std::thread consumer([&] {
        started.set_value();
        int value = 0;
        const bool ok = q.Pop(value);
        result.set_value({ok, value});
    });

    CHECK(started_future.wait_for(1s) == std::future_status::ready);

    // 有界窗口内 Pop 不应返回；如果立刻返回，说明它没有阻塞。
    CHECK(result_future.wait_for(50ms) == std::future_status::timeout);

    CHECK(q.Push(7));

    const auto status = result_future.wait_for(1s);
    CHECK(status == std::future_status::ready);
    if (status == std::future_status::ready) {
        const auto [ok, value] = result_future.get();
        CHECK(ok);
        CHECK_EQ(value, 7);
    }

    consumer.join();
}

// B3：空队列 TryPop 立即返回 false，且不修改 value。
TEST(BlockingQueue_EmptyTryPopReturnsFalse) {
    BlockingQueue<int> q;

    int value = 42;
    CHECK(!q.TryPop(value));
    CHECK_EQ(value, 42);
}

// B4：Close 不丢弃已入队元素，Pop 先取完再返回 false。
TEST(BlockingQueue_CloseDrainsPending) {
    BlockingQueue<int> q;
    for (int i = 0; i < 100; ++i) {
        CHECK(q.Push(i));
    }

    q.Close();

    int value = -1;
    int count = 0;
    while (q.Pop(value)) {
        CHECK_EQ(value, count);
        ++count;
    }

    CHECK_EQ(count, 100);
    CHECK(!q.Pop(value));
}

// B5：Close 后空队列 Pop 立即返回 false。
TEST(BlockingQueue_CloseEmptyPopReturnsFalse) {
    BlockingQueue<int> q;
    q.Close();

    int value = 42;
    CHECK(!q.Pop(value));
    CHECK_EQ(value, 42);
}

// B6：Close 后 Push 被拒绝，元素没有入队。
TEST(BlockingQueue_PushAfterCloseRejected) {
    BlockingQueue<int> q;
    q.Close();

    CHECK(!q.Push(1));

    int value = 0;
    CHECK(!q.Pop(value));
    CHECK(!q.TryPop(value));
}

// B7：Close 唤醒所有阻塞的消费者（若 notify_one 会有人睡死）。
TEST(BlockingQueue_CloseWakesAllWaiters) {
    constexpr int kConsumers = 4;
    BlockingQueue<int> q;

    std::vector<std::promise<void>> started(kConsumers);
    std::vector<std::future<void>> started_futures;
    std::vector<std::promise<bool>> results(kConsumers);
    std::vector<std::future<bool>> result_futures;
    std::vector<std::thread> consumers;

    for (int i = 0; i < kConsumers; ++i) {
        started_futures.push_back(started[i].get_future());
        result_futures.push_back(results[i].get_future());
        consumers.emplace_back([&, i] {
            started[i].set_value();
            int value = 0;
            results[i].set_value(q.Pop(value));
        });
    }

    for (auto& future : started_futures) {
        CHECK(future.wait_for(1s) == std::future_status::ready);
    }

    q.Close();

    bool all_ready = true;
    for (auto& future : result_futures) {
        if (future.wait_for(1s) != std::future_status::ready) {
            all_ready = false;
        }
    }
    CHECK(all_ready);

    if (all_ready) {
        for (auto& future : result_futures) {
            CHECK(!future.get());
        }
    }

    for (auto& thread : consumers) {
        thread.join();
    }
}

// B8：多生产者/多消费者，不丢不重。
TEST(BlockingQueue_MultiProducerConsumerNoLossNoDup) {
    constexpr int kProducers = 4;
    constexpr int kConsumers = 4;
    constexpr int kPerProducer = 2500;
    constexpr int kTotal = kProducers * kPerProducer;

    BlockingQueue<int> q;

    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&q, p] {
            for (int i = 0; i < kPerProducer; ++i) {
                q.Push(p * kPerProducer + i);
            }
        });
    }

    std::vector<std::vector<int>> consumed(kConsumers);
    std::vector<std::thread> consumers;
    for (int c = 0; c < kConsumers; ++c) {
        consumers.emplace_back([&q, &consumed, c] {
            int value = 0;
            while (q.Pop(value)) {
                consumed[c].push_back(value);
            }
        });
    }

    for (auto& thread : producers) {
        thread.join();
    }
    q.Close();
    for (auto& thread : consumers) {
        thread.join();
    }

    std::vector<int> hits(kTotal, 0);
    for (auto& values : consumed) {
        for (int value : values) {
            CHECK(value >= 0 && value < kTotal);
            if (value >= 0 && value < kTotal) {
                ++hits[value];
            }
        }
    }

    int total = 0;
    for (int hit : hits) {
        CHECK_EQ(hit, 1);
        total += hit;
    }
    CHECK_EQ(total, kTotal);
}

// B9：Close 幂等。
TEST(BlockingQueue_CloseIsIdempotent) {
    BlockingQueue<int> q;
    q.Close();
    q.Close();
    q.Close();

    int value = 0;
    CHECK(!q.Pop(value));
}

// B10：返回 false 时不修改调用方传入的 value。
TEST(BlockingQueue_FailedPopLeavesValueUntouched) {
    BlockingQueue<int> q;

    int value = 42;
    CHECK(!q.TryPop(value));
    CHECK_EQ(value, 42);

    q.Close();
    CHECK(!q.Pop(value));
    CHECK_EQ(value, 42);
    CHECK(!q.TryPop(value));
    CHECK_EQ(value, 42);
}

// B11：Close 后 TryPop 仍能取出剩余元素。
TEST(BlockingQueue_TryPopDrainsAfterClose) {
    BlockingQueue<int> q;
    CHECK(q.Push(1));
    CHECK(q.Push(2));
    CHECK(q.Push(3));
    q.Close();

    int value = 0;
    CHECK(q.TryPop(value));
    CHECK_EQ(value, 1);
    CHECK(q.TryPop(value));
    CHECK_EQ(value, 2);
    CHECK(q.TryPop(value));
    CHECK_EQ(value, 3);
    CHECK(!q.TryPop(value));
}

// B12：提交与 Close 并发，成功入队的元素都能被取出。
TEST(BlockingQueue_ConcurrentPushAndClose) {
    BlockingQueue<int> q;
    std::vector<int> accepted;

    std::thread producer([&] {
        for (int i = 0; q.Push(i); ++i) {
            accepted.push_back(i);
        }
    });

    // 只用于制造重叠；测试的正确性不依赖这个 sleep。
    std::this_thread::sleep_for(5ms);
    q.Close();
    producer.join();

    std::vector<int> drained;
    int value = 0;
    while (q.Pop(value)) {
        drained.push_back(value);
    }

    CHECK_EQ(accepted.size(), drained.size());
    CHECK(accepted == drained);
}

// B13：非平凡类型（std::string）值保持正确。
TEST(BlockingQueue_StringValuesPreserved) {
    BlockingQueue<std::string> q;
    CHECK(q.Push(std::string("")));
    CHECK(q.Push(std::string("short")));
    CHECK(q.Push(std::string(1000, 'x')));

    std::string value;
    CHECK(q.Pop(value));
    CHECK_EQ(value, std::string(""));
    CHECK(q.Pop(value));
    CHECK_EQ(value, std::string("short"));
    CHECK(q.Pop(value));
    CHECK_EQ(value, std::string(1000, 'x'));
}
