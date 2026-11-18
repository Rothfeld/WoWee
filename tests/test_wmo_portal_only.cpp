// Portal-only WMO groups (pipeline::wmoPortalOnlyGroups), with the MOGP flags,
// portal counts and MOPR entries authored in TBC 2.4.3's
// World/wmo/Dungeon/BlackTemple/BlackTemple.wmo.

#include <catch_amalgamated.hpp>

#include "pipeline/wmo_loader.hpp"

#include <cstdint>
#include <vector>

using wowee::pipeline::WMOModel;
using wowee::pipeline::WMOPortalRef;
using wowee::pipeline::wmoPortalOnlyGroups;

namespace {

void setGroup(WMOModel& model, size_t index, uint32_t flags, uint16_t portalStart, uint16_t portalCount) {
    model.groups[index].flags = flags;
    model.groups[index].portalStart = portalStart;
    model.groups[index].portalCount = portalCount;
}

// The Black Temple groups around the courtyard, with their own MOPR entries
// (portalIndex, groupIndex, side) as authored.
WMOModel blackTemple() {
    WMOModel model;
    model.groups.resize(49);
    model.portalRefs.resize(78);
    auto ref = [&](uint16_t at, uint16_t portal, uint16_t group, int16_t side) {
        model.portalRefs[at] = WMOPortalRef{portal, group, side};
    };
    // boss (Illidan's summit): looks out at BossSummit_facade through MOPR 5-8.
    setGroup(model, 2, 0x42241, 4, 6);
    ref(4, 10, 30, -1);
    for (uint16_t i = 0; i < 4; ++i) ref(static_cast<uint16_t>(5 + i), static_cast<uint16_t>(67 + i), 48, 1);
    ref(9, 74, 47, 1);
    // test (the courtyard): looks out at BossFacade.
    setGroup(model, 8, 0x43a41, 38, 9);
    ref(38, 5, 1, 1);
    ref(39, 7, 0, 1);
    for (uint16_t i = 0; i < 4; ++i) ref(static_cast<uint16_t>(40 + i), static_cast<uint16_t>(42 + i), 39, -1);
    for (uint16_t i = 0; i < 3; ++i) ref(static_cast<uint16_t>(44 + i), static_cast<uint16_t>(75 + i), 39, -1);
    // 2ndrightwall: looks out at LowerBalconyFacade.
    setGroup(model, 6, 0x42a41, 50, 2);
    ref(50, 50, 40, -1);
    ref(51, 51, 40, -1);
    setGroup(model, 39, 0x428c1, 0, 0);  // BossFacade
    setGroup(model, 40, 0x428c1, 0, 0);  // LowerBalconyFacade
    setGroup(model, 46, 0x400c9, 0, 0);  // MainGateFacade: exterior (0x8)
    setGroup(model, 48, 0x428c1, 0, 0);  // BossSummit_facade
    return model;
}

}  // namespace

TEST_CASE("Black Temple facades are drawn only through portals", "[wmo][portal]") {
    const auto portalOnly = wmoPortalOnlyGroups(blackTemple());
    REQUIRE(portalOnly.size() == 49);
    CHECK(portalOnly[39]);
    CHECK(portalOnly[40]);
    CHECK(portalOnly[48]);
}

TEST_CASE("Rooms with portals of their own are not portal-only", "[wmo][portal]") {
    const auto portalOnly = wmoPortalOnlyGroups(blackTemple());
    CHECK_FALSE(portalOnly[2]);
    CHECK_FALSE(portalOnly[8]);
    CHECK_FALSE(portalOnly[6]);
}

TEST_CASE("Exterior groups without portals are not portal-only", "[wmo][portal]") {
    // MainGateFacade carries 0x8: exterior groups are drawn whatever the walk says.
    CHECK_FALSE(wmoPortalOnlyGroups(blackTemple())[46]);
}

TEST_CASE("An interior group no portal leads into is not portal-only", "[wmo][portal]") {
    // stormwind.wmo group 293 (0x12841): interior, no portals, named by no
    // MOPR entry. No walk can decide it, so it keeps being drawn.
    WMOModel model = blackTemple();
    setGroup(model, 41, 0x12841, 0, 0);
    CHECK_FALSE(wmoPortalOnlyGroups(model)[41]);
}

TEST_CASE("A portal-only room is the far side of a portal, not its owner", "[wmo][portal]") {
    // The same flags on the group that owns the portal: it has a way out.
    WMOModel model;
    model.groups.resize(2);
    model.portalRefs = {WMOPortalRef{0, 1, 1}, WMOPortalRef{0, 0, -1}};
    setGroup(model, 0, 0x428c1, 0, 1);
    setGroup(model, 1, 0x428c1, 1, 1);
    const auto portalOnly = wmoPortalOnlyGroups(model);
    CHECK_FALSE(portalOnly[0]);
    CHECK_FALSE(portalOnly[1]);
}
