#include "aegisflow/cache/lru_cache.hpp"

#include <cassert>
#include <iostream>
#include <stdexcept>
#include <string>

using aegisflow::cache::LruCache;

void test_put_and_get() {
    LruCache<std::string, int> cache(2);

    cache.put("user:u1", 1);

    int value = 0;
    assert(cache.get("user:u1", value));
    assert(value == 1);
}

void test_missing_key_returns_false() {
    LruCache<std::string, int> cache(2);

    int value = 100;
    assert(!cache.get("user:missing", value));
    assert(value == 100);
}

void test_update_existing_key() {
    LruCache<std::string, int> cache(2);

    cache.put("user:u1", 1);
    cache.put("user:u1", 2);

    int value = 0;
    assert(cache.get("user:u1", value));
    assert(value == 2);
    assert(cache.size() == 1);
}

void test_evict_least_recently_used_key() {
    LruCache<std::string, int> cache(2);

    cache.put("user:u1", 1);
    cache.put("user:u2", 2);

    int value = 0;
    assert(cache.get("user:u1", value));

    cache.put("user:u3", 3);

    assert(cache.get("user:u1", value));
    assert(!cache.get("user:u2", value));
    assert(cache.get("user:u3", value));
}

void test_erase_and_clear() {
    LruCache<std::string, int> cache(2);

    cache.put("user:u1", 1);
    cache.put("user:u2", 2);

    assert(cache.erase("user:u1"));
    assert(!cache.erase("user:u1"));

    int value = 0;
    assert(!cache.get("user:u1", value));
    assert(cache.size() == 1);

    cache.clear();
    assert(cache.size() == 0);
}

void test_invalid_capacity() {
    bool thrown = false;

    try {
        LruCache<std::string, int> cache(0);
    } catch (const std::invalid_argument&) {
        thrown = true;
    }

    assert(thrown);
}

int main() {
    test_put_and_get();
    test_missing_key_returns_false();
    test_update_existing_key();
    test_evict_least_recently_used_key();
    test_erase_and_clear();
    test_invalid_capacity();

    std::cout << "test_lru_cache passed" << std::endl;
    return 0;
}