#include "store/kv_store.h"

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

KvStore::Status KvStore::Set(const std::string& key, std::string value) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const auto it = data_.find(key);
    if (it == data_.end()) {
        Entry entry;
        entry.type = Entry::Type::kString;
        entry.str = std::move(value);
        data_.emplace(key, std::move(entry));
        return Status::kOk;
    }
    if (it->second.type != Entry::Type::kString) {
        return Status::kWrongType;
    }
    it->second.str = std::move(value);
    return Status::kOk;
}

KvStore::Status KvStore::Get(const std::string& key, std::string* value) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const Entry* entry = Find(key);
    if (entry == nullptr) {
        return Status::kNotFound;
    }
    if (entry->type != Entry::Type::kString) {
        return Status::kWrongType;
    }
    *value = entry->str;
    return Status::kOk;
}

bool KvStore::Del(const std::string& key) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    return data_.erase(key) > 0;
}

bool KvStore::Exists(const std::string& key) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return data_.find(key) != data_.end();
}

std::size_t KvStore::Size() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return data_.size();
}

KvStore::Status KvStore::ZAdd(
    const std::string& key,
    const std::vector<std::pair<double, std::string>>& items,
    std::size_t* added) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    Status status = Status::kOk;
    Entry* entry = FindOrCreateZSet(key, &status);
    if (entry == nullptr) {
        return status;
    }

    std::size_t count = 0;
    for (const auto& item : items) {
        if (entry->zset->Add(item.first, item.second)) {
            ++count;
        }
    }
    *added = count;
    return Status::kOk;
}

KvStore::Status KvStore::ZScore(const std::string& key,
                                const std::string& member,
                                double* score) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const Entry* entry = Find(key);
    if (entry == nullptr) {
        return Status::kNotFound;
    }
    if (entry->type != Entry::Type::kZSet) {
        return Status::kWrongType;
    }

    const auto value = entry->zset->Score(member);
    if (!value.has_value()) {
        return Status::kNotFound;
    }
    *score = *value;
    return Status::kOk;
}

KvStore::Status KvStore::ZRem(const std::string& key,
                              const std::vector<std::string>& members,
                              std::size_t* removed) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const auto it = data_.find(key);
    if (it == data_.end()) {
        return Status::kNotFound;
    }
    if (it->second.type != Entry::Type::kZSet) {
        return Status::kWrongType;
    }

    std::size_t count = 0;
    for (const auto& member : members) {
        if (it->second.zset->Remove(member)) {
            ++count;
        }
    }
    *removed = count;
    return Status::kOk;
}

KvStore::Status KvStore::ZRangeByRank(const std::string& key, long long start,
                                      long long stop,
                                      std::vector<std::string>* out) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    out->clear();
    const Entry* entry = Find(key);
    if (entry == nullptr) {
        return Status::kNotFound;
    }
    if (entry->type != Entry::Type::kZSet) {
        return Status::kWrongType;
    }
    entry->zset->RangeByRank(start, stop, *out);
    return Status::kOk;
}

KvStore::Status KvStore::ZRangeByScore(
    const std::string& key, double min, bool min_exclusive, double max,
    bool max_exclusive, std::vector<std::string>* out) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    out->clear();
    const Entry* entry = Find(key);
    if (entry == nullptr) {
        return Status::kNotFound;
    }
    if (entry->type != Entry::Type::kZSet) {
        return Status::kWrongType;
    }
    entry->zset->RangeByScore(min, min_exclusive, max, max_exclusive, *out);
    return Status::kOk;
}

KvStore::Entry* KvStore::FindOrCreateZSet(const std::string& key,
                                          Status* status) {
    const auto it = data_.find(key);
    if (it != data_.end()) {
        if (it->second.type != Entry::Type::kZSet) {
            *status = Status::kWrongType;
            return nullptr;
        }
        *status = Status::kOk;
        return &it->second;
    }

    Entry entry;
    entry.type = Entry::Type::kZSet;
    entry.zset = std::make_unique<ZSet>();
    const auto result = data_.emplace(key, std::move(entry));
    *status = Status::kOk;
    return &result.first->second;
}

const KvStore::Entry* KvStore::Find(const std::string& key) const {
    const auto it = data_.find(key);
    return it == data_.end() ? nullptr : &it->second;
}
