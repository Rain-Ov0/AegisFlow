#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace aegisflow::cache {

class BloomFilter {

public:
    BloomFilter(size_t bit_count, size_t hash_count)
        : bit_count_(bit_count),
          hash_count_(hash_count),
          bits_(wordCount(bit_count), 0) {
        if (hash_count == 0) {
            throw std::invalid_argument("hash_count must be greater than 0");
        }

        seeds_.reserve(hash_count_);

        constexpr uint64_t kSeedBase = 1469598103934665603ULL;
        constexpr uint64_t kSeedStep = 0x9e3779b97f4a7c15ULL;

        for (size_t i = 0; i < hash_count_; ++ i) {
            seeds_.push_back(kSeedBase + i * kSeedStep);
        }
    }

    void add(std::string_view key) {
        for (uint64_t seed : seeds_) {
            setBit(hash(key, seed) % bit_count_);
        }
    }

    [[nodiscard]] bool possiblyContains(std::string_view key) const {
        for (uint64_t seed : seeds_) {
            if (!testBit(hash(key, seed) % bit_count_)) {
                return false;
            }
        }

        return true;
    }

    void reset() {
        std::fill(bits_.begin(), bits_.end(), 0);
    }

    [[nodiscard]] size_t bitCount() const {
        return bit_count_;
    }

    [[nodiscard]] size_t hashCount() const {
        return hash_count_;
    }

private:
    static constexpr size_t kBitsPerWord = 64;

    static size_t wordCount(size_t bit_count) {
        if (bit_count == 0) {
            throw std::invalid_argument("bit_count must be greater than 0");
        }
        return (bit_count + kBitsPerWord - 1) / kBitsPerWord;
    }

    void setBit(size_t bit_index) {
        bits_[bit_index / kBitsPerWord] |=
            1ULL << (bit_index % kBitsPerWord);
    }

    [[nodiscard]] bool testBit(size_t bit_index) const {
        return (bits_[bit_index / kBitsPerWord] &
            (1ULL << (bit_index % kBitsPerWord))) != 0;
    }

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
    size_t bit_count_ = 0;
    size_t hash_count_ = 0;

    std::vector<uint64_t> bits_;
    std::vector<uint64_t> seeds_;
};

} // namespace aegisflow::cache
