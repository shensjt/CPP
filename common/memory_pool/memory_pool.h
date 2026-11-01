#pragma once

#include <cstddef>
#include <cstdlib>
#include <mutex>
#include <new>
#include <vector>

// 固定大小分桶的内存池：8B–4KB，共 10 个桶。
// 每桶一条侵入式空闲链表 + 一把锁；chunk 只增不减，析构统一 free。
class MemoryPool {
public:
    MemoryPool() = default;

    ~MemoryPool() {
        for (auto& b : buckets_) {
            for (void* c : b.chunks_) {
                std::free(c);
            }
        }
    }

    MemoryPool& operator=(const MemoryPool&) = delete;
    MemoryPool(const MemoryPool&) = delete;

    // size 为 0 按 1 处理；超过 4KB 直接走系统 malloc。
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

        void* p = b.p_head_;
        b.p_head_ = *reinterpret_cast<void**>(b.p_head_);
        return p;
    }

    // size 必须是分配时的 size（或同一大小类），否则会还错桶。
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

        std::lock_guard<std::mutex> lg(b.mutex_);
        *reinterpret_cast<void**>(p) = b.p_head_;
        b.p_head_ = p;
    }

private:
    static constexpr size_t MaxBlock_ = 4096;
    static constexpr size_t MinBlock_ = 8;
    static constexpr size_t NumBucket_ = 10;
    static constexpr size_t ChunkSize_ = 4 * 1024 * 1024;

    struct Bucket {
        void* p_head_ = nullptr;     // 空闲链表头
        std::vector<void*> chunks_;  // 这个桶从系统申请的 chunk
        std::mutex mutex_;           // 保护上面两个字段
    };

    Bucket buckets_[NumBucket_];

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
        for (char* p = begin; p + block_size <= end; p += block_size) {
            *reinterpret_cast<void**>(p) = b.p_head_;
            b.p_head_ = p;
        }
    }

    // 返回 8 << i；调用前保证 size <= MaxBlock_。
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
