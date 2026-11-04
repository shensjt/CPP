#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <random>
#include <type_traits>
#include <utility>
#include <vector>

// 跳表：有序集合，key 唯一，按 Compare 升序排列。
// 线程安全由调用方负责（KVStore 用 shared_mutex 保护）。
// Key 需要可比较、可移动、可默认构造（头节点用 Key{}）。
//
// 与 Redis 的对应关系：
//   kMaxLevel = 32，晋升概率 1/4，和 ZSKIPLIST_MAXLEVEL / ZSKIPLIST_P 一致。
//   Redis 节点还存 span 用于 O(logN) 排名；这里没做，ZRANGE 按名次遍历底层。
template <class Key, class Compare = std::less<Key>>
class SkipList {
public:
    static constexpr int kMaxLevel = 32;
    static constexpr std::uint32_t kBranching = 4;  // 晋升概率 1/4

    static_assert(std::is_default_constructible_v<Key>,
                  "SkipList 头节点需要默认构造 Key");

    explicit SkipList(Compare comp = Compare(), std::uint32_t seed = 0)
        : comp_(std::move(comp)),
          rng_(seed != 0 ? seed
                         : static_cast<std::uint32_t>(std::random_device{}())) {
        head_ = CreateNode(Key{}, kMaxLevel);
    }

    ~SkipList() {
        Node* node = head_->forward[0];
        while (node != nullptr) {
            Node* next = node->forward[0];
            delete node;
            node = next;
        }
        delete head_;
    }

    SkipList(const SkipList&) = delete;
    SkipList& operator=(const SkipList&) = delete;

    // 插入成功返回 true；key 已存在返回 false，不覆盖。
    bool Insert(Key key) {
        std::array<Node*, kMaxLevel> update{};
        Node* next = FindPredecessors(key, update);
        if (next != nullptr && Equal(next->key, key)) {
            return false;
        }

        const int level = RandomLevel();
        if (level > level_) {
            for (int i = level_; i < level; ++i) {
                update[static_cast<std::size_t>(i)] = head_;
            }
            level_ = level;
        }

        Node* node = CreateNode(std::move(key), level);
        for (int i = 0; i < level; ++i) {
            const std::size_t idx = static_cast<std::size_t>(i);
            node->forward[idx] = update[idx]->forward[idx];
            update[idx]->forward[idx] = node;
        }
        ++size_;
        return true;
    }

    // 找到返回节点地址；节点在下次修改前有效。
    const Key* Find(const Key& key) const {
        Node* node = LowerBoundNode(key);
        if (node != nullptr && Equal(node->key, key)) {
            return &node->key;
        }
        return nullptr;
    }

    bool Contains(const Key& key) const { return Find(key) != nullptr; }

    bool Erase(const Key& key) {
        std::array<Node*, kMaxLevel> update{};
        Node* node = FindPredecessors(key, update);
        if (node == nullptr || !Equal(node->key, key)) {
            return false;
        }

        // 只断开真正指向 node 的层；更高层本来就不指向它。
        for (int i = 0; i < level_; ++i) {
            const std::size_t idx = static_cast<std::size_t>(i);
            if (update[idx]->forward[idx] != node) {
                break;
            }
            update[idx]->forward[idx] = node->forward[idx];
        }

        delete node;
        // 最高层空了就把层高收回来，Level() 才不会虚高。
        while (level_ > 1 &&
               head_->forward[static_cast<std::size_t>(level_ - 1)] == nullptr) {
            --level_;
        }
        --size_;
        return true;
    }

    // 升序遍历全部节点；fn 返回 false 提前结束。
    template <class Fn>
    void ForEach(Fn&& fn) const {
        for (Node* node = head_->forward[0]; node != nullptr;
             node = node->forward[0]) {
            if (!fn(node->key)) {
                return;
            }
        }
    }

    // 遍历 [begin, end)，左闭右开；fn 返回 false 提前结束。
    template <class Fn>
    void Range(const Key& begin, const Key& end, Fn&& fn) const {
        for (Node* node = LowerBoundNode(begin);
             node != nullptr && comp_(node->key, end); node = node->forward[0]) {
            if (!fn(node->key)) {
                return;
            }
        }
    }

    // 从第一个 >= begin 的节点开始遍历，结束条件交给 fn。
    // ZRANGEBYSCORE 的 max 是闭区间，用这个接口比构造 end 更自然。
    template <class Fn>
    void RangeFrom(const Key& begin, Fn&& fn) const {
        for (Node* node = LowerBoundNode(begin); node != nullptr;
             node = node->forward[0]) {
            if (!fn(node->key)) {
                return;
            }
        }
    }

    std::size_t Size() const { return size_; }
    int Level() const { return level_; }
    bool Empty() const { return size_ == 0; }

private:
    struct Node {
        Key key;
        std::vector<Node*> forward;  // 长度等于节点层高

        Node(Key k, int level)
            : key(std::move(k)),
              forward(static_cast<std::size_t>(level), nullptr) {}
    };

    static Node* CreateNode(Key key, int level) {
        return new Node(std::move(key), level);
    }

    bool Equal(const Key& a, const Key& b) const {
        // 不依赖 operator==，自定义比较器也能正确判等。
        return !comp_(a, b) && !comp_(b, a);
    }

    // 返回第 0 层第一个 >= key 的节点；update[i] 记录第 i 层最后一个 < key 的节点。
    Node* FindPredecessors(const Key& key,
                           std::array<Node*, kMaxLevel>& update) const {
        Node* node = head_;
        for (int i = level_ - 1; i >= 0; --i) {
            const std::size_t idx = static_cast<std::size_t>(i);
            while (node->forward[idx] != nullptr &&
                   comp_(node->forward[idx]->key, key)) {
                node = node->forward[idx];
            }
            update[idx] = node;
        }
        return node->forward[0];
    }

    Node* LowerBoundNode(const Key& key) const {
        Node* node = head_;
        for (int i = level_ - 1; i >= 0; --i) {
            const std::size_t idx = static_cast<std::size_t>(i);
            while (node->forward[idx] != nullptr &&
                   comp_(node->forward[idx]->key, key)) {
                node = node->forward[idx];
            }
        }
        return node->forward[0];
    }

    int RandomLevel() {
        int level = 1;
        while (level < kMaxLevel && (rng_() & (kBranching - 1)) == 0) {
            ++level;
        }
        return level;
    }

    Compare comp_;
    std::mt19937 rng_;
    Node* head_ = nullptr;
    int level_ = 1;
    std::size_t size_ = 0;
};
