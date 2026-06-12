#include "aegisflow/risk/blacklist_manager.hpp"

#include <cassert>
#include <iostream>
#include <string>

namespace v1 = aegisflow::v1;

using aegisflow::risk::BlacklistEntry;
using aegisflow::risk::BlacklistManager;
using aegisflow::risk::BlacklistManagerOptions;
using aegisflow::risk::EntityType;

BlacklistManagerOptions testOptions() {
    BlacklistManagerOptions options;
    options.bloom_bits = 1 << 16;
    options.bloom_hashes = 5;
    options.cache_capacity = 64;
    options.positive_ttl_ms = 60ULL * 1000ULL;
    options.negative_ttl_ms = 1000ULL;
    return options;
}

v1::Event makeEvent(
    const std::string& user_id,
    const std::string& ip,
    const std::string& device_id
) {
    v1::Event event;
    event.set_event_id(1);
    event.set_timestamp_ms(1000);
    event.set_user_id(user_id);
    event.set_ip(ip);
    event.set_device_id(device_id);
    event.set_scene("login");
    event.set_type(v1::LOGIN);
    event.set_result(v1::SUCCESS);
    return event;
}

void test_user_blacklist_hit() {
    BlacklistManager manager(nullptr, testOptions());

    manager.loadEntries({
        {EntityType::User, "u_black_001", "blacklisted_user", 0},
    });

    const auto result = manager.checkUser("u_black_001");

    assert(result.hit);
    assert(result.type == EntityType::User);
    assert(result.id == "u_black_001");
    assert(result.reason == "blacklisted_user");
    assert(manager.localSize() == 1);
}

void test_ip_blacklist_hit() {
    BlacklistManager manager(nullptr, testOptions());

    manager.loadEntries({
        {EntityType::Ip, "10.0.0.9", "blacklisted_ip", 0},
    });

    const auto result = manager.checkIp("10.0.0.9");

    assert(result.hit);
    assert(result.type == EntityType::Ip);
    assert(result.reason == "blacklisted_ip");
}

void test_device_blacklist_hit() {
    BlacklistManager manager(nullptr, testOptions());

    manager.loadEntries({
        {EntityType::Device, "dev_black_001", "blacklisted_device", 0},
    });

    const auto result = manager.checkDevice("dev_black_001");

    assert(result.hit);
    assert(result.type == EntityType::Device);
    assert(result.reason == "blacklisted_device");
}

void test_missing_entities_return_miss() {
    BlacklistManager manager(nullptr, testOptions());

    manager.loadEntries({
        {EntityType::User, "u_black_001", "blacklisted_user", 0},
    });

    assert(!manager.checkUser("u_normal_001").hit);
    assert(!manager.checkIp("10.0.0.1").hit);
    assert(!manager.checkDevice("dev_normal_001").hit);
    assert(!manager.checkUser("").hit);
}

void test_check_event_returns_first_hit() {
    BlacklistManager manager(nullptr, testOptions());

    manager.loadEntries({
        {EntityType::User, "u_black_001", "blacklisted_user", 0},
        {EntityType::Ip, "10.0.0.9", "blacklisted_ip", 0},
        {EntityType::Device, "dev_black_001", "blacklisted_device", 0},
    });

    const auto result = manager.checkEvent(
        makeEvent("u_black_001", "10.0.0.9", "dev_black_001")
    );

    assert(result.hit);
    assert(result.type == EntityType::User);
    assert(result.reason == "blacklisted_user");
}

void test_expired_entry_is_not_loaded() {
    BlacklistManager manager(nullptr, testOptions());

    manager.loadEntries({
        {EntityType::User, "u_expired", "expired_blacklist", 1},
    });

    assert(manager.localSize() == 0);
    assert(!manager.checkUser("u_expired").hit);
}

void test_reload_replaces_local_state_and_clears_cache() {
    BlacklistManager manager(nullptr, testOptions());

    manager.loadEntries({
        {EntityType::User, "u_black_001", "blacklisted_user", 0},
    });

    assert(manager.checkUser("u_black_001").hit);

    manager.loadEntries({});

    assert(manager.localSize() == 0);
    assert(!manager.checkUser("u_black_001").hit);
}

int main() {
    test_user_blacklist_hit();
    test_ip_blacklist_hit();
    test_device_blacklist_hit();
    test_missing_entities_return_miss();
    test_check_event_returns_first_hit();
    test_expired_entry_is_not_loaded();
    test_reload_replaces_local_state_and_clears_cache();

    std::cout << "test_blacklist_manager passed" << std::endl;
    return 0;
}