#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace aegisflow::feature {

class CountMinSketch {
public:
    CountMinSketch(size_t depth, size_t width) 
        : depth_(depth), 
        width_(width),
        table_(depth * width, 0) {
            if (depth == 0) {
                throw std::invalid_argument("depth must be greater than 0");
            }

            if (width == 0) {
                throw std::invalid_argument("width must be greater than 0");
            }

            seeds_.resize(depth_);
            uint64_t seed = 1469598103934665603ULL;
            for (size_t i = 0; i < depth_; ++ i ) {
                seeds_.push_back(seed + i * 0x9e3779b97f4a7c15ULL);
            }
        }

void add(std::string_view key, uint32_t delta = 1) {
    for (size_t row = 0; row < depth_; ++ row) {
        const size_t col = hash(key, seeds_[row]) % width_;
        table_[row * width_ + col] += delta;
    }
}

[[nodiscard]] uint64_t estimate(std::string_view key) const {
    uint64_t result = std::numeric_limits<uint64_t>::max();

    for (size_t row = 0; row < depth_; ++ row ) {
        const size_t col = hash(key, seeds_[row]) % width_;
        result = std::min(result, table_[row * width_ + col]);
    }

    return result;
}

void reset() {
    std::fill(table_.begin(), table_.end(), 0);
}


private:
    [[nodiscard]] uint64_t hash(std::string_view key, uint64_t seed) const {
        uint64_t h = seed;

        for (unsigned char c : key) {
            h ^= c;
            h *= 1099511628211ULL;
        }

        h ^= h >> 33;
        h *= 0xff51afd7ed558ccdULL;
        h ^= h >> 33;
        h *= 0xc4ceb9fe1a85ec53ULL;
        h ^= h >> 33;

        return h;
    }

private:
    size_t depth_ = 0;
    size_t width_ = 0;

    std::vector<uint64_t> table_;
    std::vector<uint64_t> seeds_;
};

}
