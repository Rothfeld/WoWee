// LocomotionFSM unit tests
#include <catch_amalgamated.hpp>
#include <cmath>
#include "rendering/animation/locomotion_fsm.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "rendering/animation/creature_locomotion.hpp"

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

TEST_CASE("LocomotionFSM: WALK and RUN transition to IDLE immediately on move stop", "[locomotion]") {
    auto caps = makeLocoCaps();
    constexpr float dt60 = 1.0f / 60.0f;

    // WALK -> IDLE on exact release frame
    {
        LocomotionFSM fsm;
        auto in = idle();
        in.deltaTime = dt60;
        in.moving = true;
        in.movingForward = true;
        REQUIRE(fsm.resolve(in, caps).animId == anim::WALK);
        CHECK(fsm.getState() == LocomotionFSM::State::WALK);

        in.moving = false;
        in.movingForward = false;
        auto out = fsm.resolve(in, caps);
        CHECK(fsm.getState() == LocomotionFSM::State::IDLE);
        CHECK(out.animId == anim::STAND);
    }

    // RUN -> IDLE on exact release frame (W -> Stop and Q/E -> Stop)
    for (bool strafe : {false, true}) {
        LocomotionFSM fsm;
        auto in = idle();
        in.deltaTime = dt60;
        in.moving = true;
        in.sprinting = true;
        in.movingForward = !strafe;
        in.strafeLeft = strafe;
        REQUIRE(fsm.resolve(in, caps).animId == anim::RUN);
        CHECK(fsm.getState() == LocomotionFSM::State::RUN);

        in.moving = false;
        in.sprinting = false;
        in.movingForward = false;
        in.strafeLeft = false;
        auto out = fsm.resolve(in, caps);
        CHECK(fsm.getState() == LocomotionFSM::State::IDLE);
        CHECK(out.animId == anim::STAND);
    }

    // WALK_BACKWARDS -> IDLE on exact release frame (S -> Stop)
    {
        LocomotionFSM fsm;
        auto in = idle();
        in.deltaTime = dt60;
        in.moving = true;
        in.movingBackward = true;
        REQUIRE(fsm.resolve(in, caps).animId == anim::WALK_BACKWARDS);

        in.moving = false;
        in.movingBackward = false;
        auto out = fsm.resolve(in, caps);
        CHECK(fsm.getState() == LocomotionFSM::State::IDLE);
        CHECK(out.animId == anim::STAND);
    }
}

TEST_CASE("LocomotionFSM: direction reversals transition directly without IDLE flash", "[locomotion]") {
    LocomotionFSM fsm;
    auto caps = makeLocoCaps();
    constexpr float dt60 = 1.0f / 60.0f;

    // W (RUN) -> S (WALK_BACKWARDS) -> W (RUN)
    auto in = idle();
    in.deltaTime = dt60;
    in.moving = true;
    in.sprinting = true;
    in.movingForward = true;
    REQUIRE(fsm.resolve(in, caps).animId == anim::RUN);
    CHECK(fsm.getState() == LocomotionFSM::State::RUN);

    in.movingForward = false;
    in.movingBackward = true;
    in.sprinting = false;
    auto rev = fsm.resolve(in, caps);
    CHECK(fsm.getState() == LocomotionFSM::State::WALK);
    CHECK(rev.animId == anim::WALK_BACKWARDS);

    in.movingBackward = false;
    in.movingForward = true;
    in.sprinting = true;
    auto fwd = fsm.resolve(in, caps);
    CHECK(fsm.getState() == LocomotionFSM::State::RUN);
    CHECK(fwd.animId == anim::RUN);

    // Q -> E strafe reversal stays RUN
    in.movingForward = false;
    in.strafeLeft = true;
    CHECK(fsm.resolve(in, caps).animId == anim::RUN);
    in.strafeLeft = false;
    in.strafeRight = true;
    CHECK(fsm.resolve(in, caps).animId == anim::RUN);
    CHECK(fsm.getState() == LocomotionFSM::State::RUN);
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

TEST_CASE("LocomotionFSM: running jump with mid-air stop lands in JUMP_END", "[locomotion]") {
    LocomotionFSM fsm;
    auto caps = makeLocoCaps();

    // Run, jump, release W right before landing: must land in JUMP_END, not RUN.
    auto in = idle();
    in.moving = true;
    in.sprinting = true;
    in.movingForward = true;
    REQUIRE(fsm.resolve(in, caps).animId == anim::RUN);

    in.grounded = false;
    in.jumping = true;
    REQUIRE(fsm.resolve(in, caps).animId == anim::JUMP_START);

    in.moving = false;
    in.sprinting = false;
    in.movingForward = false;
    in.jumping = false;
    in.grounded = true;
    auto land = fsm.resolve(in, caps);
    CHECK(fsm.getState() == LocomotionFSM::State::JUMP_END);
    CHECK(land.animId == anim::JUMP_END);
}

TEST_CASE("LocomotionFSM: swim stop transitions immediately to SWIM_IDLE without forward flash", "[locomotion][swim]") {
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
        CHECK(fsm.resolve(in, caps).animId == anim::SWIM_IDLE);
        CHECK(fsm.getState() == LocomotionFSM::State::SWIM_IDLE);
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

TEST_CASE("LocomotionFSM: speed-driven ground gait selection across 0.5x..10x and Walk/Run modes", "[locomotion][speed]") {
    auto caps = makeLocoCaps();
    constexpr float kWalkStride = 2.5f;
    constexpr float kRunStride = 250.0f / 36.0f;

    auto evalGround = [&](bool runMode, float speed, bool sprintAura = false, bool backward = false) {
        LocomotionFSM fsm;
        auto in = idle();
        in.moving = true;
        in.movingForward = !backward;
        in.movingBackward = backward;
        in.sprinting = runMode && !backward;
        in.sprintAura = sprintAura;
        in.locomotionSpeed = speed;
        in.walkStride = kWalkStride;
        in.runStride = kRunStride;
        return fsm.resolve(in, caps).animId;
    };

    // Run mode across 0.5x..10.0x (base run = 7.0 yd/s)
    CHECK(evalGround(true, 3.50f)  == anim::WALK);   // 0.50x slowed (< 5.00 yd/s)
    CHECK(evalGround(true, 4.90f)  == anim::WALK);   // 0.70x slowed (< 5.00 yd/s)
    CHECK(evalGround(true, 5.04f)  == anim::RUN);    // 0.72x (>= 5.00 yd/s)
    CHECK(evalGround(true, 7.00f)  == anim::RUN);    // 1.00x normal run
    CHECK(evalGround(true, 9.80f)  == anim::RUN);    // 1.40x Aspect/Travel speed
    CHECK(evalGround(true, 10.85f) == anim::RUN);    // 1.55x below Sprint threshold (~10.97 yd/s)
    CHECK(evalGround(true, 11.20f) == anim::SPRINT); // 1.60x above Sprint threshold
    CHECK(evalGround(true, 11.90f) == anim::SPRINT); // 1.70x Rogue Sprint / .modify speed 1.7
    CHECK(evalGround(true, 10.50f, true) == anim::SPRINT); // 1.50x Sprint Rank 1 with sprintAura
    CHECK(evalGround(true, 14.00f) == anim::SPRINT); // 2.00x
    CHECK(evalGround(true, 21.00f) == anim::SPRINT); // 3.00x
    CHECK(evalGround(true, 35.00f) == anim::SPRINT); // 5.00x (.modify speed 5)
    CHECK(evalGround(true, 70.00f) == anim::SPRINT); // 10.0x (.modify speed 10)

    // Walk mode (KP_Divide toggle, base walk = 2.5 yd/s): stays WALK until Sprint threshold
    CHECK(evalGround(false, 1.25f)  == anim::WALK);   // 0.5x walk
    CHECK(evalGround(false, 2.50f)  == anim::WALK);   // 1.0x walk
    CHECK(evalGround(false, 5.00f)  == anim::WALK);   // 2.0x walk
    CHECK(evalGround(false, 7.50f)  == anim::WALK);   // 3.0x walk
    CHECK(evalGround(false, 10.50f) == anim::WALK);   // 4.2x walk (< 10.97 yd/s)
    CHECK(evalGround(false, 11.00f) == anim::SPRINT); // 4.4x walk promotes to Sprint (Native 2.4.3)
    CHECK(evalGround(false, 12.50f) == anim::SPRINT); // 5.0x walk promotes to Sprint

    // Backpedal never promotes to SPRINT even at 5x speed
    CHECK(evalGround(false, 22.50f, false, true) == anim::WALK_BACKWARDS);
}

TEST_CASE("LocomotionFSM: unmounted flight resolves FLY_IDLE and FLY_FORWARD and lands cleanly", "[locomotion][flight]") {
    LocomotionFSM fsm;
    auto caps = makeLocoCaps();

    auto in = idle();
    in.flying = true;
    in.grounded = true; // CameraController suppresses fall damage with grounded=true while flying
    auto hoverOut = fsm.resolve(in, caps);
    REQUIRE(hoverOut.valid);
    CHECK(fsm.getState() == LocomotionFSM::State::FLY_IDLE);
    CHECK(hoverOut.animId == anim::FLY_IDLE);
    CHECK(hoverOut.loop);

    in.moving = true;
    in.movingForward = true;
    in.sprinting = true;
    auto flyOut = fsm.resolve(in, caps);
    REQUIRE(flyOut.valid);
    CHECK(fsm.getState() == LocomotionFSM::State::FLY);
    CHECK(flyOut.animId == anim::FLY_FORWARD);
    CHECK(flyOut.loop);

    // Touching down while moving resumes ground stride directly
    in.flying = false;
    auto landOut = fsm.resolve(in, caps);
    REQUIRE(landOut.valid);
    CHECK(fsm.getState() == LocomotionFSM::State::RUN);
    CHECK(landOut.animId == anim::RUN);
}

TEST_CASE("LocomotionFSM: Wisp-like model lacking jump clips skips 1-frame JUMP_START and JUMP_END flashes", "[locomotion][wisp]") {
    LocomotionFSM fsm;
    AnimCapabilitySet wispCaps;
    wispCaps.resolvedWalk = anim::WALK;
    wispCaps.resolvedRun = anim::RUN;
    wispCaps.resolvedWalkBackwards = anim::WALK_BACKWARDS;
    wispCaps.resolvedSwimIdle = anim::SWIM_IDLE;
    wispCaps.resolvedSwim = anim::SWIM;
    wispCaps.resolvedSwimBackwards = anim::SWIM_BACKWARDS;
    // Wisp.m2 lacks JUMP_START (37), JUMP (38), JUMP_END (39), and SPRINT (143).
    wispCaps.resolvedJumpStart = 0;
    wispCaps.resolvedJump = 0;
    wispCaps.resolvedJumpEnd = 0;
    wispCaps.hasSprint = false;

    auto in = idle();
    in.moving = true;
    in.movingForward = true;
    in.sprinting = true;
    in.locomotionSpeed = 10.5f; // Night Elf Wisp Spirit (+50% run speed)
    in.walkStride = 2.5f;
    in.runStride = 6.9444f;
    auto runOut = fsm.resolve(in, wispCaps);
    REQUIRE(runOut.valid);
    CHECK(runOut.animId == anim::RUN);

    // Jumping on a model without JUMP_START goes straight to JUMP_MID on frame 0
    in.grounded = false;
    in.jumping = true;
    auto jumpOut = fsm.resolve(in, wispCaps);
    REQUIRE(jumpOut.valid);
    CHECK(fsm.getState() == LocomotionFSM::State::JUMP_MID);
    CHECK(jumpOut.animId == anim::JUMP);
    CHECK(jumpOut.loop);

    // Stationary landing on a model without JUMP_END goes straight to IDLE on frame 0
    in.grounded = true;
    in.jumping = false;
    in.moving = false;
    in.movingForward = false;
    in.sprinting = false;
    auto landOut = fsm.resolve(in, wispCaps);
    REQUIRE(landOut.valid);
    CHECK(fsm.getState() == LocomotionFSM::State::IDLE);
    CHECK(landOut.animId == anim::STAND);
    CHECK(landOut.loop);
}

TEST_CASE("Wisp stand-in continuity and slope-drop airborne grace prevent animation resets", "[locomotion][wisp]") {
    // Switching between IDs that resolve to the same looping stand-in sequence
    // (e.g. Wisp.m2 Stand 0 <-> ShuffleLeft 11 <-> ShuffleRight 12 <-> Fall 38)
    // must preserve the running sequence clock instead of snapping back to t=0.
    CHECK(preservesPlayingSequenceOnStandIn(
        anim::STAND, anim::SHUFFLE_LEFT, 0, 0, false, false, true, false));
    CHECK(preservesPlayingSequenceOnStandIn(
        anim::SHUFFLE_LEFT, anim::SHUFFLE_RIGHT, 0, 0, false, false, true, false));
    CHECK(preservesPlayingSequenceOnStandIn(
        anim::JUMP, anim::STAND, 0, 0, false, false, true, false));
    CHECK(preservesPlayingSequenceOnStandIn(
        anim::RUN, anim::SPRINT, 4, 4, false, false, true, false));

    // Real sequence changes, reversed playback changes, one-shots, and frozen poses must not preserve.
    CHECK_FALSE(preservesPlayingSequenceOnStandIn(
        anim::STAND, anim::RUN, 0, 4, false, false, true, false));
    CHECK_FALSE(preservesPlayingSequenceOnStandIn(
        anim::WALK, anim::WALK_BACKWARDS, 2, 2, false, true, true, false));
    CHECK_FALSE(preservesPlayingSequenceOnStandIn(
        anim::STAND, anim::JUMP_START, 0, 0, false, false, false, false));
    CHECK_FALSE(preservesPlayingSequenceOnStandIn(
        anim::DEATH, anim::DEAD, 1, 1, false, false, true, true));

    // Brief non-jump slope drops (< 150 ms) stay grounded; explicit jumps and real falls do not.
    CHECK(effectiveAnimationGrounded(true, false, 0.0f));
    CHECK(effectiveAnimationGrounded(false, false, 0.05f));
    CHECK(effectiveAnimationGrounded(false, false, 0.14f));
    CHECK_FALSE(effectiveAnimationGrounded(false, false, 0.15f));
    CHECK_FALSE(effectiveAnimationGrounded(false, true, 0.0f));
    CHECK(effectiveAnimationGrounded(false, true, 0.50f, true));
}
