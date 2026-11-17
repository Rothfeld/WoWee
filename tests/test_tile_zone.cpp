#include <catch_amalgamated.hpp>

#include "game/zone_manager.hpp"

using wowee::game::mostCommonZoneId;

TEST_CASE("A tile's zone is the zone most of its chunks name", "[tile_zone]") {
    // An Elwynn/Duskwood bank tile: most chunks Elwynn (12), some Duskwood (10).
    CHECK(mostCommonZoneId({12, 12, 10, 12, 10}) == 12u);
    // Area 0 says nothing.
    CHECK(mostCommonZoneId({0, 0, 10}) == 10u);
    CHECK(mostCommonZoneId({}) == 0u);
    CHECK(mostCommonZoneId({0, 0}) == 0u);
    // A tie does not depend on order.
    CHECK(mostCommonZoneId({1, 12}) == mostCommonZoneId({12, 1}));
}
