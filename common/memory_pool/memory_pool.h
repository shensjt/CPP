#pragma once

#include <cstddef>
#include <cstdlib>
#include <mutex>
#include <new>
#include <vector>

// 固定大小分桶的内存池：8B–4KB，共 10 个桶。
//
// 结构：
// - 每个桶一条侵入式空闲链表（free list），链表节点就存在空闲块的前 8 字节里；
// - 分配 = 摘链表头 O(1)，释放 = 头插 O(1)；
// - 桶空时向系统 malloc 一个 4MB chunk，切成该桶大小的块挂上去；
// - 大于 4KB 的请求直接走 std::malloc / std::free，不经过桶。
//
// 线程模型：
// - 每个桶一把 mutex，不同大小类的分配可以并行；
// - 同一桶内的分配/释放串行；
// - 这是“先保证正确”的版本，后续可换 thread-local cache 降锁竞争。
//
// 当前限制：
// - Deallocate 必须传入与分配时相同大小类的 size，否则会还错桶；
// - chunk 只增不减，析构时统一 free；
// - 没有对齐处理（按 8 字节对齐，足够常见类型使用）。
class MemoryPool {
public:
    MemoryPool() = default;

    ~MemoryPool() {
        // 所有 chunk 都是 malloc 来的，直接 free；块内对象不需要单独析构。
        for (auto& b : buckets_) {
            for (void* c : b.chunks_) {
                std::free(c);
            }
        }
    }

    MemoryPool& operator=(const MemoryPool&) = delete;
    MemoryPool(const MemoryPool&) = delete;

    // size 为 0 按 1 处理；超过 4KB 直接走系统 malloc。
    // 返回的块至少 size 字节，实际大小是所属桶的大小类。
    void* Allocate(size_t size) {
        if (size == 0) {
            size = 1;
        }
        if (size > MaxBlock_) {
            return std::malloc(size);
        }

        const size_t index = GetIndex(size);
        Bucket& b = buckets_[index];

        std::lock_guard<std::mutex> lg(b.mutex_);
        if (b.p_head_ == nullptr) {
            ReFill(b, MinBlock_ << index);
        }

        // 摘头：p_head_ 指向的空闲块前 8 字节存着下一个空闲块。
        void* p = b.p_head_;
        b.p_head_ = *reinterpret_cast<void**>(b.p_head_);
        return p;
    }

    // size 必须是分配时的 size（或同一大小类），否则会还错桶。
    // p 为 nullptr 时安全返回。
    void Deallocate(void* p, size_t size) {
        if (p == nullptr) {
            return;
        }
        if (size > MaxBlock_) {
            std::free(p);
            return;
        }

        const size_t index = GetIndex(size);
        Bucket& b = buckets_[index];

        // 头插：把当前链表头写进块的前 8 字节，再让 p 成为新头。
        std::lock_guard<std::mutex> lg(b.mutex_);
        *reinterpret_cast<void**>(p) = b.p_head_;
        b.p_head_ = p;
    }

private:
    static constexpr size_t MaxBlock_ = 4096;
    static constexpr size_t MinBlock_ = 8;
    static constexpr size_t NumBucket_ = 10;   // 8 << 0 ... 8 << 9 = 4096
    static constexpr size_t ChunkSize_ = 4 * 1024 * 1024;

    struct Bucket {
        void* p_head_ = nullptr;     // 空闲链表头
        std::vector<void*> chunks_;  // 这个桶从系统申请的 chunk
        std::mutex mutex_;           // 保护上面两个字段
    };

    Bucket buckets_[NumBucket_];

    // 向系统申请一个 chunk，切成 block_size 大小的块，全部头插进空闲链表。
    // 调用方必须已持有 b.mutex_。
    void ReFill(Bucket& b, size_t block_size) {
        void* chunk = std::malloc(ChunkSize_);
        if (chunk == nullptr) {
            throw std::bad_alloc();
        }
        // TODO: push_back 抛异常时释放 chunk
        b.chunks_.push_back(chunk);

        char* begin = static_cast<char*>(chunk);
        char* end = begin + ChunkSize_;
        // 最后不足一块的尾部空间直接浪费，换取切分逻辑简单。
        for (char* p = begin; p + block_size <= end; p += block_size) {
            *reinterpret_cast<void**>(p) = b.p_head_;
            b.p_head_ = p;
        }
    }

    // 返回 8 << i；调用前保证 size <= MaxBlock_。
    // 例如 size=8 -> 0，size=9 -> 1，size=4096 -> 9。
    static int GetIndex(size_t size) {
        size_t index = 0;
        size_t compare_size = MinBlock_;
        while (compare_size < size) {
            compare_size <<= 1;
            ++index;
        }
        return index;
    }
};
