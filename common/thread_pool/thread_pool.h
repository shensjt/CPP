#pragma once

#include <pthread.h>

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// 固定容量的任务队列（实际上没有容量上限）。
// 生产者：Push；消费者：Pop / TryPop。
// 关闭语义：Close 之后 Push 返回 false；已入队的任务仍会被 Pop 取完。
template <class T>
class BlockingQueue {
public:
    BlockingQueue() = default;
    ~BlockingQueue() = default;

    BlockingQueue(const BlockingQueue&) = delete;
    BlockingQueue& operator=(const BlockingQueue&) = delete;

    // 入队；队列已关闭时返回 false。
    bool Push(const T& value) {
        {
            std::lock_guard<std::mutex> lg(mutex_);
            if (closed_) {
                return false;
            }
            queue_.push(value);
        }
        // 锁外通知，避免被唤醒的线程立刻又阻塞在锁上
        not_empty_.notify_one();
        return true;
    }

    // 阻塞到有任务或 Close；返回 false 表示队列已关闭且为空。
    bool Pop(T& value) {
        std::unique_lock<std::mutex> ul(mutex_);
        // 谓词防止虚假唤醒；关闭后即使没任务也要醒。
        not_empty_.wait(ul, [this] { return closed_ || !queue_.empty(); });

        if (queue_.empty()) {
            return false;
        }
        value = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    // 非阻塞取任务；空队列返回 false。
    bool TryPop(T& value) {
        std::lock_guard<std::mutex> lg(mutex_);
        if (queue_.empty()) {
            return false;
        }
        value = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    // 关闭后不再接收新任务；已入队任务由 Pop 继续取完。
    void Close() {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            closed_ = true;
        }
        not_empty_.notify_all();
    }

private:
    std::queue<T> queue_;
    std::mutex mutex_;
    std::condition_variable not_empty_;
    bool closed_ = false;
};

// 固定线程数的线程池：Post 投递任务，析构时 Close + join。
//
// 线程模型：
// - 构造时启动 capacity_ 个 worker，每个 worker 阻塞在 BlockingQueue::Pop；
// - Post 把可调用对象 bind 成 std::function<void()> 入队；
// - 析构先 Close，worker 把队列剩余任务执行完后退出，再逐个 join。
//
// 当前限制：
// - 队列无界，没有拒绝策略/背压；
// - 线程数固定，不支持动态扩缩容；
// - 任务没有返回值，不支持 future。
class ThreadPool {
public:
    explicit ThreadPool(size_t c) : capacity_(c) {
        for (size_t i = 0; i < capacity_; ++i) {
            workers_.emplace_back([this, i] {
                // 只命名线程，便于压测时区分 IO/worker。
                const std::string name = "worker-" + std::to_string(i);
                pthread_setname_np(pthread_self(), name.c_str());
                work();
            });
        }
    }

    ~ThreadPool() {
        // 先关闭队列，再 join；worker 会把已入队任务取完。
        tasks_.Close();
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    // std::bind 把参数绑定成 void() 任务；队列关闭后返回 false。
    template <class F, class... Args>
    bool Post(F&& f, Args&&... args) {
        auto task = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
        return tasks_.Push(task);
    }

private:
    // worker 主循环：Pop 失败（关闭且为空）才退出。
    void work() {
        while (true) {
            std::function<void()> task;
            if (!tasks_.Pop(task)) {
                break;
            }
            try {
                task();
            } catch (...) {
                // 单个任务异常不能杀死 worker；TODO: 接入日志后记录。
            }
        }
    }

    std::vector<std::thread> workers_;
    BlockingQueue<std::function<void()>> tasks_;
    size_t capacity_;
};
