// TBC 2.4.3 WMO MODS doodad-set selection for GameObjects, Transports,
// WDT global WMOs, and ADT MODF placements.

#include <catch_amalgamated.hpp>

#include "pipeline/wmo_loader.hpp"
#include "tbc_data_test_util.hpp"

#include <cstring>
#include <vector>

using wowee::pipeline::WMODoodadSet;
using wowee::pipeline::WMOLoader;
using wowee::pipeline::WMOModel;
using wowee::pipeline::selectWmoDoodadIndices;

namespace {

WMODoodadSet makeSet(const char* name, uint32_t startIndex, uint32_t count) {
    WMODoodadSet set{};
    std::strncpy(set.name, name, sizeof(set.name) - 1);
    set.startIndex = startIndex;
    set.count = count;
    return set;
}

} // namespace

TEST_CASE("WMO GameObject/Transport default MODS selects Set_$DefaultGlobal only", "[wmo][mods][doodad]") {
    // Models like Transportship_NE.wmo or OrcZeppelinHouse.wmo have
    // Set_$DefaultGlobal (index 0) plus additional named sets. Default
    // GameObject/Transport selection (activeSetIndex = 0) must only emit set 0.
    const std::vector<WMODoodadSet> sets = {
        makeSet("Set_$DefaultGlobal", 0, 2),
        makeSet("Set_Sail", 2, 3),
        makeSet("Set_Extra", 5, 4),
    };
    const auto active = selectWmoDoodadIndices(sets, /*totalDoodads=*/9, /*activeSetIndex=*/0);
    CHECK(active == std::vector<uint32_t>{0, 1});
}

TEST_CASE("WMO MODS placement set combines Set_$DefaultGlobal and active set without inactive sets", "[wmo][mods][doodad]") {
    const std::vector<WMODoodadSet> sets = {
        makeSet("Set_$DefaultGlobal", 0, 2),
        makeSet("Set_A", 2, 2),
        makeSet("Set_B", 4, 3),
    };
    const auto activeB = selectWmoDoodadIndices(sets, /*totalDoodads=*/7, /*activeSetIndex=*/2);
    CHECK(activeB == std::vector<uint32_t>{0, 1, 4, 5, 6});
}

TEST_CASE("WMO MODS selection deduplicates overlapping sets and clamps out-of-range indices", "[wmo][mods][doodad]") {
    const std::vector<WMODoodadSet> sets = {
        makeSet("Set_$DefaultGlobal", 0, 3),
        makeSet("Set_Overlap", 2, 10),
    };
    const auto active = selectWmoDoodadIndices(sets, /*totalDoodads=*/5, /*activeSetIndex=*/1);
    CHECK(active == std::vector<uint32_t>{0, 1, 2, 3, 4});

    // Out-of-range activeSetIndex falls back to Set_$DefaultGlobal only.
    const auto oobSet = selectWmoDoodadIndices(sets, /*totalDoodads=*/5, /*activeSetIndex=*/99);
    CHECK(oobSet == std::vector<uint32_t>{0, 1, 2});

    // Empty MODS produces no doodads rather than dumping raw MODD entries.
    CHECK(selectWmoDoodadIndices({}, /*totalDoodads=*/5, 0).empty());
    CHECK(selectWmoDoodadIndices(sets, /*totalDoodads=*/0, 0).empty());
}

TEST_CASE("TBC 2.4.3 Transportship.wmo and Transportship_NE.wmo MODS selection", "[wmo][mods][doodad][tbcdata]") {
    const auto shipBytes = wowee::test::readTbcData("world/wmo/transports/transport_ship/transportship.wmo");
    const WMOModel ship = WMOLoader::load(shipBytes);
    REQUIRE(ship.doodadSets.size() == 1);
    REQUIRE(ship.doodads.size() == 134);
    const auto shipActive = selectWmoDoodadIndices(ship.doodadSets, ship.doodads.size(), 0);
    CHECK(shipActive.size() == 134);

    const auto neBytes = wowee::test::readTbcData("world/wmo/transports/transport_ship_ne/transportship_ne.wmo");
    const WMOModel neShip = WMOLoader::load(neBytes);
    REQUIRE(neShip.doodadSets.size() == 2);
    REQUIRE(neShip.doodads.size() == 2);
    // Default set 0 selects only Set_$DefaultGlobal (index 0), excluding Set_Sail (index 1).
    CHECK(selectWmoDoodadIndices(neShip.doodadSets, neShip.doodads.size(), 0) == std::vector<uint32_t>{0});
    CHECK(selectWmoDoodadIndices(neShip.doodadSets, neShip.doodads.size(), 1) == std::vector<uint32_t>{0, 1});
}
