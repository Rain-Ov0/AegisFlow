#pragma once

#include <cstddef>
#include <list>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace aegisflow::cache {

template <typename K, typename V>
class LruCache {
public:

    explicit LruCache(size_t capacity)
        : capacity_(capacity) {
        if (capacity == 0) {
            throw std::invalid_argument("capacity must be greater than 0");
        }
    }

    bool get(const K& key, V& value) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = map_.find(key);
        if (it == map_.end()) {
            return false;
        }

        list_.splice(list_.begin(), list_, it->second);
        value = it->second->second;
        return true;
    }

    void put(const K& key, const V& value) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = map_.find(key);
        if (it != map_.end()) {
            it->second->second = value;
            list_.splice(list_.begin(), list_, it->second);
            return;
        }

        if (list_.size() >= capacity_) {
            map_.erase(list_.back().first);
            list_.pop_back();
        }

        list_.emplace_front(key, value);
        map_[key] = list_.begin();
    }

    bool erase(const K& key) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = map_.find(key);
        if (it == map_.end()) {
            return false;
        }

        list_.erase(it->second);
        map_.erase(it);
        return true;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        list_.clear();
        map_.clear();
    }

    [[nodiscard]] size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return list_.size();
    }

    [[nodiscard]] size_t capacity() const {
        return capacity_;
    }

private:
    using ListIt = typename std::list<std::pair<K, V>>::iterator;

    size_t capacity_ = 0;
    std::list<std::pair<K, V>> list_;
    std::unordered_map<K, ListIt> map_;
    mutable std::mutex mutex_;
};

} // namespace aegisflow::cache
