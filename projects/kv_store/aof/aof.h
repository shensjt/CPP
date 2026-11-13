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
//
// 写路径：
//   调用方持有 store 写锁 -> 改内存 -> Aof::Append
//   Append 只做内存拷贝，后台线程负责 write + fsync。
//
// 启动：
//   Replay 只读解析 -> 返回最后一条完整命令的 valid_bytes
//   Start 先 truncate 掉尾部半条命令，再 O_APPEND 打开并启动后台线程
//
// 锁顺序：store 锁 -> AOF 锁；后台线程只拿 AOF 锁，不反向拿 store 锁。
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
    // 文件不存在视为空 AOF；遇到半条/损坏命令就停止，保留已应用前缀。
    ReplayResult Replay(const std::string& path, const ApplyFn& apply);

    // 先截掉 valid_bytes 之后的残留（崩溃造成的半条命令），
    // 再以 O_APPEND 打开并启动后台线程。
    // 不截断就追加，新命令会接在半条命令后面，下次 replay 读不到。
    void Start(const std::string& path, std::size_t valid_bytes,
               int flush_interval_ms = 1000);

    // 停止后台线程：把 buffer_ 写完、fsync、关闭 fd。
    void Stop();

    // 只做内存拷贝；真正写盘和 fsync 在后台线程。
    // 必须在 store 写锁内调用，保证并发写的落盘顺序。
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
