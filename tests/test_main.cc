// 测试入口：运行当前可执行文件链接进来的测试文件。
// 每个组件一个可执行文件：
//   ./threadpool_tests    BlockingQueue + ThreadPool
//   ./memory_pool_tests   MemoryPool
//
// 可选参数：名称过滤，例如 ./threadpool_tests BlockingQueue
//
// 建议再用外层 timeout 兜底，防止实现有 bug 时测试挂死：
//   timeout 180s ./threadpool_tests

#include <chrono>
#include <cstring>
#include <exception>
#include <iostream>
#include <string>

#include "test_framework.h"

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;

    int run = 0;
    int failed = 0;

    for (auto& test : test::Registry()) {
        if (filter != nullptr && test.name.find(filter) == std::string::npos) {
            continue;
        }

        const int failures_before = test::Failures().load();
        const auto start = std::chrono::steady_clock::now();

        try {
            test.fn();
        } catch (const std::exception& e) {
            test::Report(false, "test threw an exception", __FILE__, __LINE__,
                         e.what());
        } catch (...) {
            test::Report(false, "test threw an unknown exception", __FILE__,
                         __LINE__);
        }

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        const bool ok = test::Failures().load() == failures_before;

        ++run;
        if (!ok) ++failed;

        std::lock_guard<std::mutex> lock(test::OutputMutex());
        std::cout << (ok ? "[ OK ] " : "[FAIL] ") << test.name << " ("
                  << elapsed.count() << " ms)\n";
    }

    std::cout << "\n" << (run - failed) << "/" << run << " passed\n";
    return failed == 0 ? 0 : 1;
}
