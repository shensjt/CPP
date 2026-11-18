// 异步日志测试（LOG 系列）

#include "log/async_logger.h"
#include "log/logger.h"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "test_framework.h"

namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;

    TempDir() {
        path = fs::temp_directory_path() /
               ("sjt_log_" + std::to_string(static_cast<long>(::getpid())));
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::string ReadFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

}  // namespace

// LOG1：Start/Append/Stop 后文件包含日志内容。
TEST(AsyncLogger_BasicWrite) {
    TempDir dir;
    const fs::path path = dir.path / "app.log";

    AsyncLogger logger;
    logger.Start(path.string(), 1024 * 1024, 50);
    CHECK(logger.Append("line-1\n"));
    CHECK(logger.Append("line-2\n"));
    logger.Stop();

    const std::string content = ReadFile(path);
    CHECK(content.find("line-1") != std::string::npos);
    CHECK(content.find("line-2") != std::string::npos);
}

// LOG2：多线程并发写，Stop 后行数不丢。
TEST(AsyncLogger_MultiThread) {
    TempDir dir;
    const fs::path path = dir.path / "multi.log";

    AsyncLogger logger;
    logger.Start(path.string(), 4 * 1024 * 1024, 50);

    constexpr int kThreads = 4;
    constexpr int kLines = 1000;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&logger, t] {
            for (int i = 0; i < kLines; ++i) {
                logger.Append("t" + std::to_string(t) + "-" + std::to_string(i) + "\n");
            }
        });
    }
    for (auto& thread : threads) thread.join();
    logger.Stop();

    const std::string content = ReadFile(path);
    const auto lines = std::count(content.begin(), content.end(), '\n');
    CHECK_EQ(lines, static_cast<long>(kThreads * kLines));
}

// LOG3：超过单文件上限后会生成 .1 滚动文件。
TEST(AsyncLogger_Rolling) {
    TempDir dir;
    const fs::path path = dir.path / "roll.log";

    AsyncLogger logger;
    logger.Start(path.string(), 256, 50);
    for (int i = 0; i < 50; ++i) {
        logger.Append("0123456789abcdefghijklmnopqrstuvwxyz\n");
    }
    logger.Stop();

    CHECK(fs::exists(path));
    CHECK(fs::exists(path.string() + ".1"));
}

// LOG4：未写满缓冲时，也按 flush 间隔落盘。
TEST(AsyncLogger_PeriodicFlush) {
    TempDir dir;
    const fs::path path = dir.path / "periodic.log";

    AsyncLogger logger;
    logger.Start(path.string(), 1024 * 1024, 50);
    CHECK(logger.Append("periodic\n"));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    const std::string content = ReadFile(path);
    CHECK(content.find("periodic") != std::string::npos);
    logger.Stop();
}

// LOG5：未 Start 时 Append 返回 false，Stop 安全。
TEST(AsyncLogger_NotStarted) {
    AsyncLogger logger;
    CHECK(!logger.Append("ignored\n"));
    logger.Stop();
}

// LOG5：Logger 宏输出时间、线程、级别和消息。
TEST(Logger_Formatting) {
    TempDir dir;
    const fs::path path = dir.path / "logger.log";

    Logger& logger = Logger::Instance();
    logger.Start(path.string(), 1024 * 1024, 50);
    LOG_INFO("value=%d", 42);
    LOG_WARN("warn-message");
    LOG_ERROR("error-message");
    logger.Stop();

    const std::string content = ReadFile(path);
    CHECK(content.find(" INFO value=42") != std::string::npos);
    CHECK(content.find(" WARN warn-message") != std::string::npos);
    CHECK(content.find(" ERROR error-message") != std::string::npos);
    CHECK(content.find('[') != std::string::npos);
}
