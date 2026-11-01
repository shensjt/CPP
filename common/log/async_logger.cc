#include "log/async_logger.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <unistd.h>
#include <utility>

class LogFile {
public:
    LogFile(std::string path, std::size_t max_size)
        : path_(std::move(path)), max_size_(max_size) {
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
        std::filesystem::rename(path_, path_ + ".1", ec);
        Open();
        written_ = 0;
    }

    std::string path_;
    std::size_t max_size_;
    std::size_t written_ = 0;
    std::FILE* file_ = nullptr;
};

AsyncLogger::AsyncLogger() = default;

AsyncLogger::~AsyncLogger() {
    Stop();
}

void AsyncLogger::Start(const std::string& path, std::size_t max_file_size,
                        int flush_interval_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) return;

    file_ = std::make_unique<LogFile>(path, max_file_size);
    current_ = std::make_unique<Buffer>();
    next_ = std::make_unique<Buffer>();
    pending_.clear();
    flush_interval_ms_ = std::max(1, flush_interval_ms);
    running_ = true;
    thread_ = std::thread([this] { Backend(); });
}

void AsyncLogger::Stop() {
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ && !thread_.joinable()) {
            if (file_ != nullptr) file_->Fsync();
            return;
        }
        running_ = false;
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
                // 即使没写满，也按 flush 间隔把当前缓冲交给后端。
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
