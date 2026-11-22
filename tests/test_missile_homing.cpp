#include <catch_amalgamated.hpp>

#include "rendering/missile_homing.hpp"

#include <cmath>
#include <limits>
#include <optional>
#include <vector>

using wowee::rendering::HomingStepResult;
using wowee::rendering::MissileTargetSample;
using wowee::rendering::missileDirectionToEuler;
using wowee::rendering::missileFlightSpeed;
using wowee::rendering::missileMaxFlightDuration;
using wowee::rendering::stepHomingMissile;

namespace {
bool isFiniteVec(const glm::vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
}  // namespace

TEST_CASE("Lateral moving target: homing curves smoothly and updates yaw/pitch tangent", "[missile][homing]") {
    const glm::vec3 start(0.0f, 0.0f, 1.5f);
    const glm::vec3 targetStart(25.0f, -10.0f, 1.5f);
    const glm::vec3 targetVel(0.0f, 10.5f, 1.0f);  // moving laterally +Y and slightly uphill +Z
    const float speed = 20.0f;
    const float dt = 1.0f / 60.0f;

    glm::vec3 pos = start;
    glm::vec3 dir = glm::normalize(targetStart - start);
    float traveled = 0.0f;
    float elapsed = 0.0f;
    bool arrived = false;
    float initialYaw = missileDirectionToEuler(dir).z;
    float maxYawDelta = 0.0f;

    for (int i = 0; i < 300; ++i) {
        elapsed += dt;
        const glm::vec3 liveTarget = targetStart + targetVel * elapsed;
        const glm::vec3 prevPos = pos;
        const HomingStepResult step = stepHomingMissile(pos, liveTarget, dir, speed, dt, traveled);
        REQUIRE(isFiniteVec(step.position));
        REQUIRE(isFiniteVec(step.direction));
        REQUIRE(isFiniteVec(step.rotation));

        // Instantaneous direction must match actual displacement tangent.
        const glm::vec3 disp = step.position - prevPos;
        if (glm::length(disp) > 1e-4f) {
            const glm::vec3 tangent = glm::normalize(disp);
            CHECK(glm::dot(tangent, step.direction) == Catch::Approx(1.0f).margin(1e-4f));
            const glm::vec3 expectedRot = missileDirectionToEuler(tangent);
            CHECK(step.rotation.z == Catch::Approx(expectedRot.z).margin(1e-4f));
            CHECK(step.rotation.y == Catch::Approx(expectedRot.y).margin(1e-4f));
        }

        maxYawDelta = std::max(maxYawDelta, std::abs(step.rotation.z - initialYaw));
        pos = step.position;
        dir = step.direction;
        if (step.arrived) {
            arrived = true;
            CHECK(step.animProgress == Catch::Approx(1.0f));
            CHECK(glm::length(pos - liveTarget) == Catch::Approx(0.0f).margin(1e-4f));
            // Must NOT have landed at the stale launch-time target position!
            CHECK(glm::length(pos - targetStart) > 10.0f);
            break;
        }
    }

    REQUIRE(arrived);
    // Lateral pursuit must visibly curve (significant yaw change from launch heading).
    CHECK(maxYawDelta > 0.35f);
}

TEST_CASE("Target moving toward caster arrives earlier than stationary duration", "[missile][homing]") {
    const glm::vec3 start(0.0f, 0.0f, 1.0f);
    const glm::vec3 targetStart(30.0f, 0.0f, 1.0f);
    const glm::vec3 targetVel(-10.0f, 0.0f, 0.0f);  // closing at 10 yd/s
    const float speed = 20.0f;                      // stationary arrival = 1.50 s, closing = 30 / 30 = 1.00 s
    const float dt = 1.0f / 60.0f;

    glm::vec3 pos = start;
    glm::vec3 dir(1.0f, 0.0f, 0.0f);
    float traveled = 0.0f;
    float elapsed = 0.0f;
    bool arrived = false;

    for (int i = 0; i < 200; ++i) {
        elapsed += dt;
        const glm::vec3 liveTarget = targetStart + targetVel * elapsed;
        const HomingStepResult step = stepHomingMissile(pos, liveTarget, dir, speed, dt, traveled);
        pos = step.position;
        dir = step.direction;
        if (step.arrived) {
            arrived = true;
            CHECK(glm::length(pos - liveTarget) == Catch::Approx(0.0f).margin(1e-4f));
            break;
        }
    }

    REQUIRE(arrived);
    CHECK(elapsed == Catch::Approx(1.0f).margin(0.03f));
}

TEST_CASE("Target moving away from caster extends pursuit without premature detonation", "[missile][homing]") {
    const glm::vec3 start(0.0f, 0.0f, 1.0f);
    const glm::vec3 targetStart(20.0f, 0.0f, 1.0f);
    const glm::vec3 targetVel(10.0f, 0.0f, 0.0f);  // fleeing at 10 yd/s
    const float speed = 20.0f;                     // stationary = 1.00 s, fleeing = 20 / (20 - 10) = 2.00 s
    const float dt = 1.0f / 60.0f;

    glm::vec3 pos = start;
    glm::vec3 dir(1.0f, 0.0f, 0.0f);
    float traveled = 0.0f;
    float elapsed = 0.0f;
    bool arrived = false;

    for (int i = 0; i < 300; ++i) {
        elapsed += dt;
        const glm::vec3 liveTarget = targetStart + targetVel * elapsed;
        const HomingStepResult step = stepHomingMissile(pos, liveTarget, dir, speed, dt, traveled);
        pos = step.position;
        dir = step.direction;
        if (step.arrived) {
            arrived = true;
            CHECK(glm::length(pos - liveTarget) == Catch::Approx(0.0f).margin(1e-4f));
            break;
        }
    }

    REQUIRE(arrived);
    CHECK(elapsed == Catch::Approx(2.0f).margin(0.03f));
    CHECK(pos.x == Catch::Approx(40.0f).margin(0.4f));
}

TEST_CASE("Target despawning mid-flight finishes cleanly to last known position", "[missile][homing]") {
    const glm::vec3 start(10.0f, 20.0f, 5.0f);
    glm::vec3 lastTarget(35.0f, 20.0f, 5.0f);
    const float speed = 20.0f;
    const float dt = 1.0f / 60.0f;

    glm::vec3 pos = start;
    glm::vec3 dir(1.0f, 0.0f, 0.0f);
    float traveled = 0.0f;
    bool arrived = false;

    for (int i = 0; i < 200; ++i) {
        // Target moves for first 0.5 s, then despawns (resolver returns nullopt).
        std::optional<MissileTargetSample> sample;
        if (i < 30) {
            sample = MissileTargetSample{glm::vec3(35.0f, 20.0f + i * 0.2f, 5.0f), 42};
        }
        if (sample) lastTarget = sample->position;

        const HomingStepResult step = stepHomingMissile(pos, lastTarget, dir, speed, dt, traveled);
        REQUIRE(isFiniteVec(step.position));
        REQUIRE(isFiniteVec(step.rotation));
        pos = step.position;
        dir = step.direction;
        if (step.arrived) {
            arrived = true;
            CHECK(glm::length(pos - lastTarget) == Catch::Approx(0.0f).margin(1e-4f));
            // Must never snap to origin (0,0,0).
            CHECK(glm::length(pos) > 20.0f);
            break;
        }
    }

    REQUIRE(arrived);
}

TEST_CASE("High dt, zero-distance, and close-range launchDelay edge cases never overshoot or NaN", "[missile][homing]") {
    // High dt: single huge step lands exactly on target without overshoot.
    {
        float traveled = 0.0f;
        const HomingStepResult step = stepHomingMissile(
            glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(5.0f, 0.0f, 0.0f),
            glm::vec3(1.0f, 0.0f, 0.0f), 20.0f, /*dt=*/10.0f, traveled);
        CHECK(step.arrived);
        CHECK(step.animProgress == Catch::Approx(1.0f));
        CHECK(step.position == glm::vec3(5.0f, 0.0f, 0.0f));
        CHECK(traveled == Catch::Approx(5.0f));
    }

    // Zero distance: arrives cleanly without NaN and preserves previous direction.
    {
        float traveled = 0.0f;
        const glm::vec3 prevDir = glm::normalize(glm::vec3(0.0f, 1.0f, 0.0f));
        const HomingStepResult step = stepHomingMissile(
            glm::vec3(3.0f, 4.0f, 5.0f), glm::vec3(3.0f, 4.0f, 5.0f),
            prevDir, 20.0f, 0.016f, traveled);
        CHECK(step.arrived);
        CHECK(isFiniteVec(step.position));
        CHECK(isFiniteVec(step.direction));
        CHECK(isFiniteVec(step.rotation));
        CHECK(glm::dot(step.direction, prevDir) == Catch::Approx(1.0f));
    }

    // Close-range launchDelay >= nominalDuration: missileFlightSpeed caps at 2x nominalSpeed
    // instead of returning 0 (which caused 1-frame teleportation into an instantaneous beam).
    {
        const float fs = missileFlightSpeed(/*initialDistance=*/3.0f, /*nominalSpeed=*/28.0f, /*launchDelay=*/0.2f);
        CHECK(fs == Catch::Approx(56.0f));
        float traveled = 0.0f;
        const HomingStepResult step = stepHomingMissile(
            glm::vec3(0.0f), glm::vec3(3.0f, 0.0f, 0.0f),
            glm::vec3(1.0f, 0.0f, 0.0f), fs, 1.0f / 60.0f, traveled);
        CHECK_FALSE(step.arrived);
        CHECK(step.position.x == Catch::Approx(56.0f / 60.0f).margin(1e-4f));
    }

    // Zero initialDistance still returns 0 and arrives cleanly.
    CHECK(missileFlightSpeed(/*initialDistance=*/0.0f, /*nominalSpeed=*/24.0f, /*launchDelay=*/0.2f) == 0.0f);

    // Imp Firebolt (speed 16, $CST 0.766s): at 10 yd (nominal 0.625s < 0.766s) and 15 yd
    // (window 0.1715s -> uncapped 87.5 yd/s), catch-up caps at 2x (32 yd/s) so the fireball
    // visibly flies (~0.31s at 10 yd, ~0.47s at 15 yd) matching Native TBC 2.4.3.
    CHECK(missileFlightSpeed(10.0f, 16.0f, 0.766f) == Catch::Approx(32.0f));
    CHECK(missileFlightSpeed(15.0f, 16.0f, 0.766f) == Catch::Approx(32.0f));
    // At 25 yd (window 0.7965s -> 31.39 yd/s < 32 yd/s), uncapped landing timing is preserved.
    CHECK(missileFlightSpeed(25.0f, 16.0f, 0.766f) == Catch::Approx(25.0f / (25.0f / 16.0f - 0.766f)));

    // Mage Fireball (speed 24, Human $CSL 0.200s / Troll $CSL 0.266s):
    // Close range (6 yd) caps at 2x (48 yd/s); mid/long range (15 yd, 30 yd) preserves landing time.
    CHECK(missileFlightSpeed(6.0f, 24.0f, 0.200f) == Catch::Approx(48.0f));
    CHECK(missileFlightSpeed(6.0f, 24.0f, 0.266f) == Catch::Approx(48.0f));
    CHECK(missileFlightSpeed(15.0f, 24.0f, 0.200f) == Catch::Approx(15.0f / (15.0f / 24.0f - 0.200f)));
    CHECK(missileFlightSpeed(30.0f, 24.0f, 0.266f) == Catch::Approx(30.0f / (30.0f / 24.0f - 0.266f)));

    CHECK(missileMaxFlightDuration(1.0f) >= 15.0f);
    CHECK(missileMaxFlightDuration(6.0f) == Catch::Approx(30.0f));
}

TEST_CASE("Stationary target preserves straight-line path and launchDelay-compensated landing time", "[missile][homing]") {
    const glm::vec3 start(0.0f, 0.0f, 1.5f);
    const glm::vec3 end(24.0f, 0.0f, 1.5f);
    const float nominalSpeed = 20.0f;   // lands at 1.20 s
    const float launchDelay = 0.20f;    // leaves hand at 0.20 s -> flight window 1.00 s -> speed 24 yd/s
    const float flightSpeed = missileFlightSpeed(glm::length(end - start), nominalSpeed, launchDelay);
    CHECK(flightSpeed == Catch::Approx(24.0f));

    glm::vec3 pos = start;
    glm::vec3 dir(1.0f, 0.0f, 0.0f);
    float traveled = 0.0f;
    float flightElapsed = 0.0f;
    const float dt = 1.0f / 100.0f;
    bool arrived = false;

    for (int i = 0; i < 200; ++i) {
        flightElapsed += dt;
        const HomingStepResult step = stepHomingMissile(pos, end, dir, flightSpeed, dt, traveled);
        // Straight-line preservation: zero lateral/vertical drift.
        CHECK(step.position.y == Catch::Approx(0.0f).margin(1e-5f));
        CHECK(step.position.z == Catch::Approx(1.5f).margin(1e-5f));
        pos = step.position;
        dir = step.direction;
        if (step.arrived) {
            arrived = true;
            break;
        }
    }

    REQUIRE(arrived);
    CHECK(launchDelay + flightElapsed == Catch::Approx(1.20f).margin(0.015f));
}

TEST_CASE("isValidSpellMissileSpeed and isStationaryFlashMissile handle Kael Pure Nether Beam (speed 0.01)",
          "[missile][homing]") {
    using wowee::rendering::isValidSpellMissileSpeed;
    using wowee::rendering::isStationaryFlashMissile;

    CHECK(isValidSpellMissileSpeed(0.01f));
    CHECK(isValidSpellMissileSpeed(0.5f));
    CHECK(isValidSpellMissileSpeed(24.0f));
    CHECK(isValidSpellMissileSpeed(200.0f));

    CHECK_FALSE(isValidSpellMissileSpeed(0.0f));
    CHECK_FALSE(isValidSpellMissileSpeed(-1.0f));
    CHECK_FALSE(isValidSpellMissileSpeed(201.0f));
    CHECK_FALSE(isValidSpellMissileSpeed(std::numeric_limits<float>::quiet_NaN()));
    CHECK_FALSE(isValidSpellMissileSpeed(std::numeric_limits<float>::infinity()));

    CHECK(isStationaryFlashMissile(/*nominalSpeed=*/0.01f, /*flightDuration=*/3500.0f, /*animDurationSec=*/1.0f));
    CHECK_FALSE(isStationaryFlashMissile(/*nominalSpeed=*/0.01f, /*flightDuration=*/0.5f, /*animDurationSec=*/1.0f));
    CHECK_FALSE(isStationaryFlashMissile(/*nominalSpeed=*/0.01f, /*flightDuration=*/3500.0f, /*animDurationSec=*/0.0f));

    CHECK_FALSE(isStationaryFlashMissile(/*nominalSpeed=*/0.5f, /*flightDuration=*/10.0f, /*animDurationSec=*/1.0f));
    CHECK_FALSE(isStationaryFlashMissile(/*nominalSpeed=*/20.0f, /*flightDuration=*/1.5f, /*animDurationSec=*/1.0f));
    CHECK_FALSE(isStationaryFlashMissile(/*nominalSpeed=*/24.0f, /*flightDuration=*/2.0f, /*animDurationSec=*/0.667f));
}

TEST_CASE("Stuck arrow policy, fade, and chest-bone local tracking", "[missile][stuck-arrows]") {
    using wowee::rendering::shouldStickPhysicalProjectile;
    using wowee::rendering::stuckArrowOpacity;
    using wowee::rendering::computeStuckArrowLocalPose;
    using wowee::rendering::evaluateStuckArrowWorldPose;

    // Arrows that hit stick; bullets, thrown weapons, and misses do not.
    CHECK(shouldStickPhysicalProjectile("Item\\ObjectComponents\\Ammo\\ArrowFlight_01.m2", false, true));
    CHECK(shouldStickPhysicalProjectile("Item\\ObjectComponents\\Ammo\\ArrowFireFlight_01.m2", false, true));
    CHECK_FALSE(shouldStickPhysicalProjectile("Item\\ObjectComponents\\Ammo\\ArrowFlight_01.m2", false, false));
    CHECK_FALSE(shouldStickPhysicalProjectile("Item\\ObjectComponents\\Ammo\\BulletFlight_01.m2", false, true));
    CHECK_FALSE(shouldStickPhysicalProjectile("Item\\ObjectComponents\\Weapon\\Thrown_1H_Dagger_A_01.m2", true, true));

    // Opacity stays 1.0 until the final 0.5 s fade window, then ramps linearly to 0.0 at 3.0 s.
    CHECK(stuckArrowOpacity(0.0f) == Catch::Approx(1.0f));
    CHECK(stuckArrowOpacity(2.5f) == Catch::Approx(1.0f));
    CHECK(stuckArrowOpacity(2.75f) == Catch::Approx(0.5f));
    CHECK(stuckArrowOpacity(3.0f) == Catch::Approx(0.0f));

    // Rigidly tracks chest attachment translation and 90-degree yaw rotation.
    glm::mat4 chestAtHit(1.0f);
    chestAtHit[3] = glm::vec4(10.0f, 20.0f, 1.2f, 1.0f);
    const auto local = computeStuckArrowLocalPose(chestAtHit, glm::vec3(1.0f, 0.0f, 0.0f), 0);

    glm::vec3 p0(0.0f), r0(0.0f);
    evaluateStuckArrowWorldPose(chestAtHit, local, p0, r0);
    CHECK(p0.x == Catch::Approx(10.0f));
    CHECK(p0.z == Catch::Approx(1.24f));
    CHECK(r0.z == Catch::Approx(0.0f).margin(1e-4f));

    // Rotate chest +90 deg around Z and move to (15, 25, 1.2): arrow rotates +90 deg with body.
    glm::mat4 chestTurned(1.0f);
    chestTurned[0] = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);
    chestTurned[1] = glm::vec4(-1.0f, 0.0f, 0.0f, 0.0f);
    chestTurned[2] = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f);
    chestTurned[3] = glm::vec4(15.0f, 25.0f, 1.2f, 1.0f);
    glm::vec3 p1(0.0f), r1(0.0f);
    evaluateStuckArrowWorldPose(chestTurned, local, p1, r1);
    CHECK(p1.y == Catch::Approx(25.0f));
    CHECK(p1.z == Catch::Approx(1.24f));
    CHECK(r1.z == Catch::Approx(1.5707963f).margin(1e-4f));
}

TEST_CASE("Physical projectile replaceable texture slots preserve hardcoded ribbon textures", "[missile][stuck-arrows]") {
    using wowee::rendering::isReplaceableProjectileTextureType;
    // Hardcoded filename slots (type 0, e.g. BulletFlight_01.m2 slot 0 SPELLS\GRADIENT128.BLP
    // and ArrowFlight_01.m2 slot 1 SPELLS\GRADIENT128.BLP) must never be overwritten.
    CHECK_FALSE(isReplaceableProjectileTextureType(0u));
    CHECK_FALSE(isReplaceableProjectileTextureType(1u));
    // Object skin (2, ArrowFlight_01.m2 slot 0), weapon blade (3), and weapon handle (4)
    // accept ItemDisplayInfo.dbc skin textures.
    CHECK(isReplaceableProjectileTextureType(2u));
    CHECK(isReplaceableProjectileTextureType(3u));
    CHECK(isReplaceableProjectileTextureType(4u));
}
