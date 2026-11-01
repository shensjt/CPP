#pragma once

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// 双缓冲异步日志：前端只做内存拷贝，后端线程批量写文件并定期 fsync。
class AsyncLogger {
public:
    AsyncLogger();
    ~AsyncLogger();

    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;

    // flush_interval_ms 控制后台线程 fsync 的间隔，默认 1 秒。
    void Start(const std::string& path,
               std::size_t max_file_size = 64 * 1024 * 1024,
               int flush_interval_ms = 1000);
    void Stop();

    // 返回 false 表示日志未启动，调用方可以回退到 stderr。
    bool Append(std::string line);

private:
    using Buffer = std::string;

    void Backend();

    static constexpr std::size_t kBufferThreshold = 4 * 1024 * 1024;

    std::mutex mutex_;
    std::condition_variable cond_;
    std::unique_ptr<Buffer> current_;
    std::unique_ptr<Buffer> next_;
    std::vector<std::unique_ptr<Buffer>> pending_;
    std::unique_ptr<class LogFile> file_;
    std::thread thread_;
    bool running_ = false;
    int flush_interval_ms_ = 1000;
};
