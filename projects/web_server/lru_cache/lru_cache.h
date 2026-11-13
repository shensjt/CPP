#pragma once

#include <cstddef>
#include <iterator>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

// 线程安全的 LRU：list 维护访问顺序，unordered_map 做 O(1) 查找。
//
// 结构：
//   items_ 头部 = 最新访问，尾部 = 最久未访问
//   index_  key -> list 迭代器，支持 O(1) splice/erase
//   cost    由调用方给出（文件缓存里是字节数），按总 cost 淘汰
//
// 语义：
// - Get 命中时把节点 splice 到头部；
// - Put 已存在则更新 value/cost 并移到头部；不存在则头插；
// - used_ > capacity_ 时从尾部淘汰；
// - 单项 cost > capacity 时不缓存，并移除旧值；
// - 所有公开方法都加锁，可被多个 worker 并发调用。
template <class Key, class Value>
class LruCache {
public:
    explicit LruCache(std::size_t capacity) : capacity_(capacity) {}

    LruCache(const LruCache&) = delete;
    LruCache& operator=(const LruCache&) = delete;

    bool Get(const Key& key, Value& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = index_.find(key);
        if (it == index_.end()) return false;

        // 命中：移到链表头，表示最近使用。
        items_.splice(items_.begin(), items_, it->second);
        value = it->second->second.value;
        return true;
    }

    void Put(const Key& key, const Value& value, std::size_t cost) {
        std::lock_guard<std::mutex> lock(mutex_);

        // 单项就超过总容量：不缓存，同时把可能存在的旧值删掉。
        if (capacity_ == 0 || cost > capacity_) {
            RemoveLocked(key);
            return;
        }

        auto it = index_.find(key);
        if (it != index_.end()) {
            // 已存在：更新 value/cost，并移到头部。
            used_ -= it->second->second.cost;
            it->second->second.value = value;
            it->second->second.cost = cost;
            used_ += cost;
            items_.splice(items_.begin(), items_, it->second);
        } else {
            // 新项：头插，并记录迭代器。
            items_.push_front({key, Entry{value, cost}});
            index_[key] = items_.begin();
            used_ += cost;
        }

        // 超容量：从尾部淘汰最久未访问的项。
        while (used_ > capacity_ && !items_.empty()) {
            auto last = std::prev(items_.end());
            used_ -= last->second.cost;
            index_.erase(last->first);
            items_.erase(last);
        }
    }

    std::size_t Size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return items_.size();
    }

    std::size_t UsedBytes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return used_;
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        items_.clear();
        index_.clear();
        used_ = 0;
    }

private:
    struct Entry {
        Value value;
        std::size_t cost;
    };

    using Item = std::pair<Key, Entry>;
    using Iterator = typename std::list<Item>::iterator;

    // 调用方必须已持有 mutex_。
    void RemoveLocked(const Key& key) {
        auto it = index_.find(key);
        if (it == index_.end()) return;
        used_ -= it->second->second.cost;
        items_.erase(it->second);
        index_.erase(it);
    }

    mutable std::mutex mutex_;
    std::list<Item> items_;  // 头部最新
    std::unordered_map<Key, Iterator> index_;
    std::size_t capacity_;
    std::size_t used_ = 0;
};
