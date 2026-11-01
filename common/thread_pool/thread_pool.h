#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>
#include <vector>

// 任务队列：Push 到队尾，Pop 从队头取。
// Close 之后 Push 返回 false；已入队的任务仍会被 Pop 取完。
template <class T>
class BlockingQueue {
public:
    BlockingQueue() = default;
    ~BlockingQueue() = default;

    BlockingQueue(const BlockingQueue&) = delete;
    BlockingQueue& operator=(const BlockingQueue&) = delete;

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
        not_empty_.wait(ul, [this] { return closed_ || !queue_.empty(); });

        if (queue_.empty()) {
            return false;
        }
        value = std::move(queue_.front());
        queue_.pop();
        return true;
    }

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
class ThreadPool {
public:
    explicit ThreadPool(size_t c) : capacity_(c) {
        for (size_t i = 0; i < capacity_; ++i) {
            workers_.emplace_back([this] { work(); });
        }
    }

    ~ThreadPool() {
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
    void work() {
        while (true) {
            std::function<void()> task;
            if (!tasks_.Pop(task)) {
                break;
            }
            try {
                task();
            } catch (...) {
                // TODO: 接入日志后记录异常
            }
        }
    }

    std::vector<std::thread> workers_;
    BlockingQueue<std::function<void()>> tasks_;
    size_t capacity_;
};
