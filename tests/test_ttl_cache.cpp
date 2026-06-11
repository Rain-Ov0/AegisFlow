#include "aegisflow/cache/ttl_cache.hpp"

#include <cassert>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

struct ManualClock {
    using rep = int64_t;
    using period = std::milli;
    using duration = std::chrono::duration<rep, period>;
    using time_point = std::chrono::time_point<ManualClock>;
    static constexpr bool is_steady = true;

    static time_point now() {
        return now_;
    }

    static void set(uint64_t now_ms) {
        now_ = time_point(duration(now_ms));
    }

    static inline time_point now_{duration(0)};
};

using TtlStringCache = aegisflow::cache::TtlCache<
    std::string,
    std::string,
    ManualClock
>;

void test_get_before_expire() {
    ManualClock::set(1000);

    TtlStringCache cache(2);
    cache.put("black:user:u1", "blacklisted_user", 100);

    std::string value;
    assert(cache.get("black:user:u1", value));
    assert(value == "blacklisted_user");
}

void test_expired_key_misses() {
    ManualClock::set(1000);

    TtlStringCache cache(2);
    cache.put("black:user:u1", "blacklisted_user", 100);

    ManualClock::set(1099);

    std::string value;
    assert(cache.get("black:user:u1", value));

    ManualClock::set(1100);

    assert(!cache.get("black:user:u1", value));
    assert(cache.size() == 0);
}

void test_update_refreshes_ttl() {
    ManualClock::set(1000);

    TtlStringCache cache(2);
    cache.put("black:user:u1", "old_reason", 100);

    ManualClock::set(1090);
    cache.put("black:user:u1", "new_reason", 100);

    ManualClock::set(1150);

    std::string value;
    assert(cache.get("black:user:u1", value));
    assert(value == "new_reason");

    ManualClock::set(1190);
    assert(!cache.get("black:user:u1", value));
}

void test_lru_eviction() {
    ManualClock::set(1000);

    TtlStringCache cache(2);
    cache.put("black:user:u1", "r1", 1000);
    cache.put("black:user:u2", "r2", 1000);

    std::string value;
    assert(cache.get("black:user:u1", value));

    cache.put("black:user:u3", "r3", 1000);

    assert(cache.get("black:user:u1", value));
    assert(!cache.get("black:user:u2", value));
    assert(cache.get("black:user:u3", value));
}

void test_zero_ttl_is_not_cached() {
    ManualClock::set(1000);

    TtlStringCache cache(2);
    cache.put("black:user:u1", "blacklisted_user", 0);

    std::string value;
    assert(!cache.get("black:user:u1", value));
}

void test_purge_expired() {
    ManualClock::set(1000);

    TtlStringCache cache(3);
    cache.put("black:user:u1", "r1", 100);
    cache.put("black:user:u2", "r2", 200);
    cache.put("black:user:u3", "r3", 300);

    ManualClock::set(1200);

    assert(cache.purgeExpired() == 2);
    assert(cache.size() == 1);
}

void test_invalid_capacity() {
    bool thrown = false;

    try {
        TtlStringCache cache(0);
    } catch (const std::invalid_argument&) {
        thrown = true;
    }

    assert(thrown);
}

int main() {
    test_get_before_expire();
    test_expired_key_misses();
    test_update_refreshes_ttl();
    test_lru_eviction();
    test_zero_ttl_is_not_cached();
    test_purge_expired();
    test_invalid_capacity();

    std::cout << "test_ttl_cache passed" << std::endl;
    return 0;
}