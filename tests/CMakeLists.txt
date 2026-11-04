# 测试目录 CMakeLists
#
# 每个组件一个测试可执行文件：
#   threadpool_tests   —— BlockingQueue + ThreadPool
#   memory_pool_tests  —— MemoryPool
#
# 正常构建：
#   cmake -S tests -B build/tests
#   cmake --build build/tests -j
#   ctest --test-dir build/tests --output-on-failure
#
# TSan 构建：
#   cmake -S tests -B build/tests-tsan -DENABLE_TSAN=ON
#
# ASan 构建：
#   cmake -S tests -B build/tests-asan -DENABLE_ASAN=ON

cmake_minimum_required(VERSION 3.16)
project(cpp_component_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

option(ENABLE_TSAN "Build with ThreadSanitizer" OFF)
option(ENABLE_ASAN "Build with AddressSanitizer + UBSan" OFF)

if(ENABLE_TSAN AND ENABLE_ASAN)
    message(FATAL_ERROR "ENABLE_TSAN and ENABLE_ASAN are mutually exclusive")
endif()

enable_testing()

# 每个组件一个可执行文件，共享 test_main.cc 和测试框架。
function(add_component_test name)
    add_executable(${name} test_main.cc ${ARGN})

    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../common
        ${CMAKE_CURRENT_SOURCE_DIR}/../projects/web_server
        ${CMAKE_CURRENT_SOURCE_DIR}/../projects/kv_store
    )
    target_link_libraries(${name} PRIVATE Threads::Threads)

    target_compile_options(${name} PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )

    if(ENABLE_TSAN)
        target_compile_options(${name} PRIVATE -fsanitize=thread -g -O1)
        target_link_options(${name} PRIVATE -fsanitize=thread)
    endif()

    if(ENABLE_ASAN)
        target_compile_options(${name} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1)
        target_link_options(${name} PRIVATE
            -fsanitize=address,undefined)
    endif()

    add_test(NAME ${name} COMMAND ${name})

    # 防止死锁/睡死时 CI 永久挂起
    set_tests_properties(${name} PROPERTIES TIMEOUT 180)
endfunction()

add_component_test(threadpool_tests
    test_blocking_queue.cc
    test_thread_pool.cc
)

add_component_test(memory_pool_tests
    test_memory_pool.cc
)

add_component_test(net_tests
    test_epoll.cc
    test_tcp_server.cc
    test_tcp_server_pooled.cc
    test_tcp_connection.cc
    test_buffer.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/net/epoll.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/net/tcp_server.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/net/tcp_connection.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/net/buffer.cc
)

add_component_test(log_tests
    test_async_logger.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/log/async_logger.cc
)

add_component_test(kv_tests
    test_skiplist.cc
    test_resp.cc
    test_kv_store.cc
    test_command_handler.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../projects/kv_store/protocol/resp.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../projects/kv_store/store/zset.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../projects/kv_store/store/kv_store.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../projects/kv_store/command/command_handler.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/net/buffer.cc
)

add_component_test(web_server_tests
    test_http_parser.cc
    test_http_response.cc
    test_static_file.cc
    test_lru_cache.cc
    test_http_server.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../projects/web_server/http/http_parser.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../projects/web_server/http/http_response.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../projects/web_server/http/static_file_handler.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../projects/web_server/http/http_server.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/net/buffer.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/net/epoll.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/net/tcp_connection.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/net/tcp_server.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/log/async_logger.cc
)
