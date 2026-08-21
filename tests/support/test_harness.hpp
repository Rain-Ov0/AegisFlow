#pragma once

#include <exception>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace aegisflow::test {

inline void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

struct TestCase {
    std::string_view name;
    std::function<void()> run;
};

inline int runModule(
    const std::string_view module,
    const std::initializer_list<TestCase> tests
) {
    std::size_t failed = 0;
    for (const auto& test : tests) {
        try {
            test.run();
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failed;
            std::cerr << "[FAIL] " << test.name << ": " << error.what()
                      << '\n';
        } catch (...) {
            ++failed;
            std::cerr << "[FAIL] " << test.name << ": 未知异常\n";
        }
    }

    if (failed != 0) {
        std::cerr << module << ": " << failed << " 个测试失败\n";
        return 1;
    }
    std::cout << module << ": " << tests.size() << " 个测试全部通过\n";
    return 0;
}

}  // namespace aegisflow::test
