#include "aegisflow/feature/sliding_counter.hpp"
#include "aegisflow/feature/sliding_distinct.hpp"

#include "tests/support/test_harness.hpp"

#include <stdexcept>

namespace {

using aegisflow::test::require;

void counterExpiresAtExactWindowBoundary() {
    aegisflow::feature::SlidingCounter<3> counter(1000);
    require(counter.add(1000, 1000), "窗口内计数必须接受");
    require(counter.add(2000, 2000, 2), "相邻桶的 delta 必须接受");
    require(counter.sum(3998) == 3, "边界内计数不得提前过期");
    require(counter.sum(4000) == 2, "早期 bucket 应在精确边界过期");
    require(counter.sum(5000) == 0, "所有 bucket 应按窗口过期");
}

void counterRejectsEventsOutsideAcceptedTimeRange() {
    aegisflow::feature::SlidingCounter<3> counter(1000);
    require(!counter.add(1000, 4000), "过期事件不得重新写入窗口");
    require(!counter.add(5000, 4000), "未来 bucket 事件不得写入窗口");
    require(counter.sum(4000) == 0, "拒绝的事件不得改变计数");
}

void distinctRefreshesMembersAndExpiresThem() {
    aegisflow::feature::SlidingDistinct distinct(3000, 1000, 2);
    require(distinct.add("user-a", 1000, 1000), "窗口内成员必须接受");
    require(distinct.add("user-a", 2000, 2000), "成员刷新必须接受");
    require(distinct.add("user-b", 2000, 2000), "第二个成员必须接受");
    require(distinct.count(3999) == 2, "刷新后的成员必须保留到新窗口");
    require(distinct.count(5000) == 0, "distinct 成员必须按窗口过期");
    require(distinct.activeMemberCount() == 0, "过期清理后成员数必须归零");
}

void distinctCapacityRejectsWithoutOvercounting() {
    aegisflow::feature::SlidingDistinct distinct(60'000, 1000, 2);
    require(distinct.add("user-a", 1000, 1000), "第一个成员必须接受");
    require(distinct.add("user-b", 1000, 1000), "第二个成员必须接受");
    require(!distinct.add("user-c", 1000, 1000), "超出容量的成员必须被拒绝");
    require(distinct.count(1000) == 2, "容量溢出不得多计成员");
}

void invalidWindowConfigurationIsRejected() {
    bool counter_rejected = false;
    try {
        aegisflow::feature::SlidingCounter<3> counter(0);
        static_cast<void>(counter);
    } catch (const std::invalid_argument&) {
        counter_rejected = true;
    }
    require(counter_rejected, "Counter 的零 bucket 必须被拒绝");

    bool distinct_rejected = false;
    try {
        aegisflow::feature::SlidingDistinct distinct(0, 1000, 2);
        static_cast<void>(distinct);
    } catch (const std::invalid_argument&) {
        distinct_rejected = true;
    }
    require(distinct_rejected, "Distinct 的零窗口必须被拒绝");
}

}  // namespace

int main() {
    return aegisflow::test::runModule(
        "sliding_window",
        {
            {"Counter 窗口边界", counterExpiresAtExactWindowBoundary},
            {"Counter 事件时间", counterRejectsEventsOutsideAcceptedTimeRange},
            {"Distinct 刷新与过期", distinctRefreshesMembersAndExpiresThem},
            {"Distinct 容量上限", distinctCapacityRejectsWithoutOvercounting},
            {"无效窗口配置", invalidWindowConfigurationIsRejected},
        }
    );
}
