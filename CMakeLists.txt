cmake_minimum_required(VERSION 3.16)
project(sjt_webserver LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_library(net STATIC
    common/net/epoll.cc
    common/net/buffer.cc
    common/net/tcp_connection.cc
    common/net/tcp_server.cc
)
target_include_directories(net PUBLIC common)
target_link_libraries(net PUBLIC Threads::Threads)
target_compile_options(net PRIVATE -Wall -Wextra -Wpedantic)

add_library(log STATIC
    common/log/async_logger.cc
)
target_include_directories(log PUBLIC common)
target_link_libraries(log PUBLIC Threads::Threads)
target_compile_options(log PRIVATE -Wall -Wextra -Wpedantic)

add_library(http STATIC
    projects/web_server/http/http_parser.cc
    projects/web_server/http/http_response.cc
    projects/web_server/http/static_file_handler.cc
    projects/web_server/http/http_server.cc
)
target_include_directories(http PUBLIC projects/web_server)
target_link_libraries(http PUBLIC net log)
target_compile_options(http PRIVATE -Wall -Wextra -Wpedantic)

add_library(kv STATIC
    projects/kv_store/protocol/resp.cc
    projects/kv_store/store/zset.cc
    projects/kv_store/store/kv_store.cc
    projects/kv_store/aof/aof.cc
    projects/kv_store/command/command_handler.cc
    projects/kv_store/server/kv_server.cc
)
target_include_directories(kv PUBLIC projects/kv_store common)
target_link_libraries(kv PUBLIC net log)
target_compile_options(kv PRIVATE -Wall -Wextra -Wpedantic)

add_executable(kv_store projects/kv_store/main.cpp)
target_link_libraries(kv_store PRIVATE kv log)
target_compile_options(kv_store PRIVATE -Wall -Wextra -Wpedantic)

add_executable(bench_kv projects/kv_store/bench/bench_kv.cc)
target_link_libraries(bench_kv PRIVATE kv)
target_compile_options(bench_kv PRIVATE -Wall -Wextra -Wpedantic)

add_executable(web_server projects/web_server/main.cpp)
target_link_libraries(web_server PRIVATE http)
target_compile_options(web_server PRIVATE -Wall -Wextra -Wpedantic)
