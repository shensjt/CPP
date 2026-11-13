#pragma once

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <ctime>
#include <string>
#include <sys/syscall.h>
#include <unistd.h>

#include "log/async_logger.h"

enum class LogLevel { kInfo, kWarn, kError };

// 全局日志入口：格式化时间/线程/级别后交给 AsyncLogger。
//
// 行为：
// - Start 之前或 Start 失败时，日志回退到 stderr，避免初始化前丢信息；
// - Log 使用固定栈缓冲（message 4KB、line 4.3KB），超长日志会被截断；
// - 这条路径在业务线程里只做格式化 + 一次内存拷贝，不直接写盘。
class Logger {
public:
    static Logger& Instance() {
        static Logger logger;
        return logger;
    }

    // max_file_size 控制单个日志文件滚动阈值；flush_interval_ms 控制 fsync 间隔。
    void Start(const std::string& path,
               std::size_t max_file_size = 64 * 1024 * 1024,
               int flush_interval_ms = 1000) {
        logger_.Start(path, max_file_size, flush_interval_ms);
        started_.store(true);
    }

    // 先停止接收新日志，再让 AsyncLogger 刷完剩余缓冲并 join 后台线程。
    void Stop() {
        started_.store(false);
        logger_.Stop();
    }

    // printf 风格：格式串 + 可变参数。
    // 输出格式：YYYY-MM-DD HH:MM:SS.mmm [tid] LEVEL message\n
    void Log(LogLevel level, const char* fmt, ...) {
        char message[4096];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(message, sizeof(message), fmt, args);
        va_end(args);

        const auto now = std::chrono::system_clock::now();
        const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
        std::tm local{};
        // localtime_r 可重入；localtime 返回静态缓冲区，多线程不安全。
        localtime_r(&seconds, &local);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch())
                            .count() %
                        1000;
        // Linux 的 gettid 没有 glibc 包装，用 syscall 取线程 id。
        const long tid = static_cast<long>(::syscall(SYS_gettid));

        char line[4352];
        std::snprintf(line, sizeof(line),
                      "%04d-%02d-%02d %02d:%02d:%02d.%03lld [%ld] %s %s\n",
                      local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                      local.tm_hour, local.tm_min, local.tm_sec,
                      static_cast<long long>(ms), tid, LevelName(level), message);

        // Append 返回 false 表示 AsyncLogger 没启动；回退 stderr。
        if (started_.load() && logger_.Append(line)) {
            return;
        }
        std::fputs(line, stderr);
    }

private:
    static const char* LevelName(LogLevel level) {
        switch (level) {
            case LogLevel::kInfo: return "INFO";
            case LogLevel::kWarn: return "WARN";
            case LogLevel::kError: return "ERROR";
        }
        return "INFO";
    }

    AsyncLogger logger_;
    std::atomic<bool> started_{false};
};

#define LOG_INFO(...) ::Logger::Instance().Log(::LogLevel::kInfo, __VA_ARGS__)
#define LOG_WARN(...) ::Logger::Instance().Log(::LogLevel::kWarn, __VA_ARGS__)
#define LOG_ERROR(...) ::Logger::Instance().Log(::LogLevel::kError, __VA_ARGS__)
