#pragma once

#include <cstddef>
#include <string>
#include <sys/types.h>
#include <vector>

// 分段缓冲区：prependable | readable | writable。
// reader_index_ / writer_index_ 把 vector 切成三段，读数据不搬移。
// ReadFd 用 readv 一次读缓冲区剩余空间 + 栈上 extrabuf，减少系统调用。
class Buffer {
public:
    static constexpr std::size_t kInitialSize = 1024;

    Buffer();
    explicit Buffer(std::size_t initial_size);

    std::size_t ReadableBytes() const;
    std::size_t WritableBytes() const;
    std::size_t PrependableBytes() const;

    const char* Peek() const;
    void Retrieve(std::size_t len);
    void RetrieveAll();

    std::string RetrieveAsString(std::size_t len);
    std::string RetrieveAllAsString();

    void Append(const char* data, std::size_t len);
    void Append(const std::string& data);

    void EnsureWritableBytes(std::size_t len);
    char* BeginWrite();
    void HasWritten(std::size_t len);

    // 返回 readv 的结果；出错时把 errno 写入 saved_errno。
    ssize_t ReadFd(int fd, int* saved_errno);

private:
    char* Begin();
    void MakeSpace(std::size_t len);

    std::vector<char> data_;
    std::size_t reader_index_;
    std::size_t writer_index_;
};
