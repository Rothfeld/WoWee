// WMO group liquid type and tile coverage, with tile bytes and flags as
// authored in TBC 2.4.3 files (see include/pipeline/wmo_liquid.hpp).

#include <catch_amalgamated.hpp>

#include "pipeline/wmo_liquid.hpp"

#include <vector>

using wowee::pipeline::resolveWmoLiquidType;
using wowee::pipeline::wmoLiquidTileWet;

TEST_CASE("MLIQ tile with low nibble 0x0F holds no liquid", "[wmo][liquid]") {
    // blackfathom_instance_022.wmo tile bytes.
    for (uint8_t tile : {0x0f, 0x1f, 0x3f, 0x5f, 0x7f, 0xcf, 0xff}) CHECK_FALSE(wmoLiquidTileWet(tile));
    for (uint8_t tile : {0x44, 0x54, 0x64, 0x74, 0xc4, 0xf4}) CHECK(wmoLiquidTileWet(tile));
}

TEST_CASE("TBC groups take their liquid type from the tile nibble", "[wmo][liquid]") {
    // MOHD flags 0, groupLiquid 0, as in every TBC MLIQ group.
    const std::vector<uint8_t> blackfathom{0x0f, 0x44, 0x74, 0xff};      // water
    const std::vector<uint8_t> blackrockLava{0xff, 0x46, 0x06};          // magma
    const std::vector<uint8_t> orgrimmarLava{0x02, 0x0f};                // magma
    const std::vector<uint8_t> stratholmeSlime{0x07, 0x0f};              // slime
    const std::vector<uint8_t> naxxSlime{0x03};                          // slime
    const std::vector<uint8_t> jungleCave{0x00, 0x0f};                   // water
    const std::vector<uint8_t> dry{0x0f, 0xff};
    CHECK(resolveWmoLiquidType(0, 0x3805, 0, blackfathom) == 1);
    CHECK(resolveWmoLiquidType(0, 0x3805, 0, blackrockLava) == 3);
    CHECK(resolveWmoLiquidType(0, 0x3805, 0, orgrimmarLava) == 3);
    CHECK(resolveWmoLiquidType(0, 0x3805, 0, stratholmeSlime) == 4);
    CHECK(resolveWmoLiquidType(0, 0x3a05, 0, naxxSlime) == 4);
    CHECK(resolveWmoLiquidType(0, 0x3a05, 0, jungleCave) == 1);
    CHECK(resolveWmoLiquidType(0, 0x3805, 0, dry) == 0);
}

TEST_CASE("Group flag 0x80000 turns basic water into ocean", "[wmo][liquid]") {
    const std::vector<uint8_t> water{0x04};
    CHECK(resolveWmoLiquidType(0, 0x80000, 0, water) == 2);
}

TEST_CASE("MOHD flag 0x4 names the LiquidType ID in the group", "[wmo][liquid]") {
    const std::vector<uint8_t> water{0x04};
    CHECK(resolveWmoLiquidType(0x4, 0, 41, water) == 41);
    CHECK(resolveWmoLiquidType(0x4, 0, 3, water) == 3);
}

TEST_CASE("Legacy nonzero groupLiquid overrides the tiles", "[wmo][liquid]") {
    const std::vector<uint8_t> water{0x04};
    CHECK(resolveWmoLiquidType(0, 0, 2, water) == 3);   // entry 3 -> magma
    CHECK(resolveWmoLiquidType(0, 0, 15, water) == 1);  // 15 -> tiles
}
