#pragma once
#include <vector>
#include <mutex>
#include <stdlib.h>


class MemoryPool{
public:

MemoryPool() = default;
~MemoryPool() {
    for(auto& b : buckets_) {
        for(void* c : b.chunks_) {
            std::free(c);
        }
    }
}

MemoryPool& operator=(const MemoryPool&) = delete;
MemoryPool(const MemoryPool&) = delete;


void* Allocate(size_t size) {
    if(size == 0) size =1;
    if(size > MaxBlock_) return std::malloc(size);

    const size_t index = GetIndex(size);
    Bucket& b = buckets_[index];
    // 开始操作共享数据，加锁
    std::lock_guard<std::mutex> lg(b.mutex_);
    if(b.p_head_ == nullptr)
        ReFill(b, MinBlock_ << index);
    // 从头拿
    void* p = b.p_head_;
    // 链表移除,更新头为之前的下一个
    b.p_head_ = *reinterpret_cast<void**>(b.p_head_);
    return p;
}

void Deallocate(void* p, size_t size) {
    if (p == nullptr) return;
    if(size > MaxBlock_){
        std::free(p);
        return;
    }

    const size_t index = GetIndex(size);
    Bucket& b = buckets_[index];

    // 开始操作共享数据，加锁
    std::lock_guard<std::mutex> lg(b.mutex_);

    // 本质是把内存块放回链表
    *reinterpret_cast<void**>(p) = b.p_head_;
    b.p_head_ = p;
}

private:
    static constexpr size_t MaxBlock_ = 4096;
    static constexpr size_t MinBlock_ = 8;
    static constexpr size_t NumBucket_ = 10;
    static constexpr size_t ChunkSize_ = 4 * 1024 * 1024;
    struct Bucket
    {
        // 链表头指针
        void* p_head_ = nullptr;
        // 还没使用的内存块的指针，存单
        std::vector<void*> chunks_;
        std::mutex mutex_;
    };
    // 十个规格的buckets
    Bucket buckets_[NumBucket_];

    // 当一个bucket为空时，重新根据大小填充并划分
    void ReFill(Bucket& b, size_t block_size) {
        void* chunk = std::malloc(ChunkSize_);
        if(chunk == nullptr) throw std::bad_alloc();
        // 新建的chunk放入存储
        b.chunks_.push_back(chunk);

        // 开始划分
        char* begin = static_cast<char*>(chunk);
        char* end = begin + ChunkSize_;
        for(char* p = begin; p + block_size <= end; p += block_size) {
            *reinterpret_cast<void**>(p) = b.p_head_;
            b.p_head_ = p;
        }
    }

    // 10为超过数组，大内存直接malloc
    static int GetIndex(size_t size) {
        size_t index = 0;
        size_t compare_size = MinBlock_;
        while(compare_size < size) {
            compare_size <<= 1;
            index++;
        }
        return index;
    }

};