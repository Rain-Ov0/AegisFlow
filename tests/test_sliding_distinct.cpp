#include "aegisflow/feature/sliding_distinct.hpp"

#include <cassert>
#include <iostream>
#include <stdexcept>

using aegisflow::feature::SlidingDistinct;

// 测试空状态
void test_empty() {
    SlidingDistinct distinct(60 * 1000, 1000, 100);
    assert(distinct.count(0) == 0);
    assert(!distinct.degraded());
}

// 测试单个成员
void test_single_member() {
    SlidingDistinct distinct(60 * 1000, 1000, 100);

    assert(distinct.add("u1", 1000, 1000));
    assert(distinct.count(1000) == 1);
}

// 测试重复成员仅计一次
void test_duplicate_member_count_once() {
    SlidingDistinct distinct(60 * 1000, 1000, 100);

    assert(distinct.add("u1", 1000, 1000));
    assert(distinct.add("u1", 1000, 1000));
    assert(distinct.add("u1", 1500, 1500));

    assert(distinct.count(1500) == 1);
}

// 测试多个成员
void test_multiple_members() {
    SlidingDistinct distinct(60 * 1000, 1000, 100);

    assert(distinct.add("u1", 1000, 1000));
    assert(distinct.add("u2", 1000, 1000));
    assert(distinct.add("u3", 1000, 1000));
    assert(distinct.count(1500) == 3);
}

// 测试成员刷新延长生命周期
void test_member_refresh_extends_lifetime() {
    SlidingDistinct distinct(3 * 1000, 1000, 100);

    assert(distinct.add("u1", 0, 0));
    assert(distinct.add("u1", 2000, 2000));
    assert(distinct.count(3000) == 1);
    assert(distinct.count(5000) == 0);
}

// 测试过期旧桶
void test_expired_old_bucket() {
    SlidingDistinct distinct(3 * 1000, 1000, 100);

    assert(distinct.add("u1", 0, 0));
    assert(distinct.count(2000) == 1);
    assert(distinct.count(3000) == 0);
}

// 测试拒绝过期事件
void test_rejcet_expired_event() {
    SlidingDistinct distinct(3 * 1000, 1000, 100);
    assert(!distinct.add("u1", 0, 3000));
    assert(distinct.count(3000) == 0);
}

// 测试拒绝特征事件
void test_reject_feature_event() {
    SlidingDistinct distinct(3 * 1000, 1000, 100);
    assert(!distinct.add("u1", 2000, 1000));
    assert(distinct.count(1000) == 0);
}

// 测试容量降级
void test_capacity_degraded() {
    SlidingDistinct distinct(60 * 1000, 1000, 2);

    assert(distinct.add("u1", 1000, 1000));
    assert(distinct.add("u2", 1000, 1000));
    assert(!distinct.add("u3", 1000, 1000));

    assert(distinct.count(1000) == 2);
    assert(distinct.degraded());
}

// 测试无效参数
void test_invalid_argument() {
    bool thrown = false;

    try {
        SlidingDistinct distinct(0, 1000, 100);
    } catch (const std::invalid_argument&) {
        thrown = true;
    }

    assert(thrown);
}

int main() {
    test_empty();
    test_single_member();
    test_duplicate_member_count_once();
    test_multiple_members();
    test_member_refresh_extends_lifetime();
    test_expired_old_bucket();
    test_rejcet_expired_event();
    test_reject_feature_event();
    test_capacity_degraded();
    test_invalid_argument();

    std::cout << "test_sliding_distinct passed" << std::endl;

    return 0;
}
