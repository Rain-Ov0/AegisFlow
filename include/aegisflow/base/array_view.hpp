#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <type_traits>
#include <vector>

namespace aegisflow::base {

template <typename T>
class ArrayView {
    static_assert(std::is_object<T>::value,
                  "ArrayView element type must be an object type");

public:
    static constexpr std::size_t dynamic_extent =
        static_cast<std::size_t>(-1);

    using value_type = std::remove_cv_t<T>;
    using pointer = T*;
    using reference = T&;
    using iterator = pointer;

    constexpr ArrayView() noexcept = default;

    constexpr ArrayView(pointer data, const std::size_t size) noexcept
        : data_(data), size_(size) {
        assert(data != nullptr || size == 0);
    }

    template <std::size_t N>
    constexpr ArrayView(T (&values)[N]) noexcept
        : data_(values), size_(N) {}

    template <
        typename U,
        typename std::enable_if<
            std::is_convertible<U (*)[], T (*)[]>::value,
            int>::type = 0>
    constexpr ArrayView(const ArrayView<U>& other) noexcept
        : data_(other.data()), size_(other.size()) {}

    template <
        typename U,
        std::size_t N,
        typename std::enable_if<
            std::is_convertible<U (*)[], T (*)[]>::value,
            int>::type = 0>
    constexpr ArrayView(std::array<U, N>& values) noexcept
        : data_(values.data()), size_(values.size()) {}

    template <
        typename U,
        std::size_t N,
        typename std::enable_if<
            std::is_convertible<const U (*)[], T (*)[]>::value,
            int>::type = 0>
    constexpr ArrayView(const std::array<U, N>& values) noexcept
        : data_(values.data()), size_(values.size()) {}

    template <typename U, std::size_t N>
    ArrayView(std::array<U, N>&&) = delete;

    template <typename U, std::size_t N>
    ArrayView(const std::array<U, N>&&) = delete;

    template <
        typename U,
        typename Allocator,
        typename std::enable_if<
            std::is_convertible<U (*)[], T (*)[]>::value,
            int>::type = 0>
    constexpr ArrayView(std::vector<U, Allocator>& values) noexcept
        : data_(values.data()), size_(values.size()) {}

    template <
        typename U,
        typename Allocator,
        typename std::enable_if<
            std::is_convertible<const U (*)[], T (*)[]>::value,
            int>::type = 0>
    constexpr ArrayView(const std::vector<U, Allocator>& values) noexcept
        : data_(values.data()), size_(values.size()) {}

    template <typename U, typename Allocator>
    ArrayView(std::vector<U, Allocator>&&) = delete;

    template <typename U, typename Allocator>
    ArrayView(const std::vector<U, Allocator>&&) = delete;

    [[nodiscard]] constexpr pointer data() const noexcept {
        return data_;
    }

    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] constexpr bool empty() const noexcept {
        return size_ == 0;
    }

    [[nodiscard]] constexpr iterator begin() const noexcept {
        return data_;
    }

    [[nodiscard]] constexpr iterator end() const noexcept {
        return data_ == nullptr ? nullptr : data_ + size_;
    }

    [[nodiscard]] constexpr reference operator[](
        const std::size_t index
    ) const noexcept {
        assert(index < size_);
        return data_[index];
    }

    [[nodiscard]] constexpr ArrayView first(
        const std::size_t count
    ) const noexcept {
        assert(count <= size_);
        return ArrayView(data_, count);
    }

    [[nodiscard]] constexpr ArrayView subview(
        const std::size_t offset,
        const std::size_t count = dynamic_extent
    ) const noexcept {
        assert(offset <= size_);
        assert(count == dynamic_extent || count <= size_ - offset);
        const auto subview_size =
            count == dynamic_extent ? size_ - offset : count;
        return ArrayView(offset == 0 ? data_ : data_ + offset, subview_size);
    }

private:
    pointer data_ = nullptr;
    std::size_t size_ = 0;
};

}  // namespace aegisflow::base
