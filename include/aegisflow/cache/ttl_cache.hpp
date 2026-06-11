#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <list>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace aegisflow::cache {

template <typename K, typename V, typename Clock = std::chrono::steady_clock>
class TtlLruCache {
public:
    explicit TtlLruCache(size_t capacity)
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

        const auto now = Clock::now();
        if (isExpired(*it->second, now)) {
            list_.erase(it->second);
            map_.erase(it);
            return false;
        }

        list_.splice(list_.begin(), list_, it->second);
        value = it->second->value;
        return true;
    }

    void put(const K& key, const V& value, uint64_t ttl_ms) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (ttl_ms == 0) {
            eraseLocked(key);
            return;
        }

        const auto expires_at = expireTime(ttl_ms);
        auto it = map_.find(key);

        if (it != map_.end()) {
            it->second->value = value;
            it->second->expires_at = expires_at;
            list_.splice(list_.begin(), list_, it->second);
            return;
        }

        if (list_.size() >= capacity_) {
            map_.erase(list_.back().key);
            list_.pop_back();
        }

        list_.push_front(Entry{key, value, expires_at});
        map_[key] = list_.begin();
    }

    bool erase(const K& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        return eraseLocked(key);
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        list_.clear();
        map_.clear();
    }

    size_t purgeExpired() {
        std::lock_guard<std::mutex> lock(mutex_);

        const auto now = Clock::now();
        size_t removed = 0;

        for (auto it = list_.begin(); it != list_.end();) {
            if (!isExpired(*it, now)) {
                ++it;
                continue;
            }

            map_.erase(it->key);
            it = list_.erase(it);
            ++removed;
        }

        return removed;
    }

    [[nodiscard]] size_t size() {
        purgeExpired();
        std::lock_guard<std::mutex> lock(mutex_);
        return list_.size();
    }

    [[nodiscard]] size_t capacity() const {
        return capacity_;
    }

private:
    using TimePoint = typename Clock::time_point;
    using ListIt = typename std::list<struct Entry>::iterator;

    struct Entry {
        K key;
        V value;
        TimePoint expires_at;
    };

    [[nodiscard]] TimePoint expireTime(uint64_t ttl_ms) const {
        return Clock::now() + 
            std::chrono::duration_cast<typename Clock::duration>(
                std::chrono::milliseconds(ttl_ms)
            );
    }

    [[nodiscard]] bool isExpired(const Entry& entry, TimePoint now) const {
        return entry.expires_at <= now;
    }

    bool eraseLocked(const K& key) {
        auto it = map_.find(key);
        if (it == map_.end()) {
            return false;
        }

        list_.erase(it->second);
        map_.erase(it);
        return true;
    }

private:
    size_t capacity_ = 0;
    std::list<Entry> list_;
    std::unordered_map<K, typename std::list<Entry>::iterator> map_;
    mutable std::mutex mutex_;
};    

template <typename K, typename V, typename Clock = std::chrono::steady_clock>
using TtlCache = TtlLruCache<K, V, Clock>;

} // namespace aegisflow::cache