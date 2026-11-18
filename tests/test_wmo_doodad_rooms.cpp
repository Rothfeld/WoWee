// One M2 instance standing for several WMO doodad entries
// (rendering::addWmoDoodad). Black Temple's MODD 1213 and 3422
// (SilvermoonSmallTree01, scale 1.51) stand at one spot, listed by the
// courtyard (group 8) and by BossSummit_facade (group 48).

#include <catch_amalgamated.hpp>

#include "rendering/wmo_doodad_rooms.hpp"

#include <vector>

using wowee::rendering::WmoDoodadRooms;
using wowee::rendering::addWmoDoodad;

TEST_CASE("Doodad entries at one spot are drawn with any of their rooms", "[wmo][doodad]") {
    WmoDoodadRooms tree = addWmoDoodad({}, 7, {8});
    tree = addWmoDoodad(tree, 7, {48});
    CHECK(tree.placed);
    CHECK(tree.wmoInstance == 7);
    CHECK(tree.rooms == std::vector<uint16_t>{8, 48});
    tree = addWmoDoodad(tree, 7, {8});
    CHECK(tree.rooms == std::vector<uint16_t>{8, 48});
}

TEST_CASE("A doodad entry listed by no room keeps the instance drawn", "[wmo][doodad]") {
    // Either order: an entry drawn with the building outweighs a room list.
    WmoDoodadRooms first = addWmoDoodad(addWmoDoodad({}, 7, {}), 7, {48});
    CHECK(first.wmoInstance == 0);
    CHECK(first.rooms.empty());
    WmoDoodadRooms second = addWmoDoodad(addWmoDoodad({}, 7, {48}), 7, {});
    CHECK(second.wmoInstance == 0);
    CHECK(second.rooms.empty());
}

TEST_CASE("Doodads of two buildings at one spot are drawn without rooms", "[wmo][doodad]") {
    const WmoDoodadRooms shared = addWmoDoodad(addWmoDoodad({}, 7, {8}), 9, {3});
    CHECK(shared.placed);
    CHECK(shared.wmoInstance == 0);
    CHECK(shared.rooms.empty());
}

TEST_CASE("A single doodad entry keeps its own rooms", "[wmo][doodad]") {
    const WmoDoodadRooms one = addWmoDoodad({}, 7, {8, 12});
    CHECK(one.wmoInstance == 7);
    CHECK(one.rooms == std::vector<uint16_t>{8, 12});
}
