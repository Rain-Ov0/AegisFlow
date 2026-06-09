#pragma once

#include <cstddef>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aegisflow::feature {

class SpaceSavingTopK {
public:
    explicit SpaceSavingTopK(size_t capacity)
        : capacity_(capacity) {
            if (capacity == 0) {
                throw std::invalid_argument("capacity must be greater than 0");
            }
        }

    void update(const std::string& key, uint64_t delta = 1) {
        if (delta == 0) {
            return ;
        }

        auto it = items_.find(key);
        if (it != items_.end()) {
            ordered_.erase(OrderedItem{it->second.count, it->first});
            it->second.count += delta;
            ordered_.insert(OrderedItem{it->second.count, it->first});
            return ;
        }

        if (items_.size() < capacity_) {
            items_.emplace(key, Item{delta, 0});
            ordered_.insert(OrderedItem{delta, key});
            return ;
        }

        auto min_it = ordered_.begin();
        const std::string victim_key = min_it->key;
        const uint64_t min_count = min_it->count;
        
        ordered_.erase(min_it);
        items_.erase(victim_key);

        const uint64_t new_count = min_count + delta;
        items_.emplace(key, Item{new_count, min_count});
        ordered_.insert(OrderedItem{new_count, key});
    }

    [[nodiscard]] uint64_t estimate(const std::string& key) const {
        auto it = items_.find(key);
        if (it == items_.end()) {
            return 0;
        }
        return it->second.count;
    }

    [[nodiscard]] bool contains(const std::string& key) const {
        return items_.find(key) != items_.end();
    }

    [[nodiscard]] std::vector<std::pair<std::string, uint64_t>> topk(size_t k) const {
        std::vector<std::pair<std::string, uint64_t>> result;
        result.reserve(k < items_.size() ? k : items_.size());

        for (auto it = ordered_.rbegin(); it != ordered_.rend() && result.size() < k; ++it) {
            result.emplace_back(it->key, it->count);
        }

        return result;
    }

private:
    struct Item {
        uint64_t count = 0;
        uint64_t error = 0;
    };

    struct OrderedItem {
        uint64_t count = 0;
        std::string key;

        bool operator<(const OrderedItem& other) const {
            if (count != other.count) {
                return count < other.count;
            }
            return key < other.key;
        }
    };


private:
    size_t capacity_ = 0;
    std::unordered_map<std::string, Item> items_;
    std::set<OrderedItem> ordered_;
};
} // namespace aegisflow::feature