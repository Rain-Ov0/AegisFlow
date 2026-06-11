#include "aegisflow/cache/bloom_filter.hpp"

#include <cassert>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using aegisflow::cache::BloomFilter;

// 测试黑名单实体 key 加入后可以命中
void test_blacklist_entity_keys_possibly_contains() {
    BloomFilter filter(1 << 20, 7);

    filter.add("user:u_black_001");
    filter.add("ip:10.0.0.9");
    filter.add("device:dev_black_001");

    assert(filter.possiblyContains("user:u_black_001"));
    assert(filter.possiblyContains("ip:10.0.0.9"));
    assert(filter.possiblyContains("device:dev_black_001"));
}

// 测试未加入 key 大概率返回 false
void test_non_inserted_key_is_probably_absent() {
    BloomFilter filter(1 << 20, 7);

    filter.add("user:u_black_001");

    assert(!filter.possiblyContains("user:u_normal_001"));
    assert(!filter.possiblyContains("ip:10.0.0.8"));
    assert(!filter.possiblyContains("device:dev_normal_001"));
}

// 测试大量 key 不会出现 false negative
void test_many_keys_have_no_false_negative() {
    BloomFilter filter(1 << 20, 7);
    std::vector<std::string> keys;

    for (int i = 0; i < 1000; ++ i) {
        keys.push_back("user:u_black_" + std::to_string(i));
    }

    for (const auto& key : keys) {
        filter.add(key);
    }

    for (const auto& key : keys) {
        assert(filter.possiblyContains(key));
    }
}

// 测试重复添加同一个 key 仍能稳定命中
void test_duplicate_add() {
    BloomFilter filter(4096, 5);

    filter.add("user:u_black_001");
    filter.add("user:u_black_001");

    assert(filter.possiblyContains("user:u_black_001"));
}

// 测试 reset 清空状态
void test_reset() {
    BloomFilter filter(4096, 5);

    filter.add("user:u_black_001");
    assert(filter.possiblyContains("user:u_black_001"));

    filter.reset();
    assert(!filter.possiblyContains("user:u_black_001"));
}

// 测试无效参数
void test_invalid_argument_constructor() {
    bool bit_count_thrown = false;
    bool hash_count_thrown = false;

    try {
        BloomFilter filter(0, 7);
    } catch (const std::invalid_argument&) {
        bit_count_thrown = true;
    }

    try {
        BloomFilter filter(1024, 0);
    } catch (const std::invalid_argument&) {
        hash_count_thrown = true;
    }

    assert(bit_count_thrown);
    assert(hash_count_thrown);
}

int main() {
    test_blacklist_entity_keys_possibly_contains();
    test_non_inserted_key_is_probably_absent();
    test_many_keys_have_no_false_negative();
    test_duplicate_add();
    test_reset();
    test_invalid_argument_constructor();

    std::cout << "test_bloom_filter passed" << std::endl;
    return 0;
}