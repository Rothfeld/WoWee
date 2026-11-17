// Which units Tab and TargetNearestEnemy may pick.
//
// The wolves at the Dun Morogh start carry a faction template with no
// hostility in it, so they are neutral rather than hostile. Asking for hostile
// units alone found none of them, and Tab cleared the target instead.
#include <catch_amalgamated.hpp>

#include "game/tab_target.hpp"

using namespace wowee::game;

namespace {
constexpr uint32_t kBeast = 1;
}

TEST_CASE("a hostile unit is a tab target", "[tab-target]") {
    CHECK(isTabTargetable(true, false, false, kBeast, 0));
}

TEST_CASE("a neutral unit is a tab target", "[tab-target]") {
    CHECK(isTabTargetable(false, false, false, kBeast, 0));
}

TEST_CASE("a friendly unit is not a tab target", "[tab-target]") {
    CHECK_FALSE(isTabTargetable(false, true, false, kBeast, 0));
}

TEST_CASE("a friendly unit attacking the player is a tab target", "[tab-target]") {
    CHECK(isTabTargetable(false, true, true, kBeast, 0));
}

TEST_CASE("a neutral critter is not a tab target", "[tab-target]") {
    CHECK_FALSE(isTabTargetable(false, false, false, CREATURE_TYPE_CRITTER, 0));
}

TEST_CASE("a hostile critter is a tab target", "[tab-target]") {
    CHECK(isTabTargetable(true, false, false, CREATURE_TYPE_CRITTER, 0));
}

TEST_CASE("a unit whose creature type has not arrived is a tab target", "[tab-target]") {
    CHECK(isTabTargetable(false, false, false, 0, 0));
}

TEST_CASE("a unit that cannot be selected or attacked is never a tab target", "[tab-target]") {
    for (const uint32_t flag : {UNIT_FLAG_NOT_SELECTABLE, UNIT_FLAG_NON_ATTACKABLE,
                                UNIT_FLAG_IMMUNE_TO_PC}) {
        CHECK_FALSE(isTabTargetable(true, false, true, kBeast, flag));
        CHECK_FALSE(isTabTargetable(false, false, false, kBeast, flag));
    }
}

// Geometry and order measured on the 2.4.3 client; see tab_target.hpp.
namespace {
constexpr float kDeg = 3.14159265358979f / 180.0f;

// A point `yards` away at `degrees` from facing north (canonical yaw 0), left positive.
glm::vec3 at(float degrees, float yards) {
    // Canonical yaw is atan2(-dy, dx): left of north is -y... rotate the facing direction.
    const float yaw = -degrees * kDeg;  // positive degrees = counterclockwise seen from above
    return {yards * std::cos(yaw), -yards * std::sin(yaw), 0.0f};
}
}

TEST_CASE("the tab cone is 30 degrees either side of the facing", "[tab-target]") {
    const glm::vec3 player(0.0f);
    for (const float side : {1.0f, -1.0f}) {
        CHECK(isInTabCone(player, 0.0f, at(side * 29.9f, 20.0f)));
        CHECK_FALSE(isInTabCone(player, 0.0f, at(side * 30.1f, 20.0f)));
    }
    CHECK_FALSE(isInTabCone(player, 0.0f, at(180.0f, 8.0f)));
    // The cone turns with the facing.
    CHECK(isInTabCone(player, 1.0f, at(-1.0f / kDeg, 15.0f)));
}

TEST_CASE("tab range is 41 yards in 3D", "[tab-target]") {
    const glm::vec3 player(0.0f);
    CHECK(isInTabRange(player, {40.94f, 0.0f, 0.0f}));
    CHECK_FALSE(isInTabRange(player, {41.19f, 0.0f, 0.0f}));
    CHECK_FALSE(isInTabRange(player, {40.9f, 0.0f, 5.0f}));
}

TEST_CASE("a point behind the camera or outside the view is off screen", "[tab-target]") {
    const glm::mat4 identity(1.0f);
    CHECK(isOnScreen(identity, {0.5f, -0.5f, 0.0f}));
    CHECK_FALSE(isOnScreen(identity, {1.5f, 0.0f, 0.0f}));
    glm::mat4 behind(1.0f);
    behind[3][3] = -1.0f;
    CHECK_FALSE(isOnScreen(behind, {0.0f, 0.0f, 0.0f}));
}

TEST_CASE("tab picks the nearest first and steps outward, wrapping", "[tab-target]") {
    const std::vector<TabCandidate> c{{3, 35.0f}, {1, 8.0f}, {2, 20.0f}};
    CHECK(pickTabTarget(c, 0, false, false) == 1);
    CHECK(pickTabTarget(c, 1, false, false) == 2);
    CHECK(pickTabTarget(c, 2, false, false) == 3);
    CHECK(pickTabTarget(c, 3, false, false) == 1);
}

TEST_CASE("a target outside the list or a paused cycle restarts at the nearest", "[tab-target]") {
    const std::vector<TabCandidate> c{{1, 8.0f}, {2, 20.0f}};
    CHECK(pickTabTarget(c, 99, false, false) == 1);
    CHECK(pickTabTarget(c, 1, true, false) == 1);
    CHECK(pickTabTarget({}, 1, false, false) == 0);
}

TEST_CASE("reverse steps back through the same order", "[tab-target]") {
    const std::vector<TabCandidate> c{{1, 8.0f}, {2, 20.0f}, {3, 35.0f}};
    CHECK(pickTabTarget(c, 0, false, true) == 1);
    CHECK(pickTabTarget(c, 1, false, true) == 3);
    CHECK(pickTabTarget(c, 3, false, true) == 2);
}
