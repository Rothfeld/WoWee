// Mounted units: the rider's seat, its mount's size and a flying unit's
// create-block spline. Native observations (2.4.3.8606), Dragonmaw Wind
// Reaver 23330 and Sky Stalker 23030 tempspawned in Nagrand:
// _experiments/20261118T150000Z.npc_mount in the workspace.
#include <catch_amalgamated.hpp>

#include "game/spline_flight.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "rendering/animation/rider_pose.hpp"
#include "rendering/character_scale.hpp"

#include <glm/gtc/matrix_transform.hpp>

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

TEST_CASE("A rider's weapons follow its swing, else its seat", "[rider][sheath]") {
    // Native Bonechewer Raider / Amani'shi Warbringer at sheath state 1:
    // axes on the back while seated (Mount 91, WeaponFlags 4), in hand for
    // each Attack1H (17) or AttackOff (87) above the spine, on the back again
    // after it (_experiments/20261118T231500Z.rider_attack).
    CHECK(rendering::riderWeaponPoseAnimation(0, anim::MOUNT) == anim::MOUNT);
    CHECK(rendering::riderWeaponPoseAnimation(17, anim::MOUNT) == 17u);
    CHECK(rendering::riderWeaponPoseAnimation(87, anim::MOUNT) == 87u);
    // A cast above the seat (SpellCastDirected 53, WeaponFlags 4) decides too.
    CHECK(rendering::riderWeaponPoseAnimation(53, anim::MOUNT) == 53u);
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

TEST_CASE("A rider sits on its mount's MountMain as the mount's pose carries it", "[rider]") {
    // White tiger 6080 (RidingFrostSabre, display 1.5) under Tyrande (2.0):
    // drawn at 3.0, facing 30 degrees. Its Stand pose carries MountMain to
    // (-0.663, 0.039, 1.026), pitched about 6 degrees nose down.
    const float yaw = glm::radians(30.0f), pitch = glm::radians(-6.4f), roll = glm::radians(1.5f);
    const glm::vec3 mountAt(100.0f, 200.0f, 10.0f);
    glm::mat4 mount = glm::translate(glm::mat4(1.0f), mountAt);
    mount = glm::rotate(mount, yaw, glm::vec3(0, 0, 1));
    mount = glm::scale(mount, glm::vec3(3.0f));
    glm::mat4 bone = glm::rotate(glm::mat4(1.0f), pitch, glm::vec3(1, 0, 0));
    bone = glm::rotate(bone, roll, glm::vec3(0, 1, 0));
    const glm::vec3 offset(-0.663f, 0.039f, 1.026f);
    const glm::mat4 seat = mount * glm::translate(glm::mat4(1.0f), offset) * bone;

    const auto pose = rendering::riderSeatPose(seat);
    const glm::vec3 expected = mountAt + glm::vec3(glm::rotate(glm::mat4(1.0f), yaw, glm::vec3(0, 0, 1)) *
                                                   glm::vec4(offset * 3.0f, 0.0f));
    CHECK(pose.position.x == Catch::Approx(expected.x).margin(1e-4));
    CHECK(pose.position.y == Catch::Approx(expected.y).margin(1e-4));
    CHECK(pose.position.z == Catch::Approx(expected.z).margin(1e-4));
    // The mount's scale stays with the mount: the rotation alone, as
    // CharacterRenderer's yaw * pitch * roll.
    CHECK(pose.rotation.x == Catch::Approx(pitch).margin(1e-5));
    CHECK(pose.rotation.y == Catch::Approx(roll).margin(1e-5));
    CHECK(pose.rotation.z == Catch::Approx(yaw).margin(1e-5));
}

TEST_CASE("A level seat turns the rider with its mount only", "[rider]") {
    // Riding horse 2410: MountMain (0.213, 0, 1.866) on an unanimated bone.
    const glm::mat4 seat = glm::translate(glm::rotate(glm::mat4(1.0f), glm::radians(-120.0f), glm::vec3(0, 0, 1)),
                                          glm::vec3(0.213f, 0.0f, 1.866f));
    const auto pose = rendering::riderSeatPose(seat);
    CHECK(pose.position.z == Catch::Approx(1.866f));
    CHECK(pose.rotation.x == Catch::Approx(0.0f).margin(1e-6));
    CHECK(pose.rotation.y == Catch::Approx(0.0f).margin(1e-6));
    CHECK(pose.rotation.z == Catch::Approx(glm::radians(-120.0f)).margin(1e-5));
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
