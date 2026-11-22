#include <catch_amalgamated.hpp>
#include "rendering/animation/possessed_locomotion.hpp"

using namespace wowee::rendering;

namespace {

AnimCapabilitySet makeDrakeCaps() {
    // Creature\Drake\Drake.m2 (Power of the Blue Flight, entry 25653, displayId 13971):
    // has STAND (0), WALK (4), RUN (5), SPELL_CAST_DIRECTED (53), SPELL_CAST_OMNI (54),
    // but no JUMP_START/JUMP/JUMP_END or WALK_BACKWARDS.
    AnimCapabilitySet caps;
    caps.hasStand = true;
    caps.hasWalk = true;
    caps.hasRun = true;
    caps.hasWalkBackwards = true; // fallback to WALK
    caps.resolvedStand = anim::STAND;
    caps.resolvedWalk = anim::WALK;
    caps.resolvedRun = anim::RUN;
    caps.resolvedWalkBackwards = anim::WALK;
    caps.resolvedJumpStart = 0;
    caps.resolvedJump = 0;
    caps.resolvedJumpEnd = 0;
    return caps;
}

AnimCapabilitySet makeHumanoidCaps() {
    AnimCapabilitySet caps;
    caps.hasStand = true;
    caps.hasWalk = true;
    caps.hasRun = true;
    caps.hasWalkBackwards = true;
    caps.hasJump = true;
    caps.resolvedStand = anim::STAND;
    caps.resolvedWalk = anim::WALK;
    caps.resolvedRun = anim::RUN;
    caps.resolvedWalkBackwards = anim::WALK_BACKWARDS;
    caps.resolvedJumpStart = anim::JUMP_START;
    caps.resolvedJump = anim::JUMP;
    caps.resolvedJumpEnd = anim::JUMP_END;
    return caps;
}

} // namespace

TEST_CASE("PossessedLocomotionDriver drives walk, run, and backpedal on Drake.m2", "[possess][locomotion]") {
    PossessedLocomotionDriver driver;
    driver.bind(0xf130006435596f40ULL, 325u, makeDrakeCaps());

    PossessedLocomotionDriver::Input in;
    in.grounded = true;
    in.haveAnimState = true;
    in.currentAnimId = anim::STAND;

    // Standing idle -> STAND
    auto d = driver.step(in);
    REQUIRE(d.shouldPlay);
    REQUIRE(d.animId == anim::STAND);
    REQUIRE(d.loop);

    // Running forward -> RUN (5)
    in.moving = true;
    in.sprinting = true;
    in.movingForward = true;
    d = driver.step(in);
    REQUIRE(d.shouldPlay);
    REQUIRE(d.animId == anim::RUN);
    REQUIRE(d.loop);

    // Staying in RUN does not re-trigger
    in.currentAnimId = anim::RUN;
    d = driver.step(in);
    REQUIRE_FALSE(d.shouldPlay);

    // Toggle walk -> WALK (4)
    in.sprinting = false;
    d = driver.step(in);
    REQUIRE(d.shouldPlay);
    REQUIRE(d.animId == anim::WALK);
    REQUIRE(d.loop);

    // Backpedal on Drake.m2 falls back to WALK (4)
    in.currentAnimId = anim::WALK;
    in.movingForward = false;
    in.movingBackward = true;
    in.sprinting = true;
    d = driver.step(in);
    REQUIRE_FALSE(d.shouldPlay);
    REQUIRE(d.animId == anim::WALK);

    // Stop moving -> STAND (0)
    in.moving = false;
    in.movingBackward = false;
    d = driver.step(in);
    REQUIRE(d.shouldPlay);
    REQUIRE(d.animId == anim::STAND);
}

TEST_CASE("PossessedLocomotionDriver handles jump without re-triggering fallback on Drake.m2", "[possess][locomotion]") {
    PossessedLocomotionDriver driver;
    driver.bind(0xf130006435596f40ULL, 325u, makeDrakeCaps());

    PossessedLocomotionDriver::Input in;
    in.grounded = true;
    in.haveAnimState = true;
    in.currentAnimId = anim::STAND;
    driver.step(in);

    // Jump on Drake.m2 (no JUMP_START/JUMP/JUMP_END) resolves to STAND without per-frame drift
    in.grounded = false;
    in.jumping = true;
    auto d = driver.step(in);
    REQUIRE(d.animId == anim::STAND);
    REQUIRE_FALSE(d.shouldPlay);

    // Next airborne frame still does not re-trigger
    in.jumping = false;
    d = driver.step(in);
    REQUIRE_FALSE(d.shouldPlay);
}

TEST_CASE("PossessedLocomotionDriver plays full jump sequence on humanoid possessed unit", "[possess][locomotion]") {
    PossessedLocomotionDriver driver;
    driver.bind(0xf130001234567890ULL, 42u, makeHumanoidCaps());

    PossessedLocomotionDriver::Input in;
    in.grounded = true;
    in.haveAnimState = true;
    in.currentAnimId = anim::STAND;
    driver.step(in);

    // Jump takeoff -> JUMP_START (37), one-shot
    in.grounded = false;
    in.jumping = true;
    auto d = driver.step(in);
    REQUIRE(d.shouldPlay);
    REQUIRE(d.animId == anim::JUMP_START);
    REQUIRE_FALSE(d.loop);

    // JUMP_START completes -> JUMP (38), looping
    in.jumping = false;
    in.currentAnimId = anim::JUMP_START;
    in.currentAnimDuration = 0.5f;
    in.currentAnimTime = 0.48f;
    d = driver.step(in);
    REQUIRE(d.shouldPlay);
    REQUIRE(d.animId == anim::JUMP);
    REQUIRE(d.loop);

    // Land -> JUMP_END (39), one-shot
    in.grounded = true;
    in.currentAnimId = anim::JUMP;
    d = driver.step(in);
    REQUIRE(d.shouldPlay);
    REQUIRE(d.animId == anim::JUMP_END);
    REQUIRE_FALSE(d.loop);
}

TEST_CASE("PossessedLocomotionDriver preserves active cast one-shot while idle", "[possess][locomotion]") {
    PossessedLocomotionDriver driver;
    driver.bind(0xf130006435596f40ULL, 325u, makeDrakeCaps());

    PossessedLocomotionDriver::Input in;
    in.grounded = true;
    in.haveAnimState = true;
    in.currentAnimId = anim::STAND;
    driver.step(in);

    // Dragon casts Breath: Haste (anim 53 SPELL_CAST_DIRECTED) mid-play
    in.currentAnimId = anim::SPELL_CAST_DIRECTED;
    in.currentAnimDuration = 1.0f;
    in.currentAnimTime = 0.3f;
    auto d = driver.step(in);
    REQUIRE_FALSE(d.shouldPlay);

    // Once cast finishes, returns to STAND
    in.currentAnimTime = 0.98f;
    d = driver.step(in);
    REQUIRE(d.shouldPlay);
    REQUIRE(d.animId == anim::STAND);
}

TEST_CASE("PossessedLocomotionDriver never overrides corpse poses", "[possess][locomotion]") {
    PossessedLocomotionDriver driver;
    driver.bind(0xf130006435596f40ULL, 325u, makeDrakeCaps());

    PossessedLocomotionDriver::Input in;
    in.dead = true;
    in.haveAnimState = true;
    in.currentAnimId = anim::DEATH;
    auto d = driver.step(in);
    REQUIRE_FALSE(d.shouldPlay);
}
