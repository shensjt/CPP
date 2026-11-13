#include "net/buffer.h"

#include <cassert>
#include <cerrno>
#include <cstring>
#include <sys/uio.h>

// 初始 1KB，避免每个连接一上来就占大块内存。
Buffer::Buffer() : Buffer(kInitialSize) {}

Buffer::Buffer(std::size_t initial_size)
    : data_(initial_size), reader_index_(0), writer_index_(0) {}

std::size_t Buffer::ReadableBytes() const {
    return writer_index_ - reader_index_;
}

std::size_t Buffer::WritableBytes() const {
    return data_.size() - writer_index_;
}

std::size_t Buffer::PrependableBytes() const {
    return reader_index_;
}

const char* Buffer::Peek() const {
    return data_.data() + reader_index_;
}

char* Buffer::Begin() {
    return data_.data();
}

char* Buffer::BeginWrite() {
    return data_.data() + writer_index_;
}

void Buffer::HasWritten(std::size_t len) {
    assert(len <= WritableBytes());
    writer_index_ += len;
}

// 只移动读下标；不搬数据、不清零。
// 当 len == 可读长度时直接复位两个下标，避免 reader_ 一直往右漂。
void Buffer::Retrieve(std::size_t len) {
    assert(len <= ReadableBytes());
    if (len < ReadableBytes()) {
        reader_index_ += len;
    } else {
        RetrieveAll();
    }
}

void Buffer::RetrieveAll() {
    reader_index_ = 0;
    writer_index_ = 0;
}

// 先拷贝再消费：调用方拿到独立的 string，不依赖 Buffer 内部指针。
std::string Buffer::RetrieveAsString(std::size_t len) {
    assert(len <= ReadableBytes());
    std::string result(Peek(), len);
    Retrieve(len);
    return result;
}

std::string Buffer::RetrieveAllAsString() {
    return RetrieveAsString(ReadableBytes());
}

void Buffer::EnsureWritableBytes(std::size_t len) {
    if (WritableBytes() < len) {
        MakeSpace(len);
    }
}

// 追加可能触发 MakeSpace（memmove 或 resize），因此调用后旧指针失效。
void Buffer::Append(const char* data, std::size_t len) {
    EnsureWritableBytes(len);
    std::memcpy(BeginWrite(), data, len);
    HasWritten(len);
}

void Buffer::Append(const std::string& data) {
    Append(data.data(), data.size());
}

// 目标：腾出至少 len 字节可写区。
// - 总空闲（可写 + 已读走）都不够 len + 1KB：直接扩容；
// - 否则把可读数据挪到最前面，复用 prependable 区，避免扩容。
// +kInitialSize 是启发式：搬一次至少多留 1KB，避免频繁小数量的 memmove。
void Buffer::MakeSpace(std::size_t len) {
    if (WritableBytes() + PrependableBytes() < len + kInitialSize) {
        data_.resize(writer_index_ + len);
        return;
    }

    // 空闲空间够：把可读数据挪到最前面，复用前面的 prepend 区。
    const std::size_t readable = ReadableBytes();
    std::memmove(Begin(), Begin() + reader_index_, readable);
    reader_index_ = 0;
    writer_index_ = readable;
}

// 一次 readv 同时读两个位置：
//   vec[0] -> Buffer 当前可写区
//   vec[1] -> 栈上 64KB extrabuf
// 数据不超过可写区时零额外拷贝；超过时多出的部分 Append 进 Buffer。
// 这样既减少系统调用，也能吞下较大的突发数据。
ssize_t Buffer::ReadFd(int fd, int* saved_errno) {
    char extrabuf[65536];
    struct iovec vec[2];

    const std::size_t writable = WritableBytes();
    vec[0].iov_base = Begin() + writer_index_;
    vec[0].iov_len = writable;
    vec[1].iov_base = extrabuf;
    vec[1].iov_len = sizeof(extrabuf);

    // 可写区已经 >= 64KB 时，只用 vec[0]，没必要再读 extrabuf。
    const int iovcnt = (writable < sizeof(extrabuf)) ? 2 : 1;
    const ssize_t n = ::readv(fd, vec, iovcnt);

    if (n < 0) {
        // 保留 errno，调用方区分 EAGAIN / EINTR / 其他错误。
        if (saved_errno != nullptr) {
            *saved_errno = errno;
        }
    } else if (static_cast<std::size_t>(n) <= writable) {
        // 全部落在 Buffer 可写区，直接移动写指针。
        writer_index_ += static_cast<std::size_t>(n);
    } else {
        // 可写区被填满，剩余部分来自 extrabuf。
        writer_index_ = data_.size();
        Append(extrabuf, static_cast<std::size_t>(n) - writable);
    }
    return n;
}
