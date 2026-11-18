// Mounted units: the rider's seat, its mount's size and a flying unit's
// create-block spline. Native observations (2.4.3.8606), Dragonmaw Wind
// Reaver 23330 and Sky Stalker 23030 tempspawned in Nagrand:
// _experiments/20261118T150000Z.npc_mount in the workspace.
#include <catch_amalgamated.hpp>

#include "game/spline_flight.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "rendering/animation/rider_pose.hpp"
#include "rendering/character_scale.hpp"

using namespace wowee;
namespace anim = wowee::rendering::anim;

TEST_CASE("A rider's idle, locomotion and melee stance are its seat", "[rider]") {
    for (const uint32_t seat : {anim::STAND, anim::WALK, anim::RUN, anim::FLY_FORWARD,
                                anim::FLY_IDLE, anim::READY_UNARMED, anim::READY_1H,
                                anim::READY_2H, anim::READY_2H_LOOSE, anim::MOUNT})
        CHECK(rendering::riderKeepsSeat(seat));
}

TEST_CASE("A rider's swings, casts and wounds play over the seat", "[rider]") {
    // Attack1H (17), SpellCastDirected (53), its hold ReadySpellDirected (51),
    // ChannelCastOmni (125), CombatWound (9), a bow's aim (29), Death (1).
    for (const uint32_t action : {17u, 53u, 51u, 125u, 9u, anim::READY_BOW, anim::DEATH})
        CHECK_FALSE(rendering::riderKeepsSeat(action));
}

TEST_CASE("A mount is drawn at its own scale times its rider's", "[rider]") {
    // Wind Reaver: rider display 21546 at 2.0 on nether drake display 21548
    // (1.0, model 2516 at 1.0) - the drake twice a player's.
    CHECK(rendering::remoteMountScale(2.0f, 1.0f, 1.0f) == Catch::Approx(2.0f));
    // The same unit with rider display 1139 (1.0): a player-sized drake.
    CHECK(rendering::remoteMountScale(1.0f, 1.0f, 1.0f) == Catch::Approx(1.0f));
    // A mount display with its own scale keeps it (panther 9991, 1.5).
    CHECK(rendering::remoteMountScale(1.0f, 1.5f, 1.0f) == Catch::Approx(1.5f));
}

TEST_CASE("A unit in view mid-way along a Flying spline flies", "[rider][spline]") {
    constexpr uint32_t kHover = 0x40000000u, kFlying = 0x02000000u;
    // Sky Stalker on its path: HOVER in its flags, Flying (0x200) on the
    // spline, three nodes, 12 s.
    CHECK(game::createBlockUnitMoveFlags(kHover, 0x200u, 3, 12000, kFlying) == (kHover | kFlying));
    // A ground spline says nothing about flight.
    CHECK(game::createBlockUnitMoveFlags(kHover, 0x0u, 3, 12000, kFlying) == kHover);
    // A finished spline (Done, 0x1) or none at all is not followed.
    CHECK(game::createBlockUnitMoveFlags(kHover, 0x201u, 3, 12000, kFlying) == kHover);
    CHECK(game::createBlockUnitMoveFlags(0u, 0x200u, 0, 0, kFlying) == 0u);
}

TEST_CASE("A character is drawn at its race display's scale", "[rider][character]") {
    // Tauren male display 59 (1.35, model 1.0), gnome male 1563 (1.15),
    // tauren female 60 (1.0, CreatureModelData 60 at 1.25), .modify scale 1.5.
    CHECK(rendering::characterRenderScale(1.0f, 1.35f, 1.0f) == Catch::Approx(1.35f));
    CHECK(rendering::characterRenderScale(1.0f, 1.15f, 1.0f) == Catch::Approx(1.15f));
    CHECK(rendering::characterRenderScale(1.0f, 1.0f, 1.25f) == Catch::Approx(1.25f));
    CHECK(rendering::characterRenderScale(1.5f, 1.0f, 1.0f) == Catch::Approx(1.5f));
    // Its horse (display 2404 at 1.0) follows: a tauren rides a 1.35 horse.
    CHECK(rendering::remoteMountScale(rendering::characterRenderScale(1.0f, 1.35f, 1.0f), 1.0f, 1.0f)
          == Catch::Approx(1.35f));
}

TEST_CASE("A dismount after a map change leaves the cleared rider alone", "[rider][teardown]") {
    // WorldLoader clears the character renderer, then resetAllState() despawns
    // the units: a mounted unit's rider instance no longer exists.
    const auto cleared = rendering::riderDismount(false, false);
    CHECK_FALSE(cleared.unseat);
    CHECK_FALSE(cleared.playIdle);
    // A creature unmounted by its death keeps its corpse pose.
    const auto dead = rendering::riderDismount(true, true);
    CHECK(dead.unseat);
    CHECK_FALSE(dead.playIdle);
    // A living rider leaves the seat for its idle.
    const auto alive = rendering::riderDismount(true, false);
    CHECK(alive.unseat);
    CHECK(alive.playIdle);
}
