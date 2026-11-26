// The rule the terrain renderer's tileset texture cache follows
// (rendering/pinned_lru_cache.hpp): what counts against the budget, what may
// be evicted, in which order, and how much.

#include <catch_amalgamated.hpp>

#include "rendering/pinned_lru_cache.hpp"

#include <string>
#include <vector>

using Cache = wowee::rendering::PinnedLruCache<std::string, std::string>;

namespace {

Cache makeCache(size_t budget) {
    Cache cache;
    cache.setBudgetBytes(budget);
    return cache;
}

} // namespace

TEST_CASE("Nothing is evicted while the request fits", "[pinned_lru_cache]") {
    Cache cache = makeCache(100);
    cache.insert("grass", "grass", 40);
    cache.insert("dirt", "dirt", 40);

    std::vector<std::string> evicted;
    REQUIRE(cache.makeRoom(20, evicted));
    REQUIRE(evicted.empty());
    REQUIRE(cache.size() == 2);
    REQUIRE(cache.bytes() == 80);
}

TEST_CASE("The least recently used unpinned entries go first, and only as many as needed", "[pinned_lru_cache]") {
    Cache cache = makeCache(100);
    cache.insert("grass", "grass", 30);
    cache.insert("dirt", "dirt", 30);
    cache.insert("rock", "rock", 30);
    // A lookup is a use: grass is now the most recent.
    REQUIRE(cache.find("grass") != nullptr);

    std::vector<std::string> evicted;
    REQUIRE(cache.makeRoom(40, evicted));
    REQUIRE(evicted == std::vector<std::string>{"dirt"});
    REQUIRE(cache.bytes() == 60);
    REQUIRE(cache.contains("grass"));
    REQUIRE(cache.contains("rock"));

    evicted.clear();
    REQUIRE(cache.makeRoom(100, evicted));
    REQUIRE(evicted == std::vector<std::string>{"rock", "grass"});
    REQUIRE(cache.size() == 0);
    REQUIRE(cache.bytes() == 0);
}

TEST_CASE("A pinned entry is never evicted, however old", "[pinned_lru_cache]") {
    Cache cache = makeCache(100);
    Cache::Entry& grass = cache.insert("grass", "grass", 50);
    cache.insert("dirt", "dirt", 50);
    cache.pin(grass);

    std::vector<std::string> evicted;
    REQUIRE(cache.makeRoom(50, evicted));
    REQUIRE(evicted == std::vector<std::string>{"dirt"});
    REQUIRE(cache.contains("grass"));
    // The pointer handed out at insert is still the entry.
    REQUIRE(cache.find("grass") == &grass);
    REQUIRE(grass.value == "grass");
}

TEST_CASE("With everything pinned there is no room and nothing is lost", "[pinned_lru_cache]") {
    Cache cache = makeCache(100);
    Cache::Entry& grass = cache.insert("grass", "grass", 60);
    Cache::Entry& dirt = cache.insert("dirt", "dirt", 40);
    cache.pin(grass);
    cache.pin(dirt);

    std::vector<std::string> evicted;
    REQUIRE_FALSE(cache.makeRoom(10, evicted));
    REQUIRE(evicted.empty());
    REQUIRE(cache.size() == 2);
    REQUIRE(cache.bytes() == 100);
}

TEST_CASE("An entry becomes evictable when its last pin is released", "[pinned_lru_cache]") {
    Cache cache = makeCache(100);
    Cache::Entry& grass = cache.insert("grass", "grass", 100);
    // Two chunks drawing with the same texture.
    cache.pin(grass);
    cache.pin(grass);
    REQUIRE(cache.pinnedEntries() == 1);

    std::vector<std::string> evicted;
    cache.unpin(grass);
    REQUIRE(cache.pinnedEntries() == 1);
    REQUIRE_FALSE(cache.makeRoom(10, evicted));

    cache.unpin(grass);
    REQUIRE(cache.pinnedEntries() == 0);
    // Unpinned is not evicted: it stays cached until room is asked for.
    REQUIRE(cache.contains("grass"));
    REQUIRE(cache.makeRoom(10, evicted));
    REQUIRE(evicted == std::vector<std::string>{"grass"});
}

TEST_CASE("Entry pointers survive the growth of the cache", "[pinned_lru_cache]") {
    Cache cache = makeCache(1u << 30);
    Cache::Entry& first = cache.insert("tile0", "tile0", 1);
    cache.pin(first);
    for (int i = 1; i < 5000; ++i) {
        cache.insert("tile" + std::to_string(i), "v", 1);
    }
    REQUIRE(cache.find("tile0") == &first);
    REQUIRE(first.value == "tile0");
    REQUIRE(first.pins == 1);
}

TEST_CASE("Visiting many places keeps the accounting at what is cached", "[pinned_lru_cache]") {
    // The shape of the teleport sequence: each place pins a few shared
    // textures, the previous place's pins are released, and the budget holds
    // three places' worth. Only what is still cached counts.
    Cache cache = makeCache(120);
    std::vector<Cache::Entry*> held;
    size_t evictedTotal = 0;
    for (int place = 0; place < 200; ++place) {
        for (Cache::Entry* entry : held) cache.unpin(*entry);
        held.clear();
        for (int t = 0; t < 4; ++t) {
            const std::string key = "place" + std::to_string(place) + "_" + std::to_string(t);
            std::vector<std::string> evicted;
            REQUIRE(cache.makeRoom(10, evicted));
            evictedTotal += evicted.size();
            Cache::Entry& entry = cache.insert(key, key, 10);
            cache.pin(entry);
            held.push_back(&entry);
        }
        REQUIRE(cache.bytes() <= 120);
        REQUIRE(cache.bytes() == cache.size() * 10);
        REQUIRE(cache.pinnedEntries() == 4);
    }
    REQUIRE(cache.size() == 12);
    REQUIRE(evictedTotal == 200 * 4 - 12);
}

TEST_CASE("drain hands back every value and empties the cache", "[pinned_lru_cache]") {
    Cache cache = makeCache(100);
    cache.pin(cache.insert("grass", "grass", 10));
    cache.insert("dirt", "dirt", 10);

    std::vector<std::string> drained;
    cache.drain([&](std::string& value) { drained.push_back(value); });
    REQUIRE(drained.size() == 2);
    REQUIRE(cache.size() == 0);
    REQUIRE(cache.bytes() == 0);
    REQUIRE(cache.pinnedEntries() == 0);
}

TEST_CASE("Minimap interior compositing never evicts tiles pinned in the same pass", "[pinned_lru_cache][minimap]") {
    // Reproduces the cross-map WMO minimap crash shape: a 1024-tile cache
    // filled across prior maps, followed by an interior WMO pass (e.g. Black
    // Temple map 564) pinning hundreds of tiles in one composite loop before
    // writing descriptor sets. No tile pinned earlier in the pass may be
    // evicted by later tiles in the same pass, and evicted tiles from prior
    // passes are returned for fence-deferred cleanup rather than destroyed
    // immediately.
    constexpr size_t kMaxTileCache = 1024;
    Cache cache = makeCache(kMaxTileCache);

    for (size_t i = 0; i < kMaxTileCache; ++i) {
        const std::string key = "prior_tile_" + std::to_string(i);
        cache.insert(key, key, 1);
    }
    REQUIRE(cache.size() == kMaxTileCache);

    std::vector<Cache::Entry*> passPinned;
    std::vector<std::string> passKeys;
    std::vector<std::string> deferredEvictions;
    constexpr size_t kInteriorTilesInPass = 384;

    for (size_t i = 0; i < kInteriorTilesInPass; ++i) {
        const std::string key = "bt_interior_" + std::to_string(i);
        passKeys.push_back(key);
        Cache::Entry* entry = cache.find(key);
        if (!entry) {
            std::vector<std::string> evicted;
            REQUIRE(cache.makeRoom(1, evicted));
            for (auto& oldVal : evicted) {
                deferredEvictions.push_back(std::move(oldVal));
            }
            entry = &cache.insert(key, key, 1);
        }
        cache.pin(*entry);
        passPinned.push_back(entry);
    }

    // Every pointer collected during the loop remains valid and intact when
    // building VkWriteDescriptorSet entries at the end of compositeInterior.
    REQUIRE(cache.size() == kMaxTileCache);
    REQUIRE(cache.pinnedEntries() == kInteriorTilesInPass);
    REQUIRE(deferredEvictions.size() == kInteriorTilesInPass);
    for (size_t i = 0; i < kInteriorTilesInPass; ++i) {
        REQUIRE(passPinned[i]->pins == 1);
        REQUIRE(passPinned[i]->value == passKeys[i]);
        REQUIRE(cache.find(passKeys[i]) == passPinned[i]);
    }

    // Releasing pins at the end of the pass makes those tiles eligible for
    // future passes without dropping them from the cache immediately.
    for (Cache::Entry* entry : passPinned) {
        cache.unpin(*entry);
    }
    REQUIRE(cache.pinnedEntries() == 0);
    REQUIRE(cache.size() == kMaxTileCache);
}
