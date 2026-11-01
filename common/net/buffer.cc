#include "net/buffer.h"

#include <cassert>
#include <cerrno>
#include <cstring>
#include <sys/uio.h>

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

void Buffer::Append(const char* data, std::size_t len) {
    EnsureWritableBytes(len);
    std::memcpy(BeginWrite(), data, len);
    HasWritten(len);
}

void Buffer::Append(const std::string& data) {
    Append(data.data(), data.size());
}

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

ssize_t Buffer::ReadFd(int fd, int* saved_errno) {
    char extrabuf[65536];
    struct iovec vec[2];

    const std::size_t writable = WritableBytes();
    vec[0].iov_base = Begin() + writer_index_;
    vec[0].iov_len = writable;
    vec[1].iov_base = extrabuf;
    vec[1].iov_len = sizeof(extrabuf);

    const int iovcnt = (writable < sizeof(extrabuf)) ? 2 : 1;
    const ssize_t n = ::readv(fd, vec, iovcnt);

    if (n < 0) {
        if (saved_errno != nullptr) {
            *saved_errno = errno;
        }
    } else if (static_cast<std::size_t>(n) <= writable) {
        writer_index_ += static_cast<std::size_t>(n);
    } else {
        writer_index_ = data_.size();
        Append(extrabuf, static_cast<std::size_t>(n) - writable);
    }
    return n;
}
