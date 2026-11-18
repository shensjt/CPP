// AOF 测试（AOF 系列）

#include "aof/aof.h"

#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "command/command_handler.h"
#include "protocol/resp.h"
#include "store/kv_store.h"
#include "test_framework.h"

namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;

    TempDir() {
        path = fs::temp_directory_path() /
               ("sjt_kv_aof_" + std::to_string(static_cast<long>(::getpid())));
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

Aof::ReplayResult ReplayInto(Aof& aof, const std::string& path,
                             KvStore& store) {
    CommandHandler handler(store);
    return aof.Replay(path, [&](const std::vector<std::string>& args) {
        handler.Execute(args);
    });
}

}  // namespace

// AOF1：追加命令，停止时落盘，重新回放能恢复数据。
TEST(Aof_AppendAndReplay) {
    TempDir dir;
    const std::string path = (dir.path / "kv.aof").string();

    {
        Aof aof;
        aof.Start(path, 0, 20);
        aof.Append(resp::EncodeCommand({"SET", "k", "v"}));
        aof.Append(resp::EncodeCommand({"ZADD", "z", "1.5", "a"}));
    }

    KvStore store;
    Aof reader;
    const auto result = ReplayInto(reader, path, store);
    CHECK_EQ(result.applied, std::size_t{2});

    std::string value;
    CHECK(store.Get("k", &value) == KvStore::Status::kOk);
    CHECK_EQ(value, std::string("v"));

    double score = 0.0;
    CHECK(store.ZScore("z", "a", &score) == KvStore::Status::kOk);
    CHECK_EQ(score, 1.5);
}

// AOF2：尾部半条命令不能被当成有效数据；截断后追加的新命令要能恢复。
TEST(Aof_TruncatedTail) {
    TempDir dir;
    const std::string path = (dir.path / "kv.aof").string();
    const std::string valid = resp::EncodeCommand({"SET", "a", "1"});

    {
        std::ofstream out(path, std::ios::binary);
        out << valid << "*3\r\n$3\r\nSET\r\n$1\r\nb";
    }

    KvStore first;
    Aof reader;
    const auto result = ReplayInto(reader, path, first);
    CHECK_EQ(result.applied, std::size_t{1});
    CHECK_EQ(result.valid_bytes, valid.size());

    {
        Aof aof;
        aof.Start(path, result.valid_bytes, 20);
        aof.Append(resp::EncodeCommand({"SET", "c", "3"}));
    }

    KvStore second;
    Aof reader2;
    const auto result2 = ReplayInto(reader2, path, second);
    CHECK_EQ(result2.applied, std::size_t{2});

    std::string value;
    CHECK(second.Get("a", &value) == KvStore::Status::kOk);
    CHECK_EQ(value, std::string("1"));
    CHECK(second.Get("c", &value) == KvStore::Status::kOk);
    CHECK_EQ(value, std::string("3"));
    CHECK(second.Get("b", &value) == KvStore::Status::kNotFound);
}

// AOF3：文件不存在时回放返回空。
TEST(Aof_MissingFile) {
    TempDir dir;
    const std::string path = (dir.path / "missing.aof").string();

    KvStore store;
    Aof reader;
    const auto result = ReplayInto(reader, path, store);
    CHECK_EQ(result.applied, std::size_t{0});
    CHECK_EQ(result.valid_bytes, std::size_t{0});
}

// AOF4：EncodeCommand 的字节格式。
TEST(Aof_EncodeCommand) {
    CHECK_EQ(resp::EncodeCommand({"SET", "k", "v"}),
             std::string("*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n"));
}

// AOF5：后台线程按间隔写盘，不需要等到 Stop。
TEST(Aof_BackgroundFlush) {
    TempDir dir;
    const std::string path = (dir.path / "kv.aof").string();

    Aof aof;
    aof.Start(path, 0, 20);
    aof.Append(resp::EncodeCommand({"SET", "k", "v"}));

    bool written = false;
    for (int i = 0; i < 100; ++i) {
        std::error_code ec;
        const auto size = fs::file_size(path, ec);
        if (!ec && size > 0) {
            written = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(written);
}

// AOF6：Stop 时必须把内存缓冲刷进文件。
TEST(Aof_StopFlushes) {
    TempDir dir;
    const std::string path = (dir.path / "kv.aof").string();

    {
        Aof aof;
        aof.Start(path, 0, 100000);  // 间隔很长，只能靠 Stop 刷
        aof.Append(resp::EncodeCommand({"SET", "k", "v"}));
    }

    std::error_code ec;
    CHECK(fs::file_size(path, ec) > 0);
}

// AOF7：目录不存在时自动创建。
TEST(Aof_CreatesDirectory) {
    TempDir dir;
    const std::string path = (dir.path / "nested" / "kv.aof").string();

    {
        Aof aof;
        aof.Start(path, 0, 20);
        aof.Append(resp::EncodeCommand({"SET", "k", "v"}));
    }

    CHECK(fs::exists(path));
}
