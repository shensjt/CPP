#pragma once
#include <thread>
#include <mutex>
#include <functional>
#include <queue>
#include <condition_variable>
#include <vector>

// 阻塞队列
template<class T>
class BlockingQueue {
public:
    BlockingQueue() = default;
    ~BlockingQueue() = default;

    BlockingQueue(const BlockingQueue&) = delete;
    BlockingQueue& operator=(const BlockingQueue&) = delete;


    // 入队列
    bool Push(const T& value) {
        {
            std::lock_guard<std::mutex> lg(mutex_);
            // 关闭检查
            if(closed_)
                return false;
            queue_.push(value);
        } 
        // 提醒一个
        not_empty_.notify_one();
        return true;
    }

    // 出队列
    bool Pop(T& value) {
        std::unique_lock<std::mutex> ul(mutex_);

        // wait,并且防止虚假唤醒，检查是否真有任务，且确保没有关闭
        not_empty_.wait(ul, [this]()->bool{ return closed_ || !queue_.empty(); } );

        // 非空检测
        if(queue_.empty()) return false;

        // 取值
        value = std::move(queue_.front());
        queue_.pop();

        return true;
    }
    
    bool TryPop(T& value) {
        std::lock_guard<std::mutex> lg(mutex_);
        // 非空检测
        if(queue_.empty()) return false;
        // 取值
        value = std::move(queue_.front());
        queue_.pop();
        return true;
    }
    

    void Close() {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            closed_ = true;
        }
        not_empty_.notify_all();
    }
private:
    // 基础队列
    std::queue<T> queue_;
    // 锁
    std::mutex mutex_;
    // 条件变量,队列不为空
    std::condition_variable not_empty_;
    // 是否关闭
    bool closed_ = false;
};

class ThreadPool {
public:
    explicit ThreadPool(size_t c): capacity_(c) {
        for(size_t i = 0; i < capacity_; i++) {
            workers_.emplace_back([this]()->void{ work(); });
        }
    }

    ~ThreadPool() {
        tasks_.Close();
        for(auto& worker : workers_) {
            if(worker.joinable()) {
                worker.join();
            }
        }
    }

    // 模板，来bind，并完美转发至阻塞队列
    template<class F, class... Args>
    bool Post(F&& f,Args&&... args) {
        // bind并完美转发
        auto task = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
        return tasks_.Push(task);
    }


private:
    std::vector<std::thread> workers_;
    BlockingQueue<std::function<void()>> tasks_;
    size_t capacity_;
    void work() {
        while(true) {
            std::function<void()> task;
            if(!tasks_.Pop(task))
                break;
            try{
                task();
            } catch(...) {
                // 日志
            }
        
        
        }
    }
};



