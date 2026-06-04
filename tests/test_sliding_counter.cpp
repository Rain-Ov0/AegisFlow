#include "aegisflow/feature/sliding_counter.hpp"

#include <cassert>
#include <iostream>
#include <stdexcept>

using aegisflow::feature::SlidingCounter;

// 测试单个添加
void test_single_add() {
    SlidingCounter<60> counter(1000);
    assert(counter.add(1000, 1000));
    assert(counter.sum(1000) == 1);
}

// 测试多个添加在相同窗口内
void test_multiple_adds_in_same_window() {
    SlidingCounter<60> counter(1000);
    for (int i = 0; i < 10; i ++ ) {
        assert(counter.add(1000 + i, 1000));
    }
    assert(counter.sum(1000) == 10);
}

// 测试1分钟窗口过期
void test_one_minute_window_expire() {
    SlidingCounter<60> counter(1000);
    assert(counter.add(0, 0));
    assert(counter.sum(59000) == 1);
    assert(counter.sum(60000) == 0);
}

// 测试5分钟窗口过期
void test_five_minute_window_expire() {
    SlidingCounter<60> counter(5000);
    assert(counter.add(0, 0));
    assert(counter.sum(295000) == 1);
    assert(counter.sum(300000) == 0);
}

// 测试1小时窗口过期
void test_one_hour_window_expire() {
    SlidingCounter<60> counter(60000);
    assert(counter.add(0, 0));
    assert(counter.sum(3540000) == 1);
    assert(counter.sum(3600000) == 0);
}

// 测试拒绝过期事件
void test_reject_expired_event() {
    SlidingCounter<60> counter(1000);
    assert(!counter.add(0, 60000));
    assert(counter.sum(60000) == 0);
}

// 测试拒绝未来事件
void test_reject_future_event() {
    SlidingCounter<60> counter(1000);
    assert(!counter.add(2000, 1000));
    assert(counter.sum(1000) == 0);
}

// 测试delta
void test_delta() {
    SlidingCounter<60> counter(1000);
    assert(counter.add(1000, 1000, 5));
    assert(counter.sum(1000) == 5);
}

// 测试bucket重用
void test_bucket_reuse() {
    SlidingCounter<3> counter(1000);

    assert(counter.add(0, 0));
    assert(counter.add(1000, 1000));
    assert(counter.add(2000, 2000));
    assert(counter.sum(1000) == 3);

    assert(counter.add(3000, 3000));
    assert(counter.sum(3000) == 3);
}

void test_invalid_bucket_ms() {
    bool thrown = false;

    try {
        SlidingCounter<60> counter(0);
    } catch (const std::invalid_argument&) {
        thrown = true;
    }
    assert(thrown);
}

int main() {
    test_single_add();
    test_multiple_adds_in_same_window();
    test_one_minute_window_expire();
    test_five_minute_window_expire();
    test_one_hour_window_expire();
    test_reject_expired_event();
    test_reject_future_event();
    test_delta();
    test_bucket_reuse();
    test_invalid_bucket_ms();

    std::cout << "test_sliding_counter passed" << std::endl;
    return 0;
}