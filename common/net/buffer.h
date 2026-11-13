#pragma once

#include <cstddef>
#include <string>
#include <sys/types.h>
#include <vector>

// 分段字节缓冲区：prependable | readable | writable。
//
// 设计目的：
// - TCP 是字节流，读到的数据可能半包/粘包，需要先攒起来给解析器；
// - 写响应时可能短写，需要把没发完的数据留在写 Buffer；
// - 用 reader_index_ / writer_index_ 切分 vector，Retrieve 只移动下标，
//   避免每次 erase(0, n) 的 O(n) 拷贝。
//
// 线程模型：
// - 非线程安全；
// - 每个 TcpConnection 各持有一个读 Buffer 和一个写 Buffer；
// - EPOLLONESHOT 保证同一连接同一时刻只被一个 worker 访问，所以不需要加锁。
//
// 指针有效期：
// - Peek() / BeginWrite() 返回的指针在下一次 Append / EnsureWritableBytes /
//   ReadFd 之后可能失效（MakeSpace 会 memmove 或 resize）；
// - 不要跨这些调用保存裸指针。
class Buffer {
public:
    static constexpr std::size_t kInitialSize = 1024;

    Buffer();
    explicit Buffer(std::size_t initial_size);

    // 三段长度：
    //   Readable   = writer_index_ - reader_index_
    //   Writable   = data_.size()   - writer_index_
    //   Prependable= reader_index_
    std::size_t ReadableBytes() const;
    std::size_t WritableBytes() const;
    std::size_t PrependableBytes() const;

    // 返回可读区首地址，不拷贝。调用方需自己保证读取长度不超过 ReadableBytes()。
    const char* Peek() const;

    // 只移动读下标，O(1)；不擦除内存。
    // len >= ReadableBytes() 时等价于 RetrieveAll()。
    void Retrieve(std::size_t len);
    void RetrieveAll();

    // 拷贝出前 len 字节，并消费这 len 字节。
    std::string RetrieveAsString(std::size_t len);
    std::string RetrieveAllAsString();

    // 追加到可写区；空间不够时 MakeSpace 可能 memmove 或 resize。
    // 追加后，之前通过 Peek() / BeginWrite() 拿到的指针可能失效。
    void Append(const char* data, std::size_t len);
    void Append(const std::string& data);

    // 保证至少有 len 字节可写；可能触发 MakeSpace，使旧指针失效。
    void EnsureWritableBytes(std::size_t len);

    // 低层写接口：先 EnsureWritableBytes(n)，写 BeginWrite()，再 HasWritten(n)。
    // 不要跳过 Ensure 直接写，否则可能越界。
    char* BeginWrite();
    void HasWritten(std::size_t len);

    // 用 readv 一次读取“Buffer 可写区 + 栈上 64KB extrabuf”。
    // 返回 readv 的结果：
    //   > 0  读到的字节数（已写入 Buffer）
    //   = 0  对端关闭
    //   < 0  失败，errno 写入 saved_errno；调用方区分 EAGAIN / EINTR / 其他。
    ssize_t ReadFd(int fd, int* saved_errno);

private:
    char* Begin();
    void MakeSpace(std::size_t len);

    std::vector<char> data_;
    std::size_t reader_index_;
    std::size_t writer_index_;
};
