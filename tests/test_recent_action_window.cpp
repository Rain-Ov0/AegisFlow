#include "aegisflow/feature/recent_action_window.hpp"

#include <cassert>
#include <iostream>

using aegisflow::feature::RecentAction;
using aegisflow::feature::RecentActionWindow;

// 测试空窗口
void test_empty_window() {
    RecentActionWindow<20> window;
    assert(window.empty());
    assert(window.size() == 0);
    assert(window.capacity() == 20);
    assert(window.list().empty());
}

// 测试单个推送
void test_single_push() {
    RecentActionWindow<20> window;

    window.push({
        aegisflow::v1::LOGIN,
        aegisflow::v1::SUCCESS,
        1000,
    });

    const auto actions = window.list();
    assert(actions.size() == 1);
    assert(actions[0].type == aegisflow::v1::LOGIN);
    assert(actions[0].result == aegisflow::v1::SUCCESS);
    assert(actions[0].timestamp_ms == 1000);
}

// 测试在窗口未满时保持顺序
void test_keep_order_before_full() {
    RecentActionWindow<20> window;

    for (uint64_t i = 0; i < 5; ++ i ) {
        window.push({
            aegisflow::v1::LOGIN,
            aegisflow::v1::SUCCESS,
            i,
        });
    }

    const auto actions = window.list();
    assert(actions.size() == 5);
    for (uint64_t i = 0; i < 5; ++ i ) {
        assert(actions[i].timestamp_ms == i);
    }
}

// 测试在窗口已满时仅保留最新动作
void test_only_keep_latest_actions() {
    RecentActionWindow<20> window;

    for (uint64_t i = 0; i < 25; ++ i ) {
        window.push({
            aegisflow::v1::LOGIN,
            aegisflow::v1::SUCCESS,
            i,
        });
    }

    const auto actions = window.list();

    assert(actions.size() == 20);

    for (uint64_t i = 0; i < 20; ++ i ) {
        assert(actions[i].timestamp_ms == i + 5);
    }
}

// 测试在窗口已满时，最新动作在窗口头
void test_warp_order() {
    RecentActionWindow<3> window;

    window.push({
        aegisflow::v1::LOGIN,
        aegisflow::v1::SUCCESS,
        1,
    });

    window.push({
        aegisflow::v1::PAY,
        aegisflow::v1::SUCCESS,
        2,
    });

    window.push({
        aegisflow::v1::CLICK,
        aegisflow::v1::SUCCESS,
        3,
    });

    window.push({
        aegisflow::v1::POST,
        aegisflow::v1::FAIL,
        4,
    });

    const auto actions = window.list();

    assert(actions.size() == 3);
    assert(actions[0].timestamp_ms == 2);
    assert(actions[1].timestamp_ms == 3);
    assert(actions[2].timestamp_ms == 4);

    assert(actions[0].type == aegisflow::v1::PAY);
    assert(actions[1].type == aegisflow::v1::CLICK);
    assert(actions[2].type == aegisflow::v1::POST);
    assert(actions[2].result == aegisflow::v1::FAIL);
}

int main() {
    test_empty_window();
    test_single_push();
    test_keep_order_before_full();
    test_only_keep_latest_actions();
    test_warp_order();

    std::cout << "test_recent_action_window passed" << std::endl;
    return 0;
}