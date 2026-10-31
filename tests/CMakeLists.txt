# 测试目录 CMakeLists
#
# 目前作为独立工程使用（还没有顶层 CMake）：
#   cmake -S tests -B build/tests
#   cmake --build build/tests -j
#   ./build/tests/threadpool_tests
#
# TSan 构建：
#   cmake -S tests -B build/tests-tsan -DENABLE_TSAN=ON
#   cmake --build build/tests-tsan -j
#   ./build/tests-tsan/threadpool_tests
#
# 将来有了顶层 CMake 后，把 cmake_minimum_required / project 移到顶层，
# 本文件保留 add_executable / add_test 即可。

cmake_minimum_required(VERSION 3.16)
project(threadpool_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_executable(threadpool_tests
    test_main.cc
    test_blocking_queue.cc
    test_thread_pool.cc
)

target_include_directories(threadpool_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
)

target_link_libraries(threadpool_tests PRIVATE Threads::Threads)

target_compile_options(threadpool_tests PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

option(ENABLE_TSAN "Build with ThreadSanitizer" OFF)
option(ENABLE_ASAN "Build with AddressSanitizer + UBSan" OFF)

if(ENABLE_TSAN AND ENABLE_ASAN)
    message(FATAL_ERROR "ENABLE_TSAN and ENABLE_ASAN are mutually exclusive")
endif()

if(ENABLE_TSAN)
    target_compile_options(threadpool_tests PRIVATE -fsanitize=thread -g -O1)
    target_link_options(threadpool_tests PRIVATE -fsanitize=thread)
endif()

if(ENABLE_ASAN)
    target_compile_options(threadpool_tests PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1)
    target_link_options(threadpool_tests PRIVATE
        -fsanitize=address,undefined)
endif()

enable_testing()
add_test(NAME threadpool_tests COMMAND threadpool_tests)

# 防止死锁/睡死时 CI 永久挂起
set_tests_properties(threadpool_tests PROPERTIES TIMEOUT 180)
