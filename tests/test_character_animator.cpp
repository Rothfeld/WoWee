#include <catch_amalgamated.hpp>
#include "rendering/animation/character_animator.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "rendering/animation/stand_pose.hpp"
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
