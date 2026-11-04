#include "aof/aof.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "net/buffer.h"
#include "protocol/resp.h"

namespace {

constexpr std::size_t kFlushThreshold = 4 * 1024 * 1024;
constexpr std::size_t kReadChunk = 64 * 1024;

}  // namespace

Aof::~Aof() { Stop(); }

Aof::ReplayResult Aof::Replay(const std::string& path, const ApplyFn& apply) {
    ReplayResult result;

    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return result;  // 文件不存在视为空 AOF
    }

    Buffer buffer;
    resp::Parser parser;
    resp::Command command;
    std::size_t total_read = 0;
    bool stop = false;
    char chunk[kReadChunk];

    while (!stop) {
        const ssize_t n = ::read(fd, chunk, sizeof(chunk));
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;

        buffer.Append(chunk, static_cast<std::size_t>(n));
        total_read += static_cast<std::size_t>(n);

        while (true) {
            const auto parsed = parser.Parse(buffer, command);
            if (parsed == resp::Parser::Result::kNeedMore) {
                break;
            }
            if (parsed == resp::Parser::Result::kError) {
                // 尾部被截断或文件损坏：保留已经应用的前缀。
                stop = true;
                break;
            }
            apply(command.args);
            ++result.applied;
        }
    }

    ::close(fd);
    result.valid_bytes = total_read - buffer.ReadableBytes();
    return result;
}

void Aof::Start(const std::string& path, std::size_t valid_bytes,
                int flush_interval_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) {
        return;
    }

    std::error_code ec;
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            throw std::runtime_error("create aof directory failed: " +
                                     ec.message());
        }
    }

    const auto size = std::filesystem::file_size(path, ec);
    if (!ec && valid_bytes < size) {
        if (::truncate(path.c_str(), static_cast<off_t>(valid_bytes)) < 0) {
            throw std::system_error(errno, std::generic_category(),
                                    "truncate aof");
        }
    }

    fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd_ < 0) {
        throw std::system_error(errno, std::generic_category(), "open aof");
    }

    flush_interval_ms_ = std::max(1, flush_interval_ms);
    running_ = true;
    try {
        thread_ = std::thread([this] { Backend(); });
    } catch (...) {
        running_ = false;
        ::close(fd_);
        fd_ = -1;
        throw;
    }
}

void Aof::Stop() {
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ && !thread_.joinable()) {
            if (fd_ >= 0) {
                ::close(fd_);
                fd_ = -1;
            }
            buffer_.clear();
            return;
        }
        running_ = false;
        flush_now_ = true;
        cond_.notify_all();
        worker = std::move(thread_);
    }

    if (worker.joinable()) {
        worker.join();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (fd_ >= 0) {
        ::fsync(fd_);
        ::close(fd_);
        fd_ = -1;
    }
    buffer_.clear();
    flush_now_ = false;
}

void Aof::Append(std::string_view data) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) {
        return;
    }
    buffer_.append(data.data(), data.size());
    if (buffer_.size() >= kFlushThreshold) {
        flush_now_ = true;
        cond_.notify_one();
    }
}

void Aof::Backend() {
    std::string local;
    auto last_fsync = std::chrono::steady_clock::now();

    while (true) {
        bool stopping = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cond_.wait_for(lock, std::chrono::milliseconds(flush_interval_ms_),
                           [this] { return !running_ || flush_now_; });
            flush_now_ = false;
            local.swap(buffer_);
            stopping = !running_;
        }

        if (!local.empty()) {
            if (!WriteAll(fd_, local.data(), local.size())) {
                std::fprintf(stderr, "aof write failed: %s\n",
                             std::strerror(errno));
            }
            local.clear();
        }

        const auto now = std::chrono::steady_clock::now();
        if (stopping ||
            now - last_fsync >= std::chrono::milliseconds(flush_interval_ms_)) {
            if (fd_ >= 0 && ::fsync(fd_) < 0) {
                std::fprintf(stderr, "aof fsync failed: %s\n",
                             std::strerror(errno));
            }
            last_fsync = now;
        }

        if (stopping) {
            break;
        }
    }
}

bool Aof::WriteAll(int fd, const char* data, std::size_t size) {
    std::size_t written = 0;
    while (written < size) {
        const ssize_t n = ::write(fd, data + written, size - written);
        if (n > 0) {
            written += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}
