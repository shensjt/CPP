#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// AOF：把写命令按 RESP 编码追加到文件，后台线程按间隔 fsync。
// 调用方负责在“内存修改 + Append”外面持有写锁，保证落盘顺序和生效顺序一致。
class Aof {
public:
    struct ReplayResult {
        std::size_t applied = 0;
        std::size_t valid_bytes = 0;  // 最后一条完整命令的字节数
    };

    using ApplyFn = std::function<void(const std::vector<std::string>&)>;

    Aof() = default;
    ~Aof();

    Aof(const Aof&) = delete;
    Aof& operator=(const Aof&) = delete;

    // 只读重放，不追加。返回应用了多少条命令、有效字节数。
    ReplayResult Replay(const std::string& path, const ApplyFn& apply);

    // 先截掉 valid_bytes 之后的残留（崩溃造成的半条命令），再追加打开并启动后台线程。
    void Start(const std::string& path, std::size_t valid_bytes,
               int flush_interval_ms = 1000);

    void Stop();

    // 只做内存拷贝；真正写盘和 fsync 在后台线程。
    void Append(std::string_view data);

private:
    void Backend();
    static bool WriteAll(int fd, const char* data, std::size_t size);

    std::mutex mutex_;
    std::condition_variable cond_;
    std::string buffer_;
    bool running_ = false;
    bool flush_now_ = false;
    int flush_interval_ms_ = 1000;
    int fd_ = -1;
    std::thread thread_;
};
