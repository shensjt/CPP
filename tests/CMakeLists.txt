# 测试目录 CMakeLists
#
# 目录按项目分层：
#   framework/      test_main + test_framework
#   common/         thread_pool / memory_pool / net / log
#   web_server/     http / lru_cache
#   kv_store/
#
# 每个组件一个测试可执行文件：
#   threadpool_tests   —— BlockingQueue + ThreadPool
#   memory_pool_tests  —— MemoryPool
#   net_tests          —— Epoll / Buffer / TcpConnection / TcpServer
#   log_tests          —— AsyncLogger
#   web_server_tests   —— HTTP / LRU / HttpServer
#   kv_tests           —— SkipList / RESP / KVStore / AOF / KvServer
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

# 每个组件一个可执行文件，共享 framework/test_main.cc 和测试框架。
function(add_component_test name)
    add_executable(${name} framework/test_main.cc ${ARGN})

    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/framework
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

# common/thread_pool
add_component_test(threadpool_tests
    common/thread_pool/test_blocking_queue.cc
    common/thread_pool/test_thread_pool.cc
)

# common/memory_pool
add_component_test(memory_pool_tests
    common/memory_pool/test_memory_pool.cc
)

# common/net
add_component_test(net_tests
    common/net/test_epoll.cc
    common/net/test_tcp_server.cc
    common/net/test_tcp_server_pooled.cc
    common/net/test_tcp_connection.cc
    common/net/test_buffer.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/net/epoll.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/net/tcp_server.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/net/tcp_connection.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/net/buffer.cc
)

# common/log
add_component_test(log_tests
    common/log/test_async_logger.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/log/async_logger.cc
)

# projects/kv_store
add_component_test(kv_tests
    kv_store/test_skiplist.cc
    kv_store/test_resp.cc
    kv_store/test_kv_store.cc
    kv_store/test_command_handler.cc
    kv_store/test_kv_server.cc
    kv_store/test_aof.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../projects/kv_store/protocol/resp.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../projects/kv_store/store/zset.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../projects/kv_store/store/kv_store.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../projects/kv_store/aof/aof.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../projects/kv_store/command/command_handler.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../projects/kv_store/server/kv_server.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/net/buffer.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/net/epoll.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/net/tcp_connection.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/net/tcp_server.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/log/async_logger.cc
)

# projects/web_server
add_component_test(web_server_tests
    web_server/http/test_http_parser.cc
    web_server/http/test_http_response.cc
    web_server/http/test_static_file.cc
    web_server/http/test_http_server.cc
    web_server/lru_cache/test_lru_cache.cc
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
