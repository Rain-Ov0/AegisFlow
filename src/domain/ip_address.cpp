#include "aegisflow/domain/ip_address.hpp"

#include <arpa/inet.h>

#include <algorithm>
#include <array>
#include <string>
#include <utility>

namespace aegisflow::domain {
namespace {

bool containsNull(const std::string_view value) noexcept {
    return value.find('\0') != std::string_view::npos;
}

bool isIpv4MappedIpv6(
    const std::array<std::uint8_t, 16>& bytes
) noexcept {
    return std::all_of(bytes.begin(), bytes.begin() + 10,
                       [](const std::uint8_t byte) { return byte == 0; }) &&
           bytes[10] == 0xff && bytes[11] == 0xff;
}

}  // 命名空间

IpAddress::IpAddress(
    std::array<std::uint8_t, 16> bytes,
    const std::size_t byte_count,
    std::string canonical_text
)
    : bytes_(bytes),
      byte_count_(byte_count),
      canonical_text_(std::move(canonical_text)) {}

std::optional<IpAddress> IpAddress::parse(const std::string_view text) {
    if (text.empty() || containsNull(text)) {
        return std::nullopt;
    }

    const std::string owned(text);
    std::array<std::uint8_t, 16> bytes{};
    if (::inet_pton(AF_INET, owned.c_str(), bytes.data()) == 1) {
        return fromBytes({bytes.data(), 4});
    }
    if (::inet_pton(AF_INET6, owned.c_str(), bytes.data()) == 1) {
        return fromBytes(bytes);
    }
    return std::nullopt;
}

std::optional<IpAddress> IpAddress::fromBytes(
    const base::ArrayView<const std::uint8_t> bytes
) {
    if (bytes.size() != 4 && bytes.size() != 16) {
        return std::nullopt;
    }

    std::array<std::uint8_t, 16> normalized{};
    std::size_t byte_count = bytes.size();
    std::copy(bytes.begin(), bytes.end(), normalized.begin());

    // IPv4-mapped IPv6 是 IPv4 端点的兼容表示。将它收敛为 4 字节
    // IPv4，防止 192.0.2.1 和 ::ffff:192.0.2.1 成为两个黑名单 key。
    if (byte_count == 16 && isIpv4MappedIpv6(normalized)) {
        std::copy(normalized.begin() + 12, normalized.end(), normalized.begin());
        std::fill(normalized.begin() + 4, normalized.end(), 0);
        byte_count = 4;
    }

    std::array<char, INET6_ADDRSTRLEN> text{};
    const int family = byte_count == 4 ? AF_INET : AF_INET6;
    if (::inet_ntop(family, normalized.data(), text.data(), text.size()) ==
        nullptr) {
        return std::nullopt;
    }

    return IpAddress(normalized, byte_count, std::string(text.data()));
}

}  // 命名空间 aegisflow::domain
