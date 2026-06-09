#include "aegisflow/feature/space_saving_topk.hpp"

#include <cassert>
#include <iostream>
#include <stdexcept>
#include <string>

using aegisflow::feature::SpaceSavingTopK;

// 测试空状态
void test_empty() {
    SpaceSavingTopK topk(3);
    assert(!topk.contains("never_seen"));
    assert(topk.estimate("never_seen") == 0);
    assert(topk.topk(10).empty());
}

// 测试单个高频键
void test_single_key_count() {
    SpaceSavingTopK topk(3);

    for (int i = 0; i < 100; ++ i) {
        topk.update("attack_ip");
    }

    assert(topk.contains("attack_ip"));
    assert(topk.estimate("attack_ip") == 100);
    
    const auto items = topk.topk(1);
    assert(items.size() == 1);
    assert(items[0].first == "attack_ip");
    assert(items[0].second == 100);
}

// 测试 delta 更新
void test_delta() {
    SpaceSavingTopK topk(3);

    topk.update("ip_1", 7);
    topk.update("ip_1", 3);

    assert(topk.estimate("ip_1") == 10);
}

// 测试 topk 按降序返回
void test_topk_descending_count() {
    SpaceSavingTopK topk(5);

    for (int i = 0; i < 30; ++ i ) {
        topk.update("A");
    }

    for (int i = 0; i < 10; ++ i ) {
        topk.update("B");
    }

    for (int i = 0; i < 20; ++ i ) {
        topk.update("C");
    }

    auto items = topk.topk(3);
    assert(items.size() == 3);
    assert(items[0].first == "A");
    assert(items[1].first == "C");
    assert(items[2].first == "B");
}

// 测试容量限制
void test_capacity_limit() {
    SpaceSavingTopK topk(3);

    for (int i = 0; i < 10; ++ i) {
        topk.update("ip_" + std::to_string(i));
    }

    assert(topk.topk(10).size() == 3);
}

// 测试高频 key 不会被低频 key 逐出
void test_high_frequency_key_not_evticted() {
    SpaceSavingTopK topk(3);

    for (int i = 0; i < 100; ++ i ) {
        topk.update("attack_ip");
    }

    topk.update("normal_a");
    topk.update("normal_b");

    for (int i = 0; i < 20; ++ i ) {
        topk.update("normal" + std::to_string(i));
    }

    assert(topk.contains("attack_ip"));
    assert(topk.estimate("attack_ip") == 100);
    assert(topk.topk(1)[0].first == "attack_ip");
}

// 测试替换使用 min_count + delta
void test_replace_with_min_count_plus_delta() {
    SpaceSavingTopK topk(2);

    topk.update("A", 5);
    topk.update("B", 3);
    topk.update("C", 2);

    assert(topk.contains("A"));
    assert(!topk.contains("B"));
    assert(topk.contains("C"));
    assert(topk.estimate("C") == 5);
}

// 测试 delta 为 0 时的情况
void test_zero_delta() {
    SpaceSavingTopK topk(3);

    topk.update("A", 0);
    assert(!topk.contains("A"));
    assert(topk.topk(1).empty());
}

void test_invalid_capacity() {
    bool thrown = false;

    try {
        SpaceSavingTopK topk(0);
    } catch (const std::invalid_argument&) {
        thrown = true;
    }

    assert(thrown);
}

int main() {
    test_empty();
    test_single_key_count();
    test_delta();
    test_topk_descending_count();
    test_capacity_limit();
    test_high_frequency_key_not_evticted();
    test_replace_with_min_count_plus_delta();
    test_zero_delta();
    test_invalid_capacity();
    std::cout << "test_space_saving_topk passed" << std::endl;
    return 0;
}
