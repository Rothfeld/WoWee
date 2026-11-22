#include <catch_amalgamated.hpp>
#include "rendering/animation/character_animator.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "rendering/animation/creature_locomotion.hpp"
#include "rendering/animation/stand_pose.hpp"
#include "rendering/animation/upper_body_split.hpp"
#include "game/entity.hpp"
#include "game/inventory.hpp"

using namespace wowee::rendering;

TEST_CASE("Death overrides combat and locomotion until the corpse state clears", "[animation][death]") {
    CharacterAnimator animator;
    AnimCapabilitySet caps;
    caps.resolvedStand = anim::STAND;
    animator.setCapabilities(caps);
    animator.setStunned(true);
    animator.startSpellCast(0, anim::SPELL_CAST, true, 0);
    animator.playEmote(anim::EMOTE_DANCE, true);
    CharacterAnimator::FrameInput frame;
    frame.dead = true;
    frame.diedInView = true;
    frame.moving = true;
    frame.swimming = true;
    frame.meleeSwingTimer = 1.0f;
    frame.haveAnimState = true;
    frame.currentAnimId = anim::DEATH;
    frame.currentAnimDuration = 1.0f;
    for (float time : {0.0f, 0.5f, 1.0f, 5.0f}) {
        frame.currentAnimTime = time;
        animator.setFrameInput(frame);
        animator.update(0.016f);
        REQUIRE(animator.getLastOutput().animId == anim::DEATH);
        REQUIRE_FALSE(animator.getLastOutput().loop);
    }
    frame = {};
    animator.setFrameInput(frame);
    animator.update(0.016f);
    REQUIRE(animator.getLastOutput().animId == anim::STAND);
}

TEST_CASE("A player already dead on arrival lies as Dead", "[animation][death]") {
    CharacterAnimator animator;
    AnimCapabilitySet caps;
    caps.resolvedStand = anim::STAND;
    animator.setCapabilities(caps);
    CharacterAnimator::FrameInput frame;
    frame.dead = true;  // never seen alive on this character instance
    animator.setFrameInput(frame);
    animator.update(0.016f);
    REQUIRE(animator.getLastOutput().animId == anim::DEAD);
    REQUIRE(animator.getLastOutput().loop);
}

TEST_CASE("Attack mode transitions do not draw already-held weapons", "[animation][combat]") {
    CharacterAnimator animator;
    AnimCapabilitySet caps;
    caps.resolvedStand = anim::STAND;
    caps.resolvedCombatIdle = anim::READY_1H;
    caps.resolvedReady1H = anim::READY_1H;
    caps.resolvedUnsheathe = anim::SHEATHE;
    caps.resolvedSheathe = anim::SHEATHE;
    animator.setCapabilities(caps);
    WeaponLoadout weapon;
    weapon.inventoryType = wowee::game::InvType::ONE_HAND;
    animator.setWeaponLoadout(weapon);
    CharacterAnimator::FrameInput frame;
    frame.grounded = true;
    animator.setFrameInput(frame);

    for (int repeat = 0; repeat < 3; ++repeat) {
        animator.setInCombat(true);
        animator.update(0.016f);
        REQUIRE(animator.getLastOutput().animId == anim::READY_1H);
        animator.setInCombat(false);
        animator.update(0.016f);
        REQUIRE(animator.getLastOutput().animId == anim::STAND);
    }

    // A real attachment transition asks for exactly one draw. Combat idle
    // must not truncate it, and it must not replay after completion.
    animator.setInCombat(true);
    animator.playEmote(anim::HIP_SHEATHE, false);
    frame.haveAnimState = true;
    frame.currentAnimId = anim::HIP_SHEATHE;
    frame.currentAnimDuration = 1.0f;
    frame.currentAnimTime = 0.3f;
    animator.setFrameInput(frame);
    animator.update(0.016f);
    REQUIRE(animator.getLastOutput().animId == anim::HIP_SHEATHE);
    frame.currentAnimTime = 1.0f;
    animator.setFrameInput(frame);
    animator.update(0.016f);
    REQUIRE(animator.getLastOutput().animId == anim::READY_1H);
    animator.update(0.016f);
    REQUIRE(animator.getLastOutput().animId == anim::READY_1H);
}

TEST_CASE("An aura's held animation replaces running and jumping but not a stun", "[animation][aura]") {
    // Native 2.4.3 under Whirlwind (13736): spins while running and jumping
    // (_experiments/20261117T2335Z.aura_state_locomotion).
    CharacterAnimator animator;
    AnimCapabilitySet caps;
    caps.resolvedStand = anim::STAND;
    caps.resolvedRun = anim::RUN;
    caps.resolvedJumpStart = anim::JUMP_START;
    caps.resolvedJump = anim::JUMP;
    caps.resolvedJumpEnd = anim::JUMP_END;
    caps.resolvedStun = anim::STUN;
    animator.setCapabilities(caps);
    animator.setAuraHoldAnimation(anim::WHIRLWIND);
    CharacterAnimator::FrameInput frame;
    animator.setFrameInput(frame);
    animator.update(0.016f);
    REQUIRE(animator.getLastOutput().animId == anim::WHIRLWIND);
    REQUIRE(animator.getLastOutput().loop);
    frame.moving = frame.movingForward = frame.sprinting = true;
    animator.setFrameInput(frame);
    animator.update(0.016f);
    REQUIRE(animator.getLastOutput().animId == anim::WHIRLWIND);
    frame.grounded = false;
    frame.jumping = true;
    animator.setFrameInput(frame);
    animator.update(0.016f);
    REQUIRE(animator.getLastOutput().animId == anim::WHIRLWIND);
    frame.grounded = true;
    frame.jumping = false;
    animator.setStunned(true);
    animator.setFrameInput(frame);
    animator.update(0.016f);
    REQUIRE(animator.getLastOutput().animId == anim::STUN);
    animator.setStunned(false);
    animator.setAuraHoldAnimation(0);
    animator.setFrameInput(frame);
    animator.update(0.016f);
    REQUIRE(animator.getLastOutput().animId == anim::RUN);
}

TEST_CASE("CharacterAnimator transitions immediately to Stand, Ready1H, or StealthStand on stop", "[animation][locomotion]") {
    constexpr float dt60 = 1.0f / 60.0f;
    AnimCapabilitySet caps;
    caps.resolvedStand = anim::STAND;
    caps.resolvedWalk = anim::WALK;
    caps.resolvedRun = anim::RUN;
    caps.resolvedWalkBackwards = anim::WALK_BACKWARDS;
    caps.resolvedReady1H = anim::READY_1H;
    caps.resolvedStealthIdle = anim::STEALTH_STAND;
    caps.resolvedStealthWalk = anim::STEALTH_WALK;
    caps.resolvedStealthRun = anim::STEALTH_WALK;

    // Out of combat: Run -> Stand on exact release frame
    {
        CharacterAnimator animator;
        animator.setCapabilities(caps);
        CharacterAnimator::FrameInput frame;
        frame.moving = frame.movingForward = frame.sprinting = true;
        animator.setFrameInput(frame);
        animator.update(dt60);
        REQUIRE(animator.getLastOutput().animId == anim::RUN);

        frame.moving = frame.movingForward = frame.sprinting = false;
        animator.setFrameInput(frame);
        animator.update(dt60);
        REQUIRE(animator.getLastOutput().animId == anim::STAND);
    }

    // In combat: Run -> Ready1H on exact release frame
    {
        CharacterAnimator animator;
        animator.setCapabilities(caps);
        WeaponLoadout weapon;
        weapon.inventoryType = wowee::game::InvType::ONE_HAND;
        animator.setWeaponLoadout(weapon);
        animator.setInCombat(true);

        CharacterAnimator::FrameInput frame;
        frame.moving = frame.movingForward = frame.sprinting = true;
        animator.setFrameInput(frame);
        animator.update(dt60);
        REQUIRE(animator.getLastOutput().animId == anim::RUN);

        frame.moving = frame.movingForward = frame.sprinting = false;
        animator.setFrameInput(frame);
        animator.update(dt60);
        REQUIRE(animator.getLastOutput().animId == anim::READY_1H);
    }

    // Stealthed: StealthWalk -> StealthStand on exact release frame
    {
        CharacterAnimator animator;
        animator.setCapabilities(caps);
        animator.setStealthed(true);

        CharacterAnimator::FrameInput frame;
        frame.moving = frame.movingForward = frame.sprinting = true;
        animator.setFrameInput(frame);
        animator.update(dt60);
        REQUIRE(animator.getLastOutput().animId == anim::STEALTH_WALK);

        frame.moving = frame.movingForward = frame.sprinting = false;
        animator.setFrameInput(frame);
        animator.update(dt60);
        REQUIRE(animator.getLastOutput().animId == anim::STEALTH_STAND);
    }
}

TEST_CASE("Chair stand states play low, medium, and high chair loops and stand back up cleanly", "[animation][chair][stand-state]") {
    struct ChairCase { uint8_t standState; uint32_t expectedAnim; };
    for (const ChairCase c : {
             ChairCase{ActivityFSM::STAND_STATE_SIT_CHAIR, anim::SIT_CHAIR_MED},
             ChairCase{ActivityFSM::STAND_STATE_SIT_LOW,   anim::SIT_CHAIR_LOW},
             ChairCase{ActivityFSM::STAND_STATE_SIT_MED,   anim::SIT_CHAIR_MED},
             ChairCase{ActivityFSM::STAND_STATE_SIT_HIGH,  anim::SIT_CHAIR_HIGH},
         }) {
        INFO("standState=" << static_cast<int>(c.standState));
        // Remote player standPose() mapping matches local ActivityFSM.
        const auto pose = standPose(c.standState);
        CHECK(pose.enter == 0);
        CHECK(pose.idle == c.expectedAnim);
        CHECK(pose.leave == 0);
        CHECK(isSeatedStandState(c.standState));
        CHECK(isSeatedBaseAnimation(c.expectedAnim));

        CharacterAnimator animator;
        AnimCapabilitySet caps;
        caps.resolvedStand = anim::STAND;
        animator.setCapabilities(caps);

        animator.setStandState(c.standState);
        CharacterAnimator::FrameInput frame;
        frame.grounded = true;
        frame.sitting = true;
        animator.setFrameInput(frame);
        animator.update(0.016f);
        REQUIRE(animator.getLastOutput().animId == c.expectedAnim);
        REQUIRE(animator.getLastOutput().loop);

        // Standing back up exits chair loop immediately (no sit-up transition).
        animator.setStandState(ActivityFSM::STAND_STATE_STAND);
        frame.sitting = false;
        animator.setFrameInput(frame);
        animator.update(0.016f);
        REQUIRE(animator.getLastOutput().animId == anim::STAND);
    }
}

TEST_CASE("Ground sit, sleep, and kneel play enter, loop, and exit animations", "[animation][stand-state]") {
    struct PoseCase { uint8_t state; uint32_t enter; uint32_t loop; uint32_t leave; };
    for (const PoseCase c : {
             PoseCase{ActivityFSM::STAND_STATE_SIT,   anim::SIT_GROUND_DOWN, anim::SITTING,    anim::SIT_GROUND_UP},
             PoseCase{ActivityFSM::STAND_STATE_SLEEP, anim::SLEEP_DOWN,      anim::SLEEP,      anim::SLEEP_UP},
             PoseCase{ActivityFSM::STAND_STATE_KNEEL, anim::KNEEL_START,     anim::KNEEL_LOOP, anim::KNEEL_END},
         }) {
        INFO("standState=" << static_cast<int>(c.state));
        CharacterAnimator animator;
        AnimCapabilitySet caps;
        caps.resolvedStand = anim::STAND;
        animator.setCapabilities(caps);

        animator.setStandState(c.state);
        CharacterAnimator::FrameInput frame;
        frame.grounded = true;
        frame.sitting = true;
        frame.haveAnimState = true;
        frame.currentAnimId = c.enter;
        frame.currentAnimDuration = 1.0f;
        frame.currentAnimTime = 0.2f;
        animator.setFrameInput(frame);
        animator.update(0.016f);
        REQUIRE(animator.getLastOutput().animId == c.enter);
        REQUIRE_FALSE(animator.getLastOutput().loop);

        frame.currentAnimTime = 1.0f;
        animator.setFrameInput(frame);
        animator.update(0.016f);
        REQUIRE(animator.getLastOutput().animId == c.loop);
        REQUIRE(animator.getLastOutput().loop);

        animator.setStandState(ActivityFSM::STAND_STATE_STAND);
        frame.sitting = false;
        frame.currentAnimId = c.leave;
        frame.currentAnimTime = 0.2f;
        animator.setFrameInput(frame);
        animator.update(0.016f);
        REQUIRE(animator.getLastOutput().animId == c.leave);
        REQUIRE_FALSE(animator.getLastOutput().loop);

        frame.currentAnimTime = 1.0f;
        animator.setFrameInput(frame);
        animator.update(0.016f);
        REQUIRE(animator.getLastOutput().animId == anim::STAND);
    }
}

TEST_CASE("Stealthed spell cast plays cast animation instead of StealthStand", "[animation][stealth][pickpocket]") {
    AnimCapabilitySet caps;
    caps.resolvedStand = anim::STAND;
    caps.resolvedWalk = anim::WALK;
    caps.resolvedRun = anim::RUN;
    caps.resolvedStealthIdle = anim::STEALTH_STAND;
    caps.resolvedStealthWalk = anim::STEALTH_WALK;
    caps.resolvedStealthRun = anim::STEALTH_RUN;

    CharacterAnimator animator;
    animator.setCapabilities(caps);
    animator.setStealthed(true);

    CharacterAnimator::FrameInput frame;
    frame.grounded = true;
    frame.haveAnimState = true;
    frame.currentAnimId = anim::STEALTH_STAND;
    frame.currentAnimDuration = 1333.0f;
    frame.currentAnimTime = 200.0f;

    animator.setFrameInput(frame);
    animator.update(0.016f);
    REQUIRE(animator.getLastOutput().animId == anim::STEALTH_STAND);
    REQUIRE(animator.getLastOutput().loop);

    // Pick Pocket (spell 921, SpellVisual 251 -> castKit 396 -> UseStandingStart 128).
    animator.startSpellCast(0, anim::USE_STANDING_START, false, anim::USE_STANDING_START);
    animator.stopSpellCast();

    animator.setFrameInput(frame);
    animator.update(0.016f);
    REQUIRE(animator.getLastOutput().animId == anim::USE_STANDING_START);
    REQUIRE_FALSE(animator.getLastOutput().loop);

    // Mid-cast frame keeps playing UseStandingStart.
    frame.currentAnimId = anim::USE_STANDING_START;
    frame.currentAnimDuration = 600.0f;
    frame.currentAnimTime = 250.0f;
    animator.setFrameInput(frame);
    animator.update(0.016f);
    REQUIRE(animator.getLastOutput().animId == anim::USE_STANDING_START);
    REQUIRE_FALSE(animator.getLastOutput().loop);

    // Once UseStandingStart finishes, returns to StealthStand (not kneel/loot).
    frame.currentAnimTime = 600.0f;
    animator.setFrameInput(frame);
    animator.update(0.016f);
    REQUIRE(animator.getLastOutput().animId == anim::STEALTH_STAND);
    REQUIRE(animator.getLastOutput().loop);
}

TEST_CASE("Stealth locomotion parity: StealthRun/Walk, Sprint, Walkbackwards, Strafe, Turn, Jump, and fallback", "[animation][stealth]") {
    constexpr float dt60 = 1.0f / 60.0f;
    AnimCapabilitySet dwarfCaps;
    dwarfCaps.resolvedStand = anim::STAND;
    dwarfCaps.resolvedWalk = anim::WALK;
    dwarfCaps.resolvedRun = anim::RUN;
    dwarfCaps.resolvedSprint = anim::SPRINT;
    dwarfCaps.resolvedWalkBackwards = anim::WALK_BACKWARDS;
    dwarfCaps.resolvedJumpStart = anim::JUMP_START;
    dwarfCaps.resolvedJump = anim::JUMP;
    dwarfCaps.resolvedJumpEnd = anim::JUMP_END;
    dwarfCaps.resolvedStealthIdle = anim::STEALTH_STAND;
    dwarfCaps.resolvedStealthWalk = anim::STEALTH_WALK;
    dwarfCaps.resolvedStealthRun = anim::STEALTH_RUN;
    dwarfCaps.resolvedStun = anim::STUN;

    CharacterAnimator animator;
    animator.setCapabilities(dwarfCaps);
    animator.setStealthed(true);

    // 1. Idle -> StealthStand (120)
    CharacterAnimator::FrameInput frame;
    frame.grounded = true;
    animator.setFrameInput(frame);
    animator.update(dt60);
    CHECK(animator.getLastOutput().animId == anim::STEALTH_STAND);

    // 2. Forward run in stealth -> StealthRun (223)
    frame.moving = frame.movingForward = frame.sprinting = true;
    animator.setFrameInput(frame);
    animator.update(dt60);
    CHECK(animator.getLastOutput().animId == anim::STEALTH_RUN);

    // 3. Forward run in stealth with Sprint aura -> stays crouched in StealthRun (223), not upright Sprint (143)
    animator.setSprintAuraActive(true);
    animator.setFrameInput(frame);
    animator.update(dt60);
    CHECK(animator.getLastOutput().animId == anim::STEALTH_RUN);
    animator.setSprintAuraActive(false);

    // 4. RP-Walk toggle forward in stealth -> StealthWalk (119)
    frame.sprinting = false;
    animator.setFrameInput(frame);
    animator.update(dt60);
    CHECK(animator.getLastOutput().animId == anim::STEALTH_WALK);

    // 5. Backwards in stealth (run or walk mode) -> Walkbackwards (13), matching Native 2.4.3
    frame.movingForward = false;
    frame.movingBackward = true;
    frame.sprinting = true;
    animator.setFrameInput(frame);
    animator.update(dt60);
    CHECK(animator.getLastOutput().animId == anim::WALK_BACKWARDS);
    frame.sprinting = false;
    animator.setFrameInput(frame);
    animator.update(dt60);
    CHECK(animator.getLastOutput().animId == anim::WALK_BACKWARDS);

    // 6. Strafe right in stealth -> StealthRun (223)
    frame.movingBackward = false;
    frame.strafeRight = true;
    frame.sprinting = true;
    animator.setFrameInput(frame);
    animator.update(dt60);
    CHECK(animator.getLastOutput().animId == anim::STEALTH_RUN);

    // 7. Turn in place while stealthed -> ShuffleRight (12), then back to StealthStand (120)
    frame.moving = frame.strafeRight = frame.sprinting = false;
    animator.setFrameInput(frame);
    animator.update(dt60);
    CHECK(animator.getLastOutput().animId == anim::STEALTH_STAND);
    frame.turnDirection = -1;
    animator.setFrameInput(frame);
    animator.update(dt60);
    CHECK(animator.getLastOutput().animId == anim::SHUFFLE_RIGHT);
    frame.turnDirection = 0;
    frame.haveAnimState = true;
    frame.currentAnimId = anim::SHUFFLE_RIGHT;
    frame.currentAnimDuration = 800.0f;
    // First cycle ends (turnedThisCycle_ was true), second cycle completes with no turn -> IDLE
    for (float t : {600.0f, 50.0f, 600.0f, 50.0f}) {
        frame.currentAnimTime = t;
        animator.setFrameInput(frame);
        animator.update(dt60);
    }
    CHECK(animator.getLastOutput().animId == anim::STEALTH_STAND);

    // 8. Jump while stealthed -> JumpStart (37), not clobbered by StealthStand
    frame = {};
    frame.grounded = false;
    frame.jumping = true;
    animator.setFrameInput(frame);
    animator.update(dt60);
    CHECK(animator.getLastOutput().animId == anim::JUMP_START);

    // 9. Active stun while stealthed -> Stun (14), not clobbered by StealthStand
    frame = {};
    frame.grounded = true;
    animator.setStunned(true);
    animator.setFrameInput(frame);
    animator.update(dt60);
    CHECK(animator.getLastOutput().animId == anim::STUN);
    animator.setStunned(false);

    // 10. Model without stealth sequences (e.g. DruidCat.m2) keeps Stand/Walk/Run
    AnimCapabilitySet catCaps;
    catCaps.resolvedStand = anim::STAND;
    catCaps.resolvedWalk = anim::WALK;
    catCaps.resolvedRun = anim::RUN;
    CharacterAnimator catAnim;
    catAnim.setCapabilities(catCaps);
    catAnim.setStealthed(true);
    frame = {};
    frame.grounded = true;
    catAnim.setFrameInput(frame);
    catAnim.update(dt60);
    CHECK(catAnim.getLastOutput().animId == anim::STAND);
    frame.moving = frame.movingForward = frame.sprinting = true;
    catAnim.setFrameInput(frame);
    catAnim.update(dt60);
    CHECK(catAnim.getLastOutput().animId == anim::RUN);
}

TEST_CASE("Stealth locomotion playback rate scaling, remote moving selection, upper-body split, and CREEP flag", "[animation][stealth]") {
    // Real TBC 2.4.3 DwarfMale.m2 moveSpeed values:
    //   StealthWalk (119) = 2.500 yd/s, StealthRun (223) = 3.889 yd/s,
    //   Sprint (143) = 11.667 yd/s, Walkbackwards (13) = -2.500 yd/s.
    CHECK(isGroundLocomotionScaledAnimation(anim::WALK));
    CHECK(isGroundLocomotionScaledAnimation(anim::RUN));
    CHECK(isGroundLocomotionScaledAnimation(anim::WALK_BACKWARDS));
    CHECK(isGroundLocomotionScaledAnimation(anim::STEALTH_WALK));
    CHECK(isGroundLocomotionScaledAnimation(anim::STEALTH_RUN));
    CHECK(isGroundLocomotionScaledAnimation(anim::SPRINT));
    CHECK_FALSE(isGroundLocomotionScaledAnimation(anim::STEALTH_STAND));
    CHECK_FALSE(isGroundLocomotionScaledAnimation(anim::STAND));

    // 70% Stealth (4.9 yd/s) on DwarfMale StealthRun (3.889) -> 1.26x
    CHECK(computeLocomotionPlaybackRate(anim::STEALTH_RUN, 4.9f, 3.889f, 1.0f) == Catch::Approx(4.9f / 3.889f).epsilon(0.001));
    // Stealth + Sprint (8.33 yd/s) on DwarfMale StealthRun (3.889) -> 2.14x
    CHECK(computeLocomotionPlaybackRate(anim::STEALTH_RUN, 8.33f, 3.889f, 1.0f) == Catch::Approx(8.33f / 3.889f).epsilon(0.001));
    // RP-walk in Stealth (1.75 yd/s) on StealthWalk (2.500) -> 0.70x
    CHECK(computeLocomotionPlaybackRate(anim::STEALTH_WALK, 1.75f, 2.5f, 1.0f) == Catch::Approx(0.7f).epsilon(0.001));
    // Negative moveSpeed (Walkbackwards -2.5) uses abs() and divides by scale
    CHECK(computeLocomotionPlaybackRate(anim::WALK_BACKWARDS, 4.5f, -2.5f, 1.2f) == Catch::Approx(4.5f / 3.0f).epsilon(0.001));
    // Stationary sequence (moveSpeed 0) or non-locomotion stays 1.0x
    CHECK(computeLocomotionPlaybackRate(anim::STEALTH_STAND, 4.9f, 0.0f, 1.0f) == Catch::Approx(1.0f));
    CHECK(computeLocomotionPlaybackRate(anim::STEALTH_RUN, -1.0f, 3.889f, 1.0f) == Catch::Approx(1.0f));

    // Remote moving animation selection with stealth flag
    CHECK(creatureMovingAnimation(false, false, false, true) == anim::STEALTH_RUN);
    CHECK(creatureMovingAnimation(false, false, true, true) == anim::STEALTH_WALK);
    CHECK(creatureMovingAnimation(false, true, false, true) == anim::SWIM);
    CHECK(creatureMovingAnimation(true, false, false, true) == anim::FLY_FORWARD);
    CHECK(creatureMovingAnimation(false, false, false, false) == anim::RUN);
    CHECK(creatureMovingAnimation(false, false, true, false) == anim::WALK);

    // Upper-body split recognizes StealthWalk and StealthRun as locomotion bases
    CHECK(isLocomotionBaseAnimation(anim::STEALTH_WALK, 0));
    CHECK(isLocomotionBaseAnimation(anim::STEALTH_RUN, 0));

    // UNIT_FIELD_BYTES_1 byte 2 (UNIT_VIS_FLAG_CREEP = 0x02) decoding on Unit
    wowee::game::Unit unit(0x1234);
    CHECK_FALSE(unit.hasCreepVisibility());
    const uint32_t bytes1WithCreep = static_cast<uint32_t>(wowee::game::UNIT_VIS_FLAG_CREEP) << 16;
    unit.setVisibilityFlags(static_cast<uint8_t>((bytes1WithCreep >> 16) & 0xFF));
    CHECK(unit.hasCreepVisibility());
    unit.clearCreepVisibility();
    CHECK_FALSE(unit.hasCreepVisibility());
}

TEST_CASE("TBC 2.4.3 speed scaling & sequence parity across 0.5x..10x speeds, model scales, shapeshifts, mounts, swim, and flight", "[animation][speed]") {
    constexpr float dt60 = 1.0f / 60.0f;
    constexpr float kWalkMove = 2.5f;
    constexpr float kRunMove = 250.0f / 36.0f;   // 6.944444 yd/s
    constexpr float kSprintMove = 35.0f / 3.0f;  // 11.666667 yd/s

    // 1) Exact AnimationData.dbc BodyFlags & 0x40 scaling membership:
    // Ground (4, 5, 13, 119, 223, 143) + Swim (42, 43, 44, 45) scale; Flight (135, 193) does NOT.
    for (uint32_t scaledId : {anim::WALK, anim::RUN, anim::WALK_BACKWARDS,
                              anim::STEALTH_WALK, anim::STEALTH_RUN, anim::SPRINT,
                              anim::SWIM, anim::SWIM_LEFT, anim::SWIM_RIGHT, anim::SWIM_BACKWARDS}) {
        CHECK(isLocomotionSpeedScaledAnimation(scaledId));
    }
    for (uint32_t unscaledId : {anim::STAND, anim::JUMP_START, anim::JUMP, anim::JUMP_END,
                                anim::SWIM_IDLE, anim::FLY_FORWARD, anim::FLY_IDLE, anim::MOUNT}) {
        CHECK_FALSE(isLocomotionSpeedScaledAnimation(unscaledId));
    }

    // Swim scales linearly across 1x..5x; FlyForward stays 1.0x even when seqMovingSpeed > 0
    // (gryphon.m2 Fly movingSpeed = 30.0, or HumanMale Fly 135 -> Swim 42 stand-in).
    constexpr float kSwimMove = 170.0f / 36.0f; // 4.722222 yd/s
    CHECK(computeLocomotionPlaybackRate(anim::SWIM, kSwimMove, kSwimMove, 1.0f) == Catch::Approx(1.0f).epsilon(0.001));
    CHECK(computeLocomotionPlaybackRate(anim::SWIM, kSwimMove * 2.0f, kSwimMove, 1.0f) == Catch::Approx(2.0f).epsilon(0.001));
    CHECK(computeLocomotionPlaybackRate(anim::SWIM, kSwimMove * 5.0f, kSwimMove, 1.0f) == Catch::Approx(5.0f).epsilon(0.001));
    CHECK(computeLocomotionPlaybackRate(anim::SWIM_BACKWARDS, 2.5f * 5.0f, 2.5f, 1.0f) == Catch::Approx(5.0f).epsilon(0.001));
    CHECK(computeLocomotionPlaybackRate(anim::FLY_FORWARD, 35.0f, 30.0f, 1.0f) == Catch::Approx(1.0f));
    CHECK(computeLocomotionPlaybackRate(anim::FLY_FORWARD, 35.0f, kSwimMove, 1.0f) == Catch::Approx(1.0f));

    // 2) Tauren Male (scale = 1.35, Walk/Run/Sprint dur = 1067/667/667 ms):
    // Reproduces user report: .modify speed 5 (35.0 yd/s) selects SPRINT (143)
    // at 300.15 ms cycle instead of staying in RUN (5) at 178.66 ms cycle!
    {
        constexpr float kTaurenScale = 1.35f;
        AnimCapabilitySet taurenCaps;
        taurenCaps.resolvedStand = anim::STAND;
        taurenCaps.resolvedWalk = anim::WALK;
        taurenCaps.resolvedRun = anim::RUN;
        taurenCaps.resolvedSprint = anim::SPRINT;
        taurenCaps.hasStand = taurenCaps.hasWalk = taurenCaps.hasRun = taurenCaps.hasSprint = true;

        auto evalTauren = [&](float mult, bool runMode = true) {
            CharacterAnimator anim;
            anim.setCapabilities(taurenCaps);
            CharacterAnimator::FrameInput fi;
            fi.moving = fi.movingForward = true;
            fi.sprinting = runMode;
            fi.locomotionSpeed = (runMode ? 7.0f : 2.5f) * mult;
            fi.walkStride = kWalkMove * kTaurenScale;
            fi.runStride = kRunMove * kTaurenScale;
            anim.setFrameInput(fi);
            anim.update(dt60);
            return std::make_pair(anim.getLastOutput().animId, fi.locomotionSpeed);
        };

        // 0.5x (3.5 yd/s < 6.75 yd/s) -> WALK (4)
        auto [id05, spd05] = evalTauren(0.5f);
        CHECK(id05 == anim::WALK);
        CHECK(1067.0f / computeLocomotionPlaybackRate(id05, spd05, kWalkMove, kTaurenScale) ==
              Catch::Approx(1028.89f).epsilon(0.002));

        // 1.0x (7.0 yd/s) -> RUN (5), 893.3 ms cycle
        auto [id10, spd10] = evalTauren(1.0f);
        CHECK(id10 == anim::RUN);
        CHECK(667.0f / computeLocomotionPlaybackRate(id10, spd10, kRunMove, kTaurenScale) ==
              Catch::Approx(893.30f).epsilon(0.002));

        // 2.0x (14.0 yd/s < 14.8125 yd/s) -> stays RUN (5) on Tauren 1.35x, 446.65 ms cycle
        auto [id20, spd20] = evalTauren(2.0f);
        CHECK(id20 == anim::RUN);
        CHECK(667.0f / computeLocomotionPlaybackRate(id20, spd20, kRunMove, kTaurenScale) ==
              Catch::Approx(446.65f).epsilon(0.002));

        // 5.0x (35.0 yd/s >= 14.8125 yd/s) -> promotes to SPRINT (143), 300.15 ms cycle
        auto [id50, spd50] = evalTauren(5.0f);
        CHECK(id50 == anim::SPRINT);
        CHECK(667.0f / computeLocomotionPlaybackRate(id50, spd50, kSprintMove, kTaurenScale) ==
              Catch::Approx(300.15f).epsilon(0.002));

        // 10.0x (70.0 yd/s) -> SPRINT (143), 150.08 ms cycle (linear, unclamped)
        auto [id100, spd100] = evalTauren(10.0f);
        CHECK(id100 == anim::SPRINT);
        CHECK(667.0f / computeLocomotionPlaybackRate(id100, spd100, kSprintMove, kTaurenScale) ==
              Catch::Approx(150.08f).epsilon(0.002));
    }

    // 3) Scale interactions: HumanMale scale 0.5x & 2.0x, RidingKodo scale 0.60x
    {
        const auto gaitAtScale = [&](float scale, float speed, bool hasSprint = true) {
            return selectGroundForwardGaitAnimation(
                false, speed, kWalkMove * scale, kRunMove * scale, hasSprint, false);
        };
        // Scale 2.0: walk->run at 10.0 yd/s, run->sprint at ~21.94 yd/s
        CHECK(gaitAtScale(2.0f, 7.0f)  == anim::WALK);
        CHECK(gaitAtScale(2.0f, 14.0f) == anim::RUN);
        CHECK(gaitAtScale(2.0f, 35.0f) == anim::SPRINT);
        // Scale 0.5: run->sprint at ~5.49 yd/s -> 1.0x (7.0 yd/s) promotes to SPRINT
        CHECK(gaitAtScale(0.5f, 7.0f)  == anim::SPRINT);
        // RidingKodo (scale 0.60): 1.0x (7.0 yd/s) and 2.0x (14.0 yd/s) select SPRINT
        CHECK(gaitAtScale(0.60f, 7.0f)  == anim::SPRINT);
        CHECK(gaitAtScale(0.60f, 14.0f) == anim::SPRINT);
    }

    // 4) Models without SPRINT (DruidCatTauren, DruidBearTauren, RidingHorse):
    // hasSprint=false keeps RUN (5) at 2x..10x; Prowl Cat (no StealthWalk/Run) stalks in WALK at 4.9 yd/s.
    {
        AnimCapabilitySet catCaps;
        catCaps.resolvedStand = anim::STAND;
        catCaps.resolvedWalk = anim::WALK;
        catCaps.resolvedRun = anim::RUN;
        catCaps.resolvedSprint = anim::RUN; // fallback only; model has no sequence 143
        catCaps.hasStand = catCaps.hasWalk = catCaps.hasRun = true;
        catCaps.hasSprint = false;

        CharacterAnimator cat;
        cat.setCapabilities(catCaps);
        CharacterAnimator::FrameInput fi;
        fi.moving = fi.movingForward = fi.sprinting = true;
        fi.walkStride = 10.0f / 3.0f; // DruidCat Walk = 3.3333 yd/s
        fi.runStride = 10.0f;         // DruidCat Run = 10.0 yd/s

        for (float spd : {7.0f, 14.0f, 35.0f, 70.0f}) {
            fi.locomotionSpeed = spd;
            cat.setFrameInput(fi);
            cat.update(dt60);
            CHECK(cat.getLastOutput().animId == anim::RUN);
        }
        CHECK(computeLocomotionPlaybackRate(anim::RUN, 35.0f, 10.0f, 1.0f) == Catch::Approx(3.5f).epsilon(0.001));

        // Cat Form Prowl (4.9 yd/s < 2 * 3.3333 = 6.67 yd/s) stalks in WALK (4), Dash (11.9 yd/s) runs in RUN (5)
        cat.setStealthed(true);
        fi.locomotionSpeed = 4.9f;
        cat.setFrameInput(fi);
        cat.update(dt60);
        CHECK(cat.getLastOutput().animId == anim::WALK);
        fi.locomotionSpeed = 11.9f;
        cat.setFrameInput(fi);
        cat.update(dt60);
        CHECK(cat.getLastOutput().animId == anim::RUN);
    }

    // 5) Humanoid Stealth across 0.5x..5.0x keeps STEALTH_RUN in Run mode and STEALTH_WALK in Walk mode
    {
        AnimCapabilitySet rogueCaps;
        rogueCaps.resolvedStand = anim::STAND;
        rogueCaps.resolvedWalk = anim::WALK;
        rogueCaps.resolvedRun = anim::RUN;
        rogueCaps.resolvedSprint = anim::SPRINT;
        rogueCaps.resolvedStealthIdle = anim::STEALTH_STAND;
        rogueCaps.resolvedStealthWalk = anim::STEALTH_WALK;
        rogueCaps.resolvedStealthRun = anim::STEALTH_RUN;
        rogueCaps.hasStand = rogueCaps.hasWalk = rogueCaps.hasRun = rogueCaps.hasSprint = rogueCaps.hasStealth = true;

        CharacterAnimator rogue;
        rogue.setCapabilities(rogueCaps);
        rogue.setStealthed(true);
        CharacterAnimator::FrameInput fi;
        fi.moving = fi.movingForward = true;
        fi.walkStride = kWalkMove;
        fi.runStride = kRunMove;

        for (float spd : {2.45f, 4.90f, 9.80f, 24.50f}) {
            fi.sprinting = true;
            fi.locomotionSpeed = spd;
            rogue.setFrameInput(fi);
            rogue.update(dt60);
            CHECK(rogue.getLastOutput().animId == anim::STEALTH_RUN);
        }
        for (float spd : {1.75f, 3.50f, 12.50f}) {
            fi.sprinting = false;
            fi.locomotionSpeed = spd;
            rogue.setFrameInput(fi);
            rogue.update(dt60);
            CHECK(rogue.getLastOutput().animId == anim::STEALTH_WALK);
        }
    }

    // 6) Unmounted flight in combat: flying suppresses ground COMBAT_IDLE so Hover/Fly play cleanly
    {
        AnimCapabilitySet caps;
        caps.resolvedStand = anim::STAND;
        caps.resolvedRun = anim::RUN;
        caps.resolvedCombatIdle = anim::READY_1H;
        caps.resolvedReady1H = anim::READY_1H;
        caps.hasFly = true;
        CharacterAnimator flyer;
        flyer.setCapabilities(caps);
        WeaponLoadout weapon;
        weapon.inventoryType = wowee::game::InvType::ONE_HAND;
        flyer.setWeaponLoadout(weapon);
        flyer.setInCombat(true);

        CharacterAnimator::FrameInput fi;
        fi.grounded = true; // CameraController sets grounded=true while flightAirborne_
        fi.flying = true;
        flyer.setFrameInput(fi);
        flyer.update(dt60);
        CHECK(flyer.getLastOutput().animId == anim::FLY_IDLE);

        fi.moving = fi.movingForward = fi.sprinting = true;
        fi.locomotionSpeed = 35.0f;
        flyer.setFrameInput(fi);
        flyer.update(dt60);
        CHECK(flyer.getLastOutput().animId == anim::FLY_FORWARD);

        // Non-flying humanoid in combat (e.g. Kael'thas Gravity Lapse): suppresses READY_1H
        // and plays SWIM_IDLE / SWIM when caps.hasFly is false.
        caps.hasFly = false;
        flyer.setCapabilities(caps);
        fi.moving = fi.movingForward = fi.sprinting = false;
        fi.locomotionSpeed = 0.0f;
        flyer.setFrameInput(fi);
        flyer.update(dt60);
        CHECK(flyer.getLastOutput().animId == anim::SWIM_IDLE);

        fi.moving = fi.movingForward = true;
        fi.locomotionSpeed = 7.0f;
        flyer.setFrameInput(fi);
        flyer.update(dt60);
        CHECK(flyer.getLastOutput().animId == anim::SWIM);
    }
}
