#include <catch_amalgamated.hpp>
#include "game/auto_repeat_stun_policy.hpp"

using wowee::game::AutoRepeatStunTracker;

TEST_CASE("AutoRepeatStunTracker: cancel before stun in same tick resumes on stun exit", "[game][combat][stun]") {
    AutoRepeatStunTracker tracker;
    constexpr uint64_t kTarget = 0xF1300047A754FE58ULL;
    constexpr uint32_t kAutoShot = 75u;

    // CMaNGOS HandleAuraModStun sends SMSG_CANCEL_AUTO_REPEAT right before
    // SMSG_UPDATE_OBJECT sets UNIT_FLAG_STUNNED in the same server tick.
    tracker.onServerCancel(kAutoShot, kTarget);
    REQUIRE(tracker.activeSuspendedSpellId(kTarget) == 0u);

    uint64_t resumeTarget = 0;
    REQUIRE(tracker.onStunChanged(true, kTarget, resumeTarget) == 0u);
    REQUIRE(resumeTarget == 0u);
    REQUIRE(tracker.activeSuspendedSpellId(kTarget) == kAutoShot);

    // Stun expires while still targeting the same unit: resume Auto Shot.
    REQUIRE(tracker.onStunChanged(false, kTarget, resumeTarget) == kAutoShot);
    REQUIRE(resumeTarget == kTarget);
    REQUIRE(tracker.activeSuspendedSpellId(kTarget) == 0u);
}

TEST_CASE("AutoRepeatStunTracker: cancel while already stunned resumes on stun exit", "[game][combat][stun]") {
    AutoRepeatStunTracker tracker;
    constexpr uint64_t kTarget = 0xF13000401C000001ULL;
    constexpr uint32_t kAutoShot = 75u;

    uint64_t resumeTarget = 0;
    REQUIRE(tracker.onStunChanged(true, kTarget, resumeTarget) == 0u);

    // Unit::UpdateAutoRepeat fails with SPELL_FAILED_STUNNED mid-stun.
    tracker.onServerCancel(kAutoShot, kTarget);
    REQUIRE(tracker.activeSuspendedSpellId(kTarget) == kAutoShot);

    REQUIRE(tracker.onStunChanged(false, kTarget, resumeTarget) == kAutoShot);
    REQUIRE(resumeTarget == kTarget);
}

TEST_CASE("AutoRepeatStunTracker: non-stun server cancel expires and does not resume", "[game][combat][stun]") {
    AutoRepeatStunTracker tracker;
    constexpr uint64_t kTarget = 0xF13000401C000002ULL;
    constexpr uint32_t kWandShoot = 5019u;

    tracker.onServerCancel(kWandShoot, kTarget);
    tracker.tick(AutoRepeatStunTracker::kStunCorrelationWindowSec + 0.05f);

    uint64_t resumeTarget = 0;
    REQUIRE(tracker.onStunChanged(true, kTarget, resumeTarget) == 0u);
    REQUIRE(tracker.activeSuspendedSpellId(kTarget) == 0u);
    REQUIRE(tracker.onStunChanged(false, kTarget, resumeTarget) == 0u);
    REQUIRE(resumeTarget == 0u);
}

TEST_CASE("AutoRepeatStunTracker: explicit cancel or target change prevents resume", "[game][combat][stun]") {
    constexpr uint64_t kTargetA = 0xF13000401C000003ULL;
    constexpr uint64_t kTargetB = 0xF13000401C000004ULL;
    constexpr uint32_t kAutoShot = 75u;

    SECTION("explicit clear (/stopattack) while stunned") {
        AutoRepeatStunTracker tracker;
        uint64_t resumeTarget = 0;
        tracker.onServerCancel(kAutoShot, kTargetA);
        tracker.onStunChanged(true, kTargetA, resumeTarget);
        REQUIRE(tracker.activeSuspendedSpellId(kTargetA) == kAutoShot);

        tracker.clear();
        REQUIRE(tracker.activeSuspendedSpellId(kTargetA) == 0u);
        REQUIRE(tracker.onStunChanged(false, kTargetA, resumeTarget) == 0u);
        REQUIRE(resumeTarget == 0u);
    }

    SECTION("target switched while stunned") {
        AutoRepeatStunTracker tracker;
        uint64_t resumeTarget = 0;
        tracker.onServerCancel(kAutoShot, kTargetA);
        tracker.onStunChanged(true, kTargetA, resumeTarget);
        REQUIRE(tracker.activeSuspendedSpellId(kTargetA) == kAutoShot);

        tracker.onTargetChanged(kTargetB);
        REQUIRE(tracker.activeSuspendedSpellId(kTargetB) == 0u);
        REQUIRE(tracker.onStunChanged(false, kTargetB, resumeTarget) == 0u);
        REQUIRE(resumeTarget == 0u);
    }

    SECTION("target cleared while stunned") {
        AutoRepeatStunTracker tracker;
        uint64_t resumeTarget = 0;
        tracker.onServerCancel(kAutoShot, kTargetA);
        tracker.onStunChanged(true, kTargetA, resumeTarget);

        tracker.onTargetChanged(0);
        REQUIRE(tracker.onStunChanged(false, 0, resumeTarget) == 0u);
        REQUIRE(resumeTarget == 0u);
    }

    SECTION("resetAll clears stun and suspended state") {
        AutoRepeatStunTracker tracker;
        uint64_t resumeTarget = 0;
        tracker.onServerCancel(kAutoShot, kTargetA);
        tracker.onStunChanged(true, kTargetA, resumeTarget);
        tracker.resetAll();
        REQUIRE(!tracker.playerStunned);
        REQUIRE(tracker.activeSuspendedSpellId(kTargetA) == 0u);
        REQUIRE(tracker.onStunChanged(false, kTargetA, resumeTarget) == 0u);
    }
}
