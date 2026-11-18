#pragma once

// 极简测试框架：零依赖，替代 GoogleTest 做单测演示。
// 特点：
//   - CHECK 不受 NDEBUG 影响（不像 assert 在 Release 下会被编译掉）
//   - 失败不 abort，记录后继续跑，最后统一汇报
//   - 线程安全：失败计数用 atomic，输出加锁
//   - TEST(name) 自动注册，main 中统一执行

#include <atomic>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace test {

inline std::atomic<int>& Failures() {
    static std::atomic<int> failures{0};
    return failures;
}

inline std::mutex& OutputMutex() {
    static std::mutex mutex;
    return mutex;
}

inline void Report(bool ok, const char* expr, const char* file, int line,
                   const std::string& detail = {}) {
    if (ok) return;

    Failures().fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(OutputMutex());
    std::cerr << file << ":" << line << ": CHECK failed: " << expr;
    if (!detail.empty()) std::cerr << " [" << detail << "]";
    std::cerr << '\n';
}

template <class A, class B>
void CheckEq(const A& lhs, const B& rhs, const char* expr,
             const char* file, int line) {
    if (lhs == rhs) return;

    std::ostringstream detail;
    detail << "lhs=" << lhs << ", rhs=" << rhs;
    Report(false, expr, file, line, detail.str());
}

using TestFn = void (*)();

struct TestCase {
    std::string name;
    TestFn fn;
};

inline std::vector<TestCase>& Registry() {
    static std::vector<TestCase> registry;
    return registry;
}

struct Registrar {
    Registrar(const char* name, TestFn fn) {
        Registry().push_back({name, fn});
    }
};

}  // namespace test

#define CHECK(cond) ::test::Report((cond), #cond, __FILE__, __LINE__)
#define CHECK_EQ(lhs, rhs) \
    ::test::CheckEq((lhs), (rhs), #lhs " == " #rhs, __FILE__, __LINE__)

#define TEST(test_name)                                              \
    static void test_name();                                         \
    [[maybe_unused]] static ::test::Registrar test_name##_registrar( \
        #test_name, test_name);                                      \
    static void test_name()
