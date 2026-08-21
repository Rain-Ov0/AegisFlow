#pragma once

#include "aegisflow/base/array_view.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace aegisflow::domain {

// IP 在请求校验、缓存 key 和数据库边界共用同一个值对象。
// 对象同时保存规范文本和 4/16 字节网络序数据，调用者不能组出
// “文本和二进制不表示同一地址”的状态。
class IpAddress final {
public:
    [[nodiscard]] static std::optional<IpAddress> parse(
        std::string_view text
    );
    [[nodiscard]] static std::optional<IpAddress> fromBytes(
        base::ArrayView<const std::uint8_t> bytes
    );

    [[nodiscard]] const std::string& canonicalText() const noexcept {
        return canonical_text_;
    }
    [[nodiscard]] base::ArrayView<const std::uint8_t> bytes() const noexcept {
        return {bytes_.data(), byte_count_};
    }
    [[nodiscard]] bool isIpv4() const noexcept { return byte_count_ == 4; }

    bool operator==(const IpAddress& other) const {
        return bytes_ == other.bytes_ &&
               byte_count_ == other.byte_count_ &&
               canonical_text_ == other.canonical_text_;
    }

private:
    IpAddress(
        std::array<std::uint8_t, 16> bytes,
        std::size_t byte_count,
        std::string canonical_text
    );

    std::array<std::uint8_t, 16> bytes_{};
    std::size_t byte_count_ = 0;
    std::string canonical_text_;
};

}  // 命名空间 aegisflow::domain
