#include "log/async_logger.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <unistd.h>
#include <utility>

// 日志文件的薄封装：追加、滚动、fsync。
// 只由 AsyncLogger 的后端线程使用，所以内部不加锁。
class LogFile {
public:
    // 打开/创建文件；父目录不存在时自动创建。
    // 如果文件已存在，written_ 从当前大小开始，滚动阈值按累计大小算。
    LogFile(std::string path, std::size_t max_size, std::size_t max_files)
        : path_(std::move(path)),
          max_size_(max_size),
          max_files_(max_files) {
        std::error_code ec;
        const auto parent = std::filesystem::path(path_).parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, ec);
        }
        if (std::filesystem::exists(path_, ec)) {
            written_ = static_cast<std::size_t>(std::filesystem::file_size(path_, ec));
        }
        Open();
    }

    ~LogFile() {
        if (file_ != nullptr) {
            std::fflush(file_);
            ::fsync(::fileno(file_));
            std::fclose(file_);
        }
    }

    LogFile(const LogFile&) = delete;
    LogFile& operator=(const LogFile&) = delete;

    // 按 chunk 写：一整块 4MB 数据不能一次性 fwrite，否则永远不会触发滚动。
    // 每块写到 max_size_ 边界就 Roll，换新文件继续写剩余部分。
    void Append(const char* data, std::size_t len) {
        if (file_ == nullptr) return;
        while (len > 0) {
            if (written_ > 0 && written_ + len > max_size_) {
                Roll();
            }
            const std::size_t space = max_size_ > written_ ? max_size_ - written_ : 1;
            const std::size_t chunk = std::min(len, space);
            std::fwrite(data, 1, chunk, file_);
            written_ += chunk;
            data += chunk;
            len -= chunk;
            if (written_ >= max_size_) {
                Roll();
            }
        }
    }

    void Flush() {
        if (file_ != nullptr) std::fflush(file_);
    }

    void Fsync() {
        if (file_ != nullptr) {
            std::fflush(file_);
            ::fsync(::fileno(file_));
        }
    }

private:
    void Open() {
        file_ = std::fopen(path_.c_str(), "a");
        if (file_ == nullptr) {
            throw std::runtime_error("open log file failed: " + path_);
        }
    }

    void Roll() {
        if (file_ != nullptr) {
            std::fflush(file_);
            ::fsync(::fileno(file_));
            std::fclose(file_);
            file_ = nullptr;
        }

        std::error_code ec;
        std::filesystem::remove(path_ + "." + std::to_string(max_files_), ec);
        for (std::size_t i = max_files_; i > 1; --i) {
            std::filesystem::rename(path_ + "." + std::to_string(i - 1),
                                    path_ + "." + std::to_string(i), ec);
        }
        std::filesystem::rename(path_, path_ + ".1", ec);
        Open();
        written_ = 0;
    }

    std::string path_;
    std::size_t max_size_;
    std::size_t max_files_;
    std::size_t written_ = 0;
    std::FILE* file_ = nullptr;
};

AsyncLogger::AsyncLogger() = default;

AsyncLogger::~AsyncLogger() {
    Stop();
}

// 启动后端线程；重复 Start 直接忽略。
// 文件打开失败会抛异常，由调用方决定是否回退 stderr。
void AsyncLogger::Start(const std::string& path, std::size_t max_file_size,
                        int flush_interval_ms, std::size_t max_files) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) return;

    file_ = std::make_unique<LogFile>(path, max_file_size,
                                      std::max<std::size_t>(1, max_files));
    current_ = std::make_unique<Buffer>();
    next_ = std::make_unique<Buffer>();
    pending_.clear();
    flush_interval_ms_ = std::max(1, flush_interval_ms);
    running_ = true;
    thread_ = std::thread([this] { Backend(); });
}

// 停止：把 running_ 置 false，交出当前缓冲，唤醒后端并 join。
// join 之后才关闭文件，保证后端线程不会写一个已关闭的 FILE*。
void AsyncLogger::Stop() {
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ && !thread_.joinable()) {
            if (file_ != nullptr) file_->Fsync();
            return;
        }
        running_ = false;
        // 停机时前端可能还有未满的缓冲，必须交给后端一起写完。
        if (current_ != nullptr && !current_->empty()) {
            pending_.push_back(std::move(current_));
        }
        cond_.notify_all();
        worker = std::move(thread_);
    }

    if (worker.joinable()) worker.join();

    std::lock_guard<std::mutex> lock(mutex_);
    if (file_ != nullptr) {
        file_->Fsync();
        file_.reset();
    }
    current_.reset();
    next_.reset();
    pending_.clear();
}

// 前端入口：锁内只做一次字符串 append。
// 缓冲超过阈值时交换到 pending_ 并唤醒后端，锁持有时间仍然很短。
bool AsyncLogger::Append(std::string line) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) return false;

    if (current_->size() + line.size() > kBufferThreshold) {
        pending_.push_back(std::move(current_));
        if (next_ != nullptr) {
            current_ = std::move(next_);
        } else {
            current_ = std::make_unique<Buffer>();
        }
        next_ = std::make_unique<Buffer>();
        cond_.notify_one();
    }

    current_->append(line);
    return true;
}

// 后端循环：
// 1. 等 pending_ 非空、超时或 Stop；
// 2. 把 pending_ swap 到本地（锁外写盘，避免前端被磁盘 IO 卡住）；
// 3. 批量写文件、fflush；
// 4. 按间隔 fsync；
// 5. Stop 时把剩余数据写完再退出。
void AsyncLogger::Backend() {
    std::vector<std::unique_ptr<Buffer>> local;
    auto last_fsync = std::chrono::steady_clock::now();

    while (true) {
        bool stopping = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cond_.wait_for(lock, std::chrono::milliseconds(flush_interval_ms_),
                           [this] { return !running_ || !pending_.empty(); });

            if (!running_) {
                if (current_ != nullptr && !current_->empty()) {
                    pending_.push_back(std::move(current_));
                }
                stopping = true;
            } else if (current_ != nullptr && !current_->empty()) {
                // 即使没写满，也按 flush 间隔把当前缓冲交给后端，
                // 否则低流量时日志会一直留在内存里，运行期间看不到。
                pending_.push_back(std::move(current_));
                if (next_ != nullptr) {
                    current_ = std::move(next_);
                } else {
                    current_ = std::make_unique<Buffer>();
                }
                next_ = std::make_unique<Buffer>();
            }

            local.swap(pending_);
        }

        // 锁外写盘：这一步可能慢，不能占着 mutex_。
        for (auto& buffer : local) {
            if (buffer != nullptr && !buffer->empty()) {
                file_->Append(buffer->data(), buffer->size());
            }
        }
        local.clear();
        file_->Flush();

        const auto now = std::chrono::steady_clock::now();
        if (stopping ||
            now - last_fsync >= std::chrono::milliseconds(flush_interval_ms_)) {
            file_->Fsync();
            last_fsync = now;
        }

        if (stopping) break;
    }
}
