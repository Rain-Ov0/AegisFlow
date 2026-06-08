#include "aegisflow/feature/count_min_sketch.hpp"

#include <cassert>
#include <iostream>
#include <stdexcept>

using aegisflow::feature::CountMinSketch;

// 测试空键的估计值为0
void test_empty_key_estimate_is_zero() {
    CountMinSketch cms(4, 1024);
    assert(cms.estimate("never_seen") == 0);
}

// 测试单键计数
void test_single_key_count() {
    CountMinSketch cms(4, 1024);

    for (int i = 0; i < 10; ++ i) {
        cms.add("ip_login_fail:1.2.3.4");
    }

    assert(cms.estimate("ip_login_fail:1.2.3.4") >= 10);
}

// 测试delta函数
void test_delta() {
    CountMinSketch cms(4, 1024);

    cms.add("risk_key", 7);
    cms.add("risk_key", 3);

    assert(cms.estimate("risk_key") >= 10);
}

// 测试高频率键的估计值大于低频率键的估计值
void test_high_frequency_key_larger_than_low_frequency_key() {
    CountMinSketch cms(4, 4096);

    for (int i = 0; i < 100; ++ i ) {
        cms.add("attack_ip|login|LOGIN|FAIL");
    }

    for (int i = 0; i < 5; ++ i ) {
        cms.add("normal_ip|login|LOGIN|FAIL");
    }

    assert(
        cms.estimate("attack_ip|login|LOGIN|FAIL") > 
        cms.estimate("normal_ip|login|LOGIN|FAIL")
    );
}

// 测试重置函数
void test_reset() {
    CountMinSketch cms(4, 1024);
    
    cms.add("risk_key", 10);
    assert(cms.estimate("risk_key") >= 10);

    cms.reset();
    assert(cms.estimate("risk_key") == 0);
}

// 测试无效参数构造函数
void test_invalid_argument_constructor() {
    bool depth_thrown = false;
    bool width_thrown = false;

    try {
        CountMinSketch cms(0, 1024);
    } catch (const std::invalid_argument&) {
        depth_thrown = true;
    }

    try {
        CountMinSketch cms(4, 0);
    } catch (const std::invalid_argument&) {
        width_thrown = true;
    }

    assert(depth_thrown);
    assert(width_thrown);
}

int main() {
    test_empty_key_estimate_is_zero();
    test_single_key_count();
    test_delta();
    test_high_frequency_key_larger_than_low_frequency_key();
    test_reset();
    test_invalid_argument_constructor();

    std::cout << "test_count_min_sketch passed" << std::endl;
    return 0;
}