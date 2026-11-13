#pragma once

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// 双缓冲异步日志：前端只做内存拷贝，后端线程批量写文件并定期 fsync。
//
// 缓冲流转：
//   Append -> current_
//   current_ 超过 4MB 或到 flush 间隔 -> 移到 pending_ 并唤醒后端
//   后端把 pending_ swap 到本地，锁外写文件
//
// 线程模型：
// - 多个业务线程调用 Append，mutex_ 保护缓冲和 pending_；
// - 一个后台线程 Backend 负责写文件和 fsync；
// - Stop 会停止接收新日志，把剩余 current_ 交给后端，join 后关闭文件。
//
// 持久化语义：
// - 前端返回 true 只表示“已进入内存缓冲”，不表示已落盘；
// - 后台按 flush_interval_ms fsync，崩溃最多丢约一个间隔的数据。
class AsyncLogger {
public:
    AsyncLogger();
    ~AsyncLogger();

    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;

    // flush_interval_ms 控制后台线程 fsync 的间隔，默认 1 秒。
    // max_file_size 控制单个日志文件的滚动阈值。
    void Start(const std::string& path,
               std::size_t max_file_size = 64 * 1024 * 1024,
               int flush_interval_ms = 1000);
    void Stop();

    // 返回 false 表示日志未启动，调用方可以回退到 stderr。
    // 只做一次内存 append，不写盘。
    bool Append(std::string line);

private:
    using Buffer = std::string;

    // 后端主循环：等待 current_ 满/超时/Stop，交换缓冲并写文件。
    void Backend();

    // current_ 超过这个阈值就提前唤醒后端，避免内存无限增长。
    static constexpr std::size_t kBufferThreshold = 4 * 1024 * 1024;

    std::mutex mutex_;
    std::condition_variable cond_;
    std::unique_ptr<Buffer> current_;  // 前端正在写的缓冲
    std::unique_ptr<Buffer> next_;     // 备用缓冲，交换后继续写
    std::vector<std::unique_ptr<Buffer>> pending_;  // 待后端写盘的缓冲
    std::unique_ptr<class LogFile> file_;           // 定义在 .cc
    std::thread thread_;
    bool running_ = false;
    int flush_interval_ms_ = 1000;
};
