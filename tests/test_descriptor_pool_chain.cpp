// The rule behind the terrain renderer's material descriptor pools
// (rendering/descriptor_pool_chain.hpp): a set comes from an existing pool
// while one can serve, a pool is added only when none can, and every set is
// returned beside the pool it has to be freed to.

#include <catch_amalgamated.hpp>

#include "rendering/descriptor_pool_chain.hpp"

#include <map>
#include <set>
#include <vector>

namespace {

// Pools of a fixed capacity; handles are small integers, 0 is null.
struct FakeBackend {
    int capacity;
    int maxPools = 1000;
    int nextPool = 1;
    int nextSet = 1;
    int failedAllocations = 0;
    std::map<int, std::set<int>> live;  // pool -> its live sets

    int createPool() {
        if (static_cast<int>(live.size()) >= maxPools) return 0;
        live[nextPool];
        return nextPool++;
    }
    int allocate(int pool) {
        auto& sets = live.at(pool);
        if (static_cast<int>(sets.size()) >= capacity) {
            ++failedAllocations;
            return 0;
        }
        sets.insert(nextSet);
        return nextSet++;
    }
    void free(int pool, int set) { REQUIRE(live.at(pool).erase(set) == 1); }
};

using Chain = wowee::rendering::DescriptorPoolChain<int, int>;

} // namespace

TEST_CASE("No pool exists before the first set", "[descriptor_pool_chain]") {
    Chain chain;
    REQUIRE(chain.pools().empty());
}

TEST_CASE("Pools are added only when the existing ones are full", "[descriptor_pool_chain]") {
    FakeBackend backend{.capacity = 4};
    Chain chain;
    for (int n = 1; n <= 10; ++n) {
        const auto allocation = chain.allocate(backend);
        REQUIRE(allocation);
        REQUIRE(chain.pools().size() == static_cast<size_t>((n + 3) / 4));
        // The pool handed back is the one that holds the set.
        REQUIRE(backend.live.at(allocation.pool).count(allocation.set) == 1);
    }
    REQUIRE(backend.live.at(1).size() == 4);
    REQUIRE(backend.live.at(2).size() == 4);
    REQUIRE(backend.live.at(3).size() == 2);
}

TEST_CASE("Space freed in an earlier pool is used before a pool is added", "[descriptor_pool_chain]") {
    FakeBackend backend{.capacity = 4};
    Chain chain;
    std::vector<Chain::Allocation> held;
    for (int n = 0; n < 8; ++n) held.push_back(chain.allocate(backend));
    REQUIRE(chain.pools().size() == 2);

    // A tile unloads: three sets of the first pool go back.
    for (int n = 0; n < 3; ++n) backend.free(held[n].pool, held[n].set);

    for (int n = 0; n < 3; ++n) {
        const auto allocation = chain.allocate(backend);
        REQUIRE(allocation);
        REQUIRE(allocation.pool == held[0].pool);
    }
    REQUIRE(chain.pools().size() == 2);
    REQUIRE(chain.allocate(backend));  // both full again
    REQUIRE(chain.pools().size() == 3);
}

TEST_CASE("The pool that served last is asked first", "[descriptor_pool_chain]") {
    FakeBackend backend{.capacity = 100};
    Chain chain;
    for (int n = 0; n < 250; ++n) REQUIRE(chain.allocate(backend));
    REQUIRE(chain.pools().size() == 3);
    // Sets that fit ask only the pool that served last. A full pool is asked
    // only when a pool has to be added, each existing pool once: set 101 asks
    // pool 1, set 201 asks pools 2 and 1.
    REQUIRE(backend.failedAllocations == 3);
}

TEST_CASE("A pool that cannot be created yields an empty allocation", "[descriptor_pool_chain]") {
    FakeBackend backend{.capacity = 2, .maxPools = 1};
    Chain chain;
    REQUIRE(chain.allocate(backend));
    REQUIRE(chain.allocate(backend));
    const auto none = chain.allocate(backend);
    REQUIRE_FALSE(none);
    REQUIRE(chain.pools().size() == 1);
    // Once a set is back the same pool serves again.
    backend.free(1, *backend.live.at(1).begin());
    REQUIRE(chain.allocate(backend));
}

TEST_CASE("destroyAll hands over every pool once", "[descriptor_pool_chain]") {
    FakeBackend backend{.capacity = 1};
    Chain chain;
    for (int n = 0; n < 5; ++n) REQUIRE(chain.allocate(backend));
    std::vector<int> destroyed;
    chain.destroyAll([&](int pool) { destroyed.push_back(pool); });
    REQUIRE(destroyed == std::vector<int>{1, 2, 3, 4, 5});
    REQUIRE(chain.pools().empty());
    // A chain that is used again starts a new pool.
    REQUIRE(chain.allocate(backend));
    REQUIRE(chain.pools().size() == 1);
}
