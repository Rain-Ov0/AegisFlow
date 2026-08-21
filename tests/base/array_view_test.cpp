#include "aegisflow/base/array_view.hpp"

#include "tests/support/test_harness.hpp"

#include <array>
#include <cstddef>
#include <initializer_list>
#include <numeric>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using aegisflow::base::ArrayView;
using aegisflow::test::require;

static_assert(std::is_constructible<ArrayView<const int>,
                                    ArrayView<int>>::value,
              "mutable view must convert to const view");
static_assert(!std::is_constructible<ArrayView<int>,
                                     ArrayView<const int>>::value,
              "const view must not convert to mutable view");
static_assert(!std::is_constructible<ArrayView<int>,
                                     const std::vector<int>&>::value,
              "const vector must not produce a mutable view");
static_assert(!std::is_constructible<ArrayView<int>,
                                     const std::array<int, 2>&>::value,
              "const array must not produce a mutable view");
static_assert(!std::is_constructible<ArrayView<const int>,
                                     std::vector<int>&&>::value,
              "temporary vector must not produce a view");
static_assert(!std::is_constructible<ArrayView<const int>,
                                     const std::vector<int>&&>::value,
              "const temporary vector must not produce a view");
static_assert(!std::is_constructible<ArrayView<const int>,
                                     std::array<int, 2>&&>::value,
              "temporary array must not produce a view");
static_assert(!std::is_constructible<ArrayView<const int>,
                                     std::initializer_list<int>>::value,
              "initializer_list must not produce a view");

void defaultAndPointerViewsExposeTheirRange() {
    const ArrayView<int> empty;
    require(empty.data() == nullptr, "默认 view 的 data 必须为空");
    require(empty.size() == 0 && empty.empty(), "默认 view 必须为空");
    require(empty.begin() == empty.end(), "空 view 的迭代区间必须为空");
    require(empty.first(0).begin() == empty.first(0).end(),
            "空 view 的 first(0) 必须安全");
    require(empty.subview(0).begin() == empty.subview(0).end(),
            "空 view 的 subview(0) 必须安全");

    int values[] = {2, 4, 6, 8};
    ArrayView<int> view(values, 4);
    require(view.data() == values && view.size() == 4,
            "pointer + size 必须保留原区间");
    view[1] = 5;
    require(values[1] == 5, "mutable view 必须写回原数组");
}

void arraysAndVectorsPreserveConstness() {
    int c_array[] = {1, 2, 3};
    ArrayView<int> c_array_view(c_array);
    require(c_array_view.size() == 3 && c_array_view[2] == 3,
            "C array 必须构造完整 view");

    std::array<int, 3> array = {4, 5, 6};
    ArrayView<int> array_view(array);
    array_view[0] = 7;
    require(array[0] == 7, "std::array 的 mutable view 必须可写");

    const std::array<int, 2> const_array = {8, 9};
    const ArrayView<const int> const_array_view(const_array);
    require(const_array_view.size() == 2 && const_array_view[1] == 9,
            "const std::array 必须生成只读 view");

    std::vector<int> vector = {10, 11, 12};
    ArrayView<int> vector_view(vector);
    vector_view[2] = 13;
    require(vector[2] == 13, "std::vector 的 mutable view 必须可写");

    const std::vector<int> const_vector = {14, 15};
    const ArrayView<const int> const_vector_view(const_vector);
    require(const_vector_view.size() == 2 && const_vector_view[0] == 14,
            "const std::vector 必须生成只读 view");

    const std::array<int, 0> empty_array{};
    const ArrayView<const int> empty_array_view(empty_array);
    require(empty_array_view.empty() &&
                empty_array_view.begin() == empty_array_view.end(),
            "零长度 std::array 必须生成安全空区间");
}

void mutableViewConvertsToReadOnlyView() {
    std::vector<int> values = {3, 6, 9};
    const ArrayView<int> mutable_view(values);
    const ArrayView<const int> const_view(mutable_view);
    require(const_view.data() == mutable_view.data() &&
                const_view.size() == mutable_view.size(),
            "mutable 到 const view 转换必须保留区间");
}

void iterationAndSlicingUseTheExpectedElements() {
    std::array<int, 5> values = {1, 2, 3, 4, 5};
    const ArrayView<int> view(values);
    require(std::accumulate(view.begin(), view.end(), 0) == 15,
            "begin/end 必须遍历全部元素");

    const auto first = view.first(3);
    require(first.size() == 3 && first[0] == 1 && first[2] == 3,
            "first 必须返回前缀");

    const auto middle = view.subview(1, 3);
    require(middle.size() == 3 && middle[0] == 2 && middle[2] == 4,
            "带 count 的 subview 必须返回指定区间");

    const auto suffix = view.subview(2);
    require(suffix.size() == 3 && suffix[0] == 3 && suffix[2] == 5,
            "不带 count 的 subview 必须返回余下区间");

    const auto tail = view.subview(view.size());
    require(tail.empty() && tail.begin() == tail.end() &&
                tail.data() == values.data() + values.size(),
            "尾部空 subview 必须保留正确边界");
}

}  // namespace

int main() {
    return aegisflow::test::runModule(
        "array_view",
        {
            {"默认与 pointer view", defaultAndPointerViewsExposeTheirRange},
            {"容器构造与 const 约束", arraysAndVectorsPreserveConstness},
            {"mutable 转只读", mutableViewConvertsToReadOnlyView},
            {"迭代与切片", iterationAndSlicingUseTheExpectedElements},
        }
    );
}
