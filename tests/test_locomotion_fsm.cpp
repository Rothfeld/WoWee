// LocomotionFSM unit tests
#include <catch_amalgamated.hpp>
#include <cmath>
#include "rendering/animation/locomotion_fsm.hpp"
#include "rendering/animation/animation_ids.hpp"

using namespace wowee::rendering;
namespace anim = wowee::rendering::anim;

// Helper: create a capability set with basic locomotion resolved
static AnimCapabilitySet makeLocoCaps() {
    AnimCapabilitySet caps;
    caps.resolvedStand = anim::STAND;
    caps.resolvedWalk = anim::WALK;
    caps.resolvedRun = anim::RUN;
    caps.resolvedSprint = anim::SPRINT;
    caps.resolvedWalkBackwards = anim::WALK_BACKWARDS;
    caps.resolvedJumpStart = anim::JUMP_START;
    caps.resolvedJump = anim::JUMP;
    caps.resolvedJumpEnd = anim::JUMP_END;
    caps.resolvedSwimIdle = anim::SWIM_IDLE;
    caps.resolvedSwim = anim::SWIM;
    caps.hasStand = true;
    caps.hasWalk = true;
    caps.hasRun = true;
    caps.hasSprint = true;
    caps.hasWalkBackwards = true;
    caps.hasJump = true;
    caps.hasSwim = true;
    return caps;
}

static LocomotionFSM::Input idle() {
    LocomotionFSM::Input in;
    in.deltaTime = 0.016f;
    return in;
}

TEST_CASE("LocomotionFSM: IDLE → WALK on move start (non-sprinting)", "[locomotion]") {
    LocomotionFSM fsm;
    auto caps = makeLocoCaps();

    auto in = idle();
    in.moving = true;
    auto out = fsm.resolve(in, caps);

    REQUIRE(out.valid);
    REQUIRE(out.animId == anim::WALK);
    REQUIRE(out.loop == true);
    CHECK(fsm.getState() == LocomotionFSM::State::WALK);
}

TEST_CASE("LocomotionFSM: IDLE → RUN on move start (sprinting)", "[locomotion]") {
    LocomotionFSM fsm;
    auto caps = makeLocoCaps();

    auto in = idle();
    in.moving = true;
    in.sprinting = true;
    auto out = fsm.resolve(in, caps);

    REQUIRE(out.valid);
    REQUIRE(out.animId == anim::RUN);
    CHECK(fsm.getState() == LocomotionFSM::State::RUN);
}

TEST_CASE("LocomotionFSM: WALK → IDLE on move stop (after grace)", "[locomotion]") {
    LocomotionFSM fsm;
    auto caps = makeLocoCaps();

    // Start walking
    auto in = idle();
    in.moving = true;
    fsm.resolve(in, caps);
    CHECK(fsm.getState() == LocomotionFSM::State::WALK);

    // Stop moving - grace timer keeps walk for a bit
    in.moving = false;
    in.deltaTime = 0.2f; // > grace period (0.12s)
    auto out = fsm.resolve(in, caps);

    CHECK(fsm.getState() == LocomotionFSM::State::IDLE);
}

TEST_CASE("LocomotionFSM: WALK → JUMP_START on jump", "[locomotion]") {
    LocomotionFSM fsm;
    auto caps = makeLocoCaps();

    // Start walking
    auto in = idle();
    in.moving = true;
    fsm.resolve(in, caps);

    // Jump
    in.jumping = true;
    in.grounded = false;
    auto out = fsm.resolve(in, caps);

    REQUIRE(out.valid);
    REQUIRE(out.animId == anim::JUMP_START);
    CHECK(fsm.getState() == LocomotionFSM::State::JUMP_START);
}

TEST_CASE("LocomotionFSM: SWIM_IDLE → SWIM on move start while swimming", "[locomotion]") {
    LocomotionFSM fsm;
    auto caps = makeLocoCaps();

    // Enter swim idle
    auto in = idle();
    in.swimming = true;
    fsm.resolve(in, caps);
    CHECK(fsm.getState() == LocomotionFSM::State::SWIM_IDLE);

    // Start swimming
    in.moving = true;
    auto out = fsm.resolve(in, caps);

    REQUIRE(out.valid);
    REQUIRE(out.animId == anim::SWIM);
    CHECK(fsm.getState() == LocomotionFSM::State::SWIM);
}

TEST_CASE("LocomotionFSM: backward walking resolves WALK_BACKWARDS", "[locomotion]") {
    LocomotionFSM fsm;
    auto caps = makeLocoCaps();

    auto in = idle();
    in.moving = true;
    in.movingBackward = true;
    auto out = fsm.resolve(in, caps);

    REQUIRE(out.valid);
    // Should use WALK_BACKWARDS when available
    CHECK(out.animId == anim::WALK_BACKWARDS);
}

TEST_CASE("LocomotionFSM: STAY when WALK_BACKWARDS missing from caps", "[locomotion]") {
    LocomotionFSM fsm;
    AnimCapabilitySet caps;
    caps.resolvedStand = anim::STAND;
    caps.resolvedWalk = anim::WALK;
    caps.hasStand = true;
    caps.hasWalk = true;
    // No WALK_BACKWARDS in caps

    auto in = idle();
    in.moving = true;
    in.movingBackward = true;
    auto out = fsm.resolve(in, caps);

    // Should still resolve something - falls back to walk
    REQUIRE(out.valid);
}

TEST_CASE("LocomotionFSM: reset restores IDLE", "[locomotion]") {
    LocomotionFSM fsm;
    auto caps = makeLocoCaps();

    auto in = idle();
    in.moving = true;
    fsm.resolve(in, caps);
    CHECK(fsm.getState() == LocomotionFSM::State::WALK);

    fsm.reset();
    CHECK(fsm.getState() == LocomotionFSM::State::IDLE);
}

TEST_CASE("Moving landings resolve directly to stride without a transient landing pose", "[locomotion]") {
    auto caps = makeLocoCaps();
    for (auto airborne : {LocomotionFSM::State::JUMP_START, LocomotionFSM::State::JUMP_MID}) {
        for (bool running : {false, true}) {
            LocomotionFSM fsm;
            fsm.setState(airborne);
            auto in = idle();
            in.moving = true;
            in.sprinting = running;
            fsm.resolve(in, caps);
            CHECK(fsm.getState() == (running ? LocomotionFSM::State::RUN : LocomotionFSM::State::WALK));
        }
        LocomotionFSM fsm;
        fsm.setState(airborne);
        fsm.resolve(idle(), caps);
        CHECK(fsm.getState() == LocomotionFSM::State::JUMP_END);
    }
}

TEST_CASE("LocomotionFSM: swim stop grace preserves direction until idle", "[locomotion][swim]") {
    auto caps = makeLocoCaps();
    caps.resolvedSwimBackwards = anim::SWIM_BACKWARDS;
    caps.resolvedSwimLeft = anim::SWIM_LEFT;
    caps.resolvedSwimRight = anim::SWIM_RIGHT;
    for (auto direction : {anim::SWIM, anim::SWIM_BACKWARDS, anim::SWIM_LEFT, anim::SWIM_RIGHT}) {
        LocomotionFSM fsm;
        auto in = idle();
        in.swimming = true;
        in.moving = true;
        in.movingBackward = direction == anim::SWIM_BACKWARDS;
        in.strafeLeft = direction == anim::SWIM_LEFT;
        in.strafeRight = direction == anim::SWIM_RIGHT;
        REQUIRE(fsm.resolve(in, caps).animId == direction);
        in.moving = in.movingBackward = in.strafeLeft = in.strafeRight = false;
        for (int frame = 0; frame < 7; ++frame) {
            CHECK(fsm.resolve(in, caps).animId == direction);
        }
        CHECK(fsm.resolve(in, caps).animId == anim::SWIM_IDLE);
        in.moving = true;
        CHECK(fsm.resolve(in, caps).animId == anim::SWIM);
    }
}

// ── Turning in place ─────────────────────────────────────────────────────────
// Build 8606 shuffles a standing character's feet while it turns, by keyboard
// or mouse-look, and keeps stepping for a cycle after the turn stops
// (_experiments/20261118T1952.turn_shuffle).

static LocomotionFSM::Input turning(int direction, uint32_t currentAnimId, float timeMs) {
    auto in = idle();
    in.turnDirection = direction;
    in.haveAnimState = true;
    in.currentAnimId = currentAnimId;
    in.currentAnimTime = timeMs;
    in.currentAnimDuration = 500.0f;
    return in;
}

TEST_CASE("LocomotionFSM: turning left in place plays ShuffleLeft", "[locomotion][turn]") {
    LocomotionFSM fsm;
    auto caps = makeLocoCaps();
    auto out = fsm.resolve(turning(1, anim::STAND, 0.0f), caps);
    REQUIRE(out.valid);
    CHECK(out.animId == anim::SHUFFLE_LEFT);
    CHECK(out.loop);
    CHECK(fsm.getState() == LocomotionFSM::State::TURN);
}

TEST_CASE("LocomotionFSM: turning right in place plays ShuffleRight", "[locomotion][turn]") {
    LocomotionFSM fsm;
    auto caps = makeLocoCaps();
    auto out = fsm.resolve(turning(-1, anim::STAND, 0.0f), caps);
    CHECK(out.animId == anim::SHUFFLE_RIGHT);
}

TEST_CASE("LocomotionFSM: shuffle steps one more cycle after the turn stops", "[locomotion][turn]") {
    LocomotionFSM fsm;
    auto caps = makeLocoCaps();
    fsm.resolve(turning(1, anim::STAND, 0.0f), caps);
    fsm.resolve(turning(1, anim::SHUFFLE_LEFT, 100.0f), caps);
    // Turn released mid-cycle: the shuffle keeps playing.
    CHECK(fsm.resolve(turning(0, anim::SHUFFLE_LEFT, 200.0f), caps).animId == anim::SHUFFLE_LEFT);
    CHECK(fsm.resolve(turning(0, anim::SHUFFLE_LEFT, 480.0f), caps).animId == anim::SHUFFLE_LEFT);
    // That cycle had a turn in it: one more.
    CHECK(fsm.resolve(turning(0, anim::SHUFFLE_LEFT, 10.0f), caps).animId == anim::SHUFFLE_LEFT);
    CHECK(fsm.resolve(turning(0, anim::SHUFFLE_LEFT, 450.0f), caps).animId == anim::SHUFFLE_LEFT);
    // A whole cycle without a turn: back to Stand.
    CHECK(fsm.resolve(turning(0, anim::SHUFFLE_LEFT, 5.0f), caps).animId == anim::STAND);
    CHECK(fsm.getState() == LocomotionFSM::State::IDLE);
}

TEST_CASE("LocomotionFSM: mouse-look turning in steps keeps one shuffle going", "[locomotion][turn]") {
    LocomotionFSM fsm;
    auto caps = makeLocoCaps();
    fsm.resolve(turning(-1, anim::STAND, 0.0f), caps);
    float t = 0.0f;
    // Facing changes on one frame in ten; frames in between have no turn.
    for (int frame = 1; frame < 200; ++frame) {
        t = std::fmod(t + 16.0f, 500.0f);
        const auto out = fsm.resolve(turning(frame % 10 == 0 ? -1 : 0, anim::SHUFFLE_RIGHT, t), caps);
        REQUIRE(out.animId == anim::SHUFFLE_RIGHT);
    }
}

TEST_CASE("LocomotionFSM: a one-frame turn still plays a whole shuffle", "[locomotion][turn]") {
    LocomotionFSM fsm;
    auto caps = makeLocoCaps();
    fsm.resolve(turning(-1, anim::STAND, 0.0f), caps);
    // The renderer has not started the shuffle yet.
    CHECK(fsm.resolve(turning(0, anim::STAND, 900.0f), caps).animId == anim::SHUFFLE_RIGHT);
    CHECK(fsm.resolve(turning(0, anim::SHUFFLE_RIGHT, 0.0f), caps).animId == anim::SHUFFLE_RIGHT);
    CHECK(fsm.resolve(turning(0, anim::SHUFFLE_RIGHT, 300.0f), caps).animId == anim::SHUFFLE_RIGHT);
    CHECK(fsm.resolve(turning(0, anim::SHUFFLE_RIGHT, 20.0f), caps).animId == anim::SHUFFLE_RIGHT);
    CHECK(fsm.resolve(turning(0, anim::SHUFFLE_RIGHT, 400.0f), caps).animId == anim::SHUFFLE_RIGHT);
    CHECK(fsm.resolve(turning(0, anim::SHUFFLE_RIGHT, 10.0f), caps).animId == anim::STAND);
}

TEST_CASE("LocomotionFSM: reversing the turn switches the shuffle", "[locomotion][turn]") {
    LocomotionFSM fsm;
    auto caps = makeLocoCaps();
    fsm.resolve(turning(1, anim::STAND, 0.0f), caps);
    fsm.resolve(turning(1, anim::SHUFFLE_LEFT, 100.0f), caps);
    CHECK(fsm.resolve(turning(-1, anim::SHUFFLE_LEFT, 200.0f), caps).animId == anim::SHUFFLE_RIGHT);
    // The left shuffle still reported by the renderer does not end the right one.
    CHECK(fsm.resolve(turning(0, anim::SHUFFLE_LEFT, 10.0f), caps).animId == anim::SHUFFLE_RIGHT);
}

TEST_CASE("LocomotionFSM: moving ends the turn at once", "[locomotion][turn]") {
    LocomotionFSM fsm;
    auto caps = makeLocoCaps();
    fsm.resolve(turning(1, anim::STAND, 0.0f), caps);
    auto in = turning(1, anim::SHUFFLE_LEFT, 100.0f);
    in.moving = true;
    in.sprinting = true;
    CHECK(fsm.resolve(in, caps).animId == anim::RUN);
}

TEST_CASE("LocomotionFSM: no shuffle while swimming or airborne", "[locomotion][turn]") {
    auto caps = makeLocoCaps();
    {
        LocomotionFSM fsm;
        auto in = turning(1, anim::STAND, 0.0f);
        in.swimming = true;
        CHECK(fsm.resolve(in, caps).animId == anim::SWIM_IDLE);
    }
    {
        LocomotionFSM fsm;
        auto in = turning(1, anim::STAND, 0.0f);
        in.grounded = false;
        CHECK(fsm.resolve(in, caps).animId == anim::JUMP);
    }
}
