#pragma once
#include <algorithm>
#include <functional>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include "pipeline/m2_loader.hpp"
#include "rendering/m2_billboard.hpp"
#include "rendering/m2_track_sampler.hpp"

namespace wowee::rendering {
inline constexpr uint32_t kParticleNoTrail = 0x10;
// Detached particles travel in yards (particleWorldVelocityBasis).
inline constexpr uint32_t kParticleTravelsInYards = 0x8;
inline constexpr uint32_t kParticleInModelSpace = 0x80;
// Pre-264 flag 0x4000 keeps particles in the animated emitter bone's frame
// (model * bone), just like NoTrail (0x10) (285 of 16070 TBC emitters):
// - Zul'jin's claws (Hand_1H_ZulAman_D_01Right.m2, 0x4429/0x4629, item 33975)
//   keep their frost wisps on the claw while the wielder jumps or swings.
// - Pain Suppression (33206, PainSuppression_State, 0x4029) stays round a
//   running player.
// - Murmur's mace/hand/head/shoulder sonic rings (Murmur.m2 #2-#5/#7-#8,
//   0x4229, bone 84 mace tip, 2.0 s life) stay centered on the animated mace
//   bone as Murmur sways and swings in build 8606; keeping them only in
//   model-root space left a 2 s lingering trail of rings above his shoulder.
// Emitters without 0x10/0x80/0x4000 trail in world space: Frostguard's
// Shaman_Frost (0x209/0x9) and Widow's Remorse's smoke (0x22d).
inline constexpr uint32_t kParticleFollowsModel = 0x4000;
inline constexpr uint32_t kParticleBoneFrame = kParticleNoTrail | kParticleFollowsModel;
inline constexpr uint32_t kParticleModelFrame = kParticleInModelSpace | kParticleFollowsModel;
// Sprites lie flat in the XY plane of their emitter bone, as placed in the
// world (particleQuadAxes). Ground rings stand on unrotated bones
// (WaterElemental #16 WhiteRingNoiseWater, #21 Shockwave10, flags 0x1001);
// 145 of 363 TBC XYQuad emitters sit under a rotated or billboard bone.
inline constexpr uint32_t kParticleXYQuad = 0x1000;
inline constexpr uint32_t kParticlePinned = 0x400;
// Pre-264 flag 0x8000 is M2 Squirt: on discrete (interpolationType 0) rate
// tracks each positive keyframe fires an instant burst of that many particles
// when the sequence clock crosses its timestamp (Blizzard_Impact_Base #3/#4
// 35 glow + 35 snowflakes at 800 ms, Consecration_Impact_Base #0 200 sparks at
// 67 ms, RainOfFire_Impact_Base #0 35 flames at 967 ms, ElementalEarth and
// Ghost death bursts). On a held last frame (a corpse at the end of Death)
// no keyframes are crossed, and linear 0x8000 tracks also stop emitting there.
inline constexpr uint32_t kParticleSquirt = 0x8000;
inline constexpr uint32_t kParticleStopsOnHeldFrame = kParticleSquirt;
// Particle vertex: position 3, colour 4, half width 1, cell 1, tail or
// XYQuad X axis 3, rotation 1 (radians), XYQuad Y axis 3.
inline constexpr size_t kParticleVertexFloats = 16;

/// World axes an XYQuad sprite spans: the emitter bone's X and Y as the
/// placed model carries them (`placedBone` = model matrix * bone matrix),
/// unit length. Time Rift (display 18877, time_rift1.m2) has its ring
/// emitters 0-2 (0x1219/0x121b) on bones 28-30 under bone 17, which turns
/// 90 degrees about Y, under bone 3, a Z-cylindrical billboard: the ring
/// faces the camera in the original. Laid in the model's XY plane it lay
/// flat over the ground like a halo.
inline std::pair<glm::vec3, glm::vec3> particleQuadAxes(const glm::mat4& placedBone) {
    return {glm::normalize(glm::vec3(placedBone[0])), glm::normalize(glm::vec3(placedBone[1]))};
}

// Pre-264 emitter +0x198 is a spin rate in radians per second: a sprite
// turns by spin * age. Christmas Tree (Medium), GO 178557 (spin 0.5, 3 s life,
// SPARKLE.BLP drawn as an X) shows the star mostly as a + in the original,
// most of a quarter turn by the sprite's brightest age; unrotated every sprite
// stood as the same X. 8191 of 16070 TBC emitters set it. Turning direction
// is counter-clockwise on screen for positive spin; not checked against
// the original.
inline float particleRotation(float spin, float ageSeconds) {
    return spin * ageSeconds;
}
// Emitters with flag 0x1 keep their authored colour inside a WMO interior;
// the others take the doodad's MODD placement colour. 689 of the 781 emitter
// kinds placed in TBC interiors carry it: torches, candles, braziers, glowing
// crystals. Water, smoke and steam vents mostly do not.
inline constexpr uint32_t kParticleOwnColor = 0x1;

/// Effective RGB color for an M2 particle emitter.
/// Emitters with `kParticleOwnColor` (0x1) or additive blend modes (`M2Blend`
/// 3 NoAlphaAdd / 4 AddAlpha) keep their authored color, ignoring both
/// scene/MODD placement lighting and instance-level color tint:
/// - `kParticleOwnColor` (0x1): `Torch.m2` (0x21), `candle01.m2` (0x39), and
///   SpellVisualKit CharProc 1 (`39383` Fury of Medivh / `Archimonde_Fire`,
///   `348` Immolate, `35409` Brain Damage / `ArcanePower_State_Chest` #0).
/// - Additive `M2Blend` 3/4 without 0x1 (303 TBC emitters, including
///   `Etherial.m2` #0-#24 `0x238`/`0x28`, `HHGroundMount.m2` `0x8`, Shaman
///   totems `0x400`, and Hyjal/ZA raid shoulders `0x230`): emissive additive
///   glows stay full-bright authored color rather than being darkened by
///   ambient/MODD lighting.
/// Non-additive emitters without 0x1 (alpha smoke/waterfalls/dust, `M2Blend` 2)
/// take both `placementLight` and `instanceTint`.
inline glm::vec3 particleEffectiveColor(uint32_t flags,
                                        const glm::vec3& authoredColor,
                                        const glm::vec3& placementLight,
                                        const glm::vec3& instanceTint,
                                        uint8_t blendType = 2) {
    if ((flags & kParticleOwnColor) || blendType == 3 || blendType == 4) return authoredColor;
    return authoredColor * placementLight * instanceTint;
}

// Flag 0x40 (`InheritVelocity` / `UseBurstMultiplier`, 240 TBC emitters, 0 with
// child models): in TBC 2.4.3 parent/root emitters (e.g. WaterElemental #2/#3/#6/#7/#23/#24
// with 0xc49 and burstMultiplier 2.0, Ice_Precast_Med_Hand 0x79/0x169 with 2.0-3.0,
// ShamanisticRage_State_Hand 0x69 with 3.0) do NOT add `(delta / dt) * burstMultiplier`
// to detached particle velocity; doing so launched WaterElemental arm/death splashes
// 10-20 yd into the sky on swings, runs, and death bone motion.
inline constexpr uint32_t kParticleInheritVelocity = 0x40;

// Twinkle size of a pre-264 emitter's particle (+0x180 speed, +0x184 percent,
// +0x188/+0x18C scale min/max): a random factor between min and max, drawn
// again every 1/speed seconds of the particle's age (once, for speed 0).
// When `twinklePercent < 1.0` (72 TBC emitters, e.g. Archimonde_Fire #1 with
// speed 11, percent 0.9, scale 1.0-1.0; DetectInvis_Impact_Base #0/#1 with
// percent 0.5; ArcanePower_State_Chest #0 with percent 0.6), each twinkle
// period is visible with probability `twinklePercent` and blinks off (`0.0f`)
// otherwise, using an independent hash mix so visibility and scale stay
// uncorrelated.
inline float particleTwinkleScale(uint32_t seed, float age, float speed,
                                  float scaleMin, float scaleMax,
                                  float twinklePercent = 1.0f) {
    const bool canBlinkOff = twinklePercent < 1.0f;
    if (!canBlinkOff && scaleMin == scaleMax) return scaleMin;
    if (twinklePercent <= 0.0f) return 0.0f;
    const uint32_t period = speed > 0.0f ? static_cast<uint32_t>(age * speed) : 0u;
    uint32_t h = seed ^ (period * 0x9E3779B9u);
    h ^= h >> 16; h *= 0x7FEB352Du; h ^= h >> 15; h *= 0x846CA68Bu; h ^= h >> 16;
    if (canBlinkOff) {
        uint32_t hv = h ^ 0xA5A5A5A5u;
        hv ^= hv >> 16; hv *= 0x7FEB352Du; hv ^= hv >> 15; hv *= 0x846CA68Bu; hv ^= hv >> 16;
        const float uVis = static_cast<float>(hv >> 8) / 16777216.0f;
        if (uVis >= twinklePercent) return 0.0f;
    }
    if (scaleMin == scaleMax) return scaleMin;
    return scaleMin + (scaleMax - scaleMin) * (static_cast<float>(h >> 8) / 16777215.0f);
}

/// Static wind integration for pre-264 `windVector` (+0x1B4, yards/s) over
/// `windTime` (+0x1C0, seconds). Applies uniform acceleration
/// `windVector / windTime` during `age in [0, windTime]` (integrating to a
/// total velocity impulse of `windVector` over `windTime`) and returns
/// `{deltaPos, deltaVel}` for a step `[prevAge, prevAge + dt]`.
/// All 14 TBC wind emitters author `windTime > 0` (Blacksmith_Smoke &
/// Smokestack 6.3886 s, ShadowFangFog01 & EtherealStorm 20 s, WaterElemental
/// #10 & Waterfall-long 1.0 s).
inline std::pair<glm::vec3, glm::vec3> particleWindStep(const glm::vec3& windDir,
                                                        float windTime,
                                                        float prevAge,
                                                        float dt) {
    if (windTime <= 0.0f || dt <= 0.0f) return {glm::vec3(0.0f), glm::vec3(0.0f)};
    if (glm::dot(windDir, windDir) <= 1e-12f) return {glm::vec3(0.0f), glm::vec3(0.0f)};
    const float activeDt = std::clamp(windTime - prevAge, 0.0f, dt);
    if (activeDt <= 0.0f) return {glm::vec3(0.0f), glm::vec3(0.0f)};
    const glm::vec3 accel = windDir / windTime;
    const glm::vec3 dv = accel * activeDt;
    const float coastDt = dt - activeDt;
    const glm::vec3 dp = accel * (0.5f * activeDt * activeDt) + dv * coastDt;
    return {dp, dv};
}

// Drag (emitter +0x194, pre-264 layout) scales speed by exp(-drag * t).
// Travel over dt at unit starting speed is the integral of that. Zero drag
// is straight-line motion. BFD_WallLight03 #0 authors 5.5: its sparks stop
// within 0.2 yd of the basin instead of rising for their 4 s life.
// 249 TBC emitters author negative drag (Blink_Impact_Chest #8 -8.95): they
// speed up by the same law.
inline float particleDragTravel(float drag, float dt) {
    if (drag == 0.0f) return dt;
    return (1.0f - std::exp(-drag * dt)) / drag;
}

// A tail quad runs from the particle back along -velocity * span, where span
// integrates drag over rawSpan (particleDragTravel(drag, rawSpan) when
// drag > 0, else rawSpan):
// - Purge_Impact_Chest.m2 #1 (flags 0x19, speed 0.556 yd/s, drag 30, tailLength
//   4.75 s) decays over 1/30 s = 0.033 s, keeping newborn tails ~0.018 yd rather
//   than 2.6 yd spikes.
// - Holy_Precast_High_Base.m2 #0/#2 (Chain Heal precast ground sunburst, flags
//   0xb9, drag 0.5/0.8, tailLength 0.40/0.35 s, peak alpha at age 0.0/0.12 s)
//   keeps its full authored radial streaks at birth and peak alpha (clamping
//   unpinned tailLength to particle age erased #0 at birth and truncated #2 by 3x).
// Pinned tails (0x400) clamp their time span to min(tailLength, age). 2204 of
// the 3522 TBC tail emitters are pinned, many authoring tailLength 10 s:
// - At zero gravity, -velocity * min(tailLength, age) ends at the particle's
//   birth point at most, so Bloodlust/Summon hand rings (0x479, 0.28 yd/s, 10 s)
//   and fire precast hand sparks (0x439, -1.67 yd/s inward, 5.45 s) stay inside
//   the hand instead of drawing 3-9 yd spokes, while Acid breath drops (0x429,
//   4.44 yd/s, 0.1 s) keep their short velocity tail once age > 0.1 s.
// - Under acceleration (gravity != 0), -velocity * min(tailLength, age) still
//   uses instantaneous velocity rather than clamping to the birth point:
//   Etherial.m2 #20 (0x400) and EtherialRobe.m2 #15 (0x401, gravity +69.444,
//   tailLength 0.8 s, life 0.5 s) stretch their tail upward to
//   z_birth + 0.5 * g * age^2 above the rising wrappings, forming the tall
//   magenta/crimson death flame pillar seen in native TBC 2.4.3.
inline glm::vec3 particleTailVector(uint32_t flags, const glm::vec3& velocity, float tailLength,
                                    float age, float drag = 0.0f) {
    const float rawSpan = (flags & kParticlePinned)
        ? std::min(tailLength, std::max(age, 0.0f))
        : tailLength;
    const float span = drag > 0.0f ? particleDragTravel(drag, rawSpan) : rawSpan;
    return -velocity * span;
}

// M2/WMO placements carry uniform scale in their transform. Matrix-created
// doodads have no separate scalar placement value; measure their basis.
inline float particlePlacementScale(const glm::mat4& model) {
    return glm::length(glm::vec3(model[0]));
}

// Billboard half-width: the authored lifecycle scale, at placement scale only
// for emitters with flag 0x20. No caps or floors; smoke plumes author scales
// well above one yard. 10935 of 16070 TBC emitters set 0x20 (pywowlib calls
// it "unlit"; its observed effect is the sprite size).
// - With 0x20: LootFX.m2 stars #1-#3 (0x29, 0x229) are about three times as
//   large over GnomeBigGear (sparkle scale 3.5) as over a sign (1) in the
//   original; Mark of Kael'Thas (GO 185170, size 5, 0x521) burns in flames
//   several yards wide; the Exodar runes shrink and grow with their placement.
// - Without 0x20: LootFX.m2's flare #0 (0x101) stays 0.13-0.18 yd across
//   (median blob) at sparkle scales 1, 3.5 and 4.7 in the original, while its
//   spread and rise grow; Light of Elune (feralaslightblue.m2, 0x1) drops the
//   same small dots at GO sizes 0.5, 2 and 5.
// - NoTrail (0x10) emitters with flag 0x200 take the placement scale without
//   0x20: Time Rift (display 18877, scale 4, time_rift1.m2) draws its rings
//   (#0/#1, 0x1219, authored half width about 2.5) about 19 yd across at 58 yd
//   in the original, its toon smoke (#3, 0x219) as a cloud of the same size;
//   at authored size WoWee drew them a quarter as large
//   (_experiments/20261119T082500Z.orient_shadow/, sweep_fix).
// - NoTrail (0x10) emitters without 0x20 or 0x200 keep authored size:
//   Combat Rage 41251 on Bonechewer Combatant (display 21550, scale 3.25) and
//   .modify scale 3 Human plays DruidMorph_Impact_Base.m2 (#1 0x1f, #2 0x1b
//   TOONSMOKE16.BLP, #4 0x19 stardust) as discrete ~1.0x puffs while #3
//   (0x439, with 0x20) scales 3.25x; scaling all 0x10 emitters blew #1/#2 into
//   a giant opaque cloud over the combatant's head.
// - XYQuad (0x1000) emitters with both NoTrail (0x10) and 0x20 lie in the
//   emitter bone's placed XY plane (already scaled by placementScale) while
//   0x20 also scales the sprite size by placementScale, compounding to
//   placementScale^2. On Lady Malande (display 21419, scale 2.1,
//   Helm_Robe_RaidPriest_F_01_BeF.m2 emitter #2, 0x1239, authored 0.222-0.25),
//   native draws the halo ring at ~2.1x the single-scaled radius (~1.05 yd
//   radius vs ~0.50 yd), whereas at net scale 1.05 native and WoWee match.
inline constexpr uint32_t kParticleScaledSprite = 0x20;
inline constexpr uint32_t kParticleScaledNoTrail = 0x200;
inline bool particleScaleAppliesToSize(uint32_t flags) {
    return (flags & kParticleScaledSprite) != 0 ||
           ((flags & kParticleNoTrail) != 0 && (flags & kParticleScaledNoTrail) != 0);
}
inline float particleSpriteHalfWidth(uint32_t flags, float authoredScale, float placementScale) {
    float size = particleScaleAppliesToSize(flags) ? authoredScale * placementScale : authoredScale;
    if ((flags & (kParticleXYQuad | kParticleScaledSprite | kParticleNoTrail)) ==
        (kParticleXYQuad | kParticleScaledSprite | kParticleNoTrail)) {
        size *= placementScale;
    }
    return size;
}

/// Orientation/scale basis for a detached geometry-model particle mesh.
/// Follows `particleScaleAppliesToSize`: emitters without `0x20` (e.g.
/// `Whirlwind_State_Base.m2` #0/#1, flags `0x0d`, on Combat Rage 41251)
/// keep authored mesh scale (normalized basis columns) while preserving
/// emitter rotation; emitters with `0x20` (e.g. `SandVortex.m2` `0x28`,
/// `BoneArmor_Head.m2` `0x629`) inherit placement scale from `birthBasis`.
inline glm::mat3 particleMeshBasis(uint32_t flags, const glm::mat3& birthBasis) {
    if (particleScaleAppliesToSize(flags)) return birthBasis;
    glm::mat3 basis = birthBasis;
    for (int c = 0; c < 3; ++c) {
        const float len = glm::length(basis[c]);
        basis[c] = len > 1e-6f ? (basis[c] / len) : glm::vec3(0.0f);
    }
    return basis;
}

// NoTrail (0x10) and FollowsModel (0x4000) particles remain attached to the
// animated emitter bone (model * bone). ModelSpace (0x80) particles retain
// their birth bone pose in the placed model's frame (model). Other particles
// live in world space. Bit 0x20 sizes sprites (particleSpriteHalfWidth); it
// does not select a simulation coordinate system.
inline glm::mat4 particleRenderTransform(uint32_t flags, const glm::mat4& model,
                                         const glm::mat4& bone) {
    if (flags & kParticleBoneFrame) return model * bone;
    return (flags & kParticleInModelSpace) ? model : glm::mat4(1.0f);
}

inline glm::mat4 particleBirthTransform(uint32_t flags,
                                       const glm::mat4& model, const glm::mat4& bone) {
    if (flags & kParticleBoneFrame) return glm::mat4(1.0f);
    return (flags & kParticleInModelSpace) ? bone : model * bone;
}

// Emitter position of a particle born `age` seconds before the end of a step
// of length dt, with the emitter moving linearly from `previous` to `current`
// over that step.
inline glm::vec3 particleBirthPosition(const glm::mat4& previous, const glm::mat4& current,
                                       const glm::vec3& local, float age, float dt) {
    const float back = dt > 0.0f ? std::fmin(std::fmax(age / dt, 0.0f), 1.0f) : 0.0f;
    return glm::mix(glm::vec3(current * glm::vec4(local, 1.0f)),
                    glm::vec3(previous * glm::vec4(local, 1.0f)), back);
}

inline bool particleScalesWithPlacement(bool isGameObject, bool externalPose,
                                        uint32_t flags = 0) {
    if (!isGameObject && !externalPose) return true;
    // GameObject and CharacterRenderer (unit / attached-item) emitters with
    // flag 0x20 (kParticleScaledSprite) scale detached particle velocity by
    // their placement/particle scale to match their scaled sprite size:
    // - Priestess of Delight/Dementia & Mother Shahraz (Shivan.m2 /
    //   HaremMatron.m2, display scale 2.5, crown flames #2 0x29 at 5.56 yd/s
    //   for 0.35 s and #3 0x229 at 1.56 yd/s for 1.0 s, grav 0) shoot a ~4.9 yd
    //   tapering flame jet above the crown in native 2.4.3; unscaled yards left
    //   1.56-1.94 yd travel inside the 1.74-2.08 yd birth sprite radius as a
    //   round ball.
    // - Corrupted/Cleansed Windblossom (GO 164887 / 164884, Windblossom02.m2 /
    //   Windblossom01.m2, scale 2.0, flags 0x429) send their pinned light
    //   pillars twice as high as unscaled yards.
    // Emitters without 0x20 (Sunwell gate barrier GO 188118, scale 8.69, 0x409;
    // Shaman totems 0x9/0x409/0x40b) keep moving in yards.
    return (flags & kParticleScaledSprite) != 0;
}

inline bool particleGravityScalesWithPlacement(bool isGameObject, bool externalPose) {
    // Only map doodads and spell visuals scale 0x8 gravitational acceleration
    // by placement. Units and GameObjects keep 0x8 gravity in world yards/s^2
    // even when 0x20 scales sprite size and initial velocity: Netherspite's
    // Void Zone (PortalofKathune.m2, display scale 0.1, flags 0x229, vRange 0
    // horizontal velocity, gravity -1.39) still lifts its smoke 1.56 yd over
    // 1.5 s rather than 0.156 yd.
    return !isGameObject && !externalPose;
}

inline bool particleMovesInYards(uint32_t flags, bool scalesWithPlacement);

inline glm::vec3 particleGravityDirection(uint32_t flags, const glm::mat4& model,
                                          bool scalesWithPlacement) {
    // Gravity pulls along world -Z, never the emitter bone's or a pitched
    // attachment bone's -Z: Core Hound drool falls straight down while its head
    // bone is pitched, and bone-oriented hand/breath effects (Fire_Precast_Hand,
    // AcidLiquidBreath) still rise/fall vertically when the hand/head bone pitches.
    // NoTrail and ModelFrame particles are simulated in their local frame.
    // Detached particles without flag 0x8 (or on map doodads/spell visuals) scale
    // gravity by the placement scale, matching particleWorldVelocityBasis.
    if (flags & (kParticleNoTrail | kParticleModelFrame)) return {0, 0, -1};
    const float scale = particleMovesInYards(flags, scalesWithPlacement)
        ? 1.0f : particlePlacementScale(model);
    return {0.0f, 0.0f, -scale};
}

// Flag 0x8: a detached particle (neither NoTrail nor in the model's frame)
// on a game object or unit travels in yards, whatever the placement scale;
// without it, its velocity and gravity take the placement scale as its birth
// point does. 12654 of 16070 TBC emitters set it, 7020 of them detached.
// - With 0x8: the Sunwell gate barrier (GO 188118, SUNWELLRAID_GATE_04.m2,
//   scale 8.69, 0x409) otherwise throws its sparks 8.7x too fast, past the
//   doorway, as radial streaks; Netherspite's Void Zone (PortalofKathune.m2,
//   display scale 0.1, 0x229) lifts its smoke a tenth of the height.
// - Without 0x8: the quest-object sparkle's flare (LootFX.m2 #0, 0x101,
//   0.56 yd/s for 1.6 s) rises 0.4-0.5 yd at scale 1 and 3-4 yd over
//   GnomeBigGear (scale 3.5) in the original; in yards it stayed a band at
//   the gear's hub. Light of Elune 177415/177418 (feralaslightblue.m2, 0x1,
//   GO sizes 2 and 5) fall in columns as long as the object is large.
// Attached particles live in the scaled bone or model frame: Brutallus' back
// flames (0x339, NoTrail, scale 6) rise with the model's scale in the original.
// Gravity follows the velocity's rule; it is not observed separately for
// emitters without 0x8.
// Map doodads (ADT MDDF and WMO MODD placements) and spell visuals move at
// their placement scale with 0x8 as well. Magisters' Terrace's throne orb
// (BloodElf_PowerOrb_Green.m2, MODD 964, scale 0.358, smoke #5/#6 0x629 at
// 2.2-2.8 yd/s) lifts its green wisps about 1 yd over the orb in the original;
// in yards they rose 4-5 yd up the throne. A Wild Shore bonfire (OrcBonfire.m2
// at -14695.8 503.2, scale 0.47, flames #1 0x29 at 1.11 yd/s for 2 s) keeps
// its flames inside the log pile. Warlock Hellfire (1949/5857,
// HellFire_Impact_Caster_Base.m2, #0 0x8009) scales both its projected ground
// decal and its fire-ring particle velocity by the caster's Base attachment
// scale (0.5x on Gnome, 1.0x on Human, 2.0x at scale 2) in build 8606; keeping
// 0x8 in yards left the fire ring at 1.0x so the 0.5x ground decal looked half
// the ring's diameter on a Gnome caster
// (_experiments/20261120T130700Z.hellfire_ground_decal).
inline bool particleMovesInYards(uint32_t flags, bool scalesWithPlacement) {
    return !scalesWithPlacement && !(flags & (kParticleNoTrail | kParticleModelFrame)) &&
           (flags & kParticleTravelsInYards);
}

inline glm::mat3 particleWorldVelocityBasis(uint32_t flags, const glm::mat4& birthTransform,
                                            bool scalesWithPlacement,
                                            float placementScaleOverride = -1.0f) {
    glm::mat3 basis(birthTransform);
    if (flags & (kParticleNoTrail | kParticleModelFrame)) return basis;
    if (particleMovesInYards(flags, scalesWithPlacement)) {
        for (int c = 0; c < 3; ++c) {
            const float len = glm::length(basis[c]);
            basis[c] = len > 1e-6f ? (basis[c] / len) : glm::vec3(0.0f);
        }
        return basis;
    }
    if (placementScaleOverride >= 0.0f) {
        for (int c = 0; c < 3; ++c) {
            const float len = glm::length(basis[c]);
            basis[c] = len > 1e-6f ? (basis[c] * (placementScaleOverride / len)) : glm::vec3(0.0f);
        }
    }
    return basis;
}

struct ParticleEmission {
    glm::vec3 offset;
    glm::vec3 direction;
};

// Sphere emitter (type 2) birth point and travel direction.
// The area length / width tracks are the minimum / maximum radius; a birth
// takes a radius between them. The vertical range is an elevation above the
// model's horizontal plane, the horizontal range an azimuth about +Z.
// Birth offset is `direction * sampledRadius` (including negative radii: a
// negative radius places the birth on the opposite side of the sphere while
// keeping velocity along `+direction`, so positive speed implodes inward across
// the cylinder — e.g. Etherial.m2 Death #20 and EtherialRobe.m2 Death #15 with
// radius -1.111..-0.833 and speed +2.778 form a ~2.5-3.0 yd wide tapering
// vertical flame pillar in native TBC 2.4.3).
// Particles travel along `direction` (including at zero radius, e.g.
// Strike_Impact_Chest.m2 #1 on Flurry 12970 / melee impacts, 546 TBC sphere
// emitters), or up +Z with flag 0x100.
// Native evidence:
// - Shadow Nova 30852 (shadow_nova_area.m2 #0, vertical 0, horizontal pi,
//   radius to 9.7, 0x2209, kit scale 1): a smoke band spreading flat over
//   the ground round the caster.
// - Meeting Stone Summoning Portal 179944 (Summon_Ritual.m2, vertical pi,
//   horizontal 0): a ring standing in the model's YZ plane.
// - Etherial.m2 Death #20 / EtherialRobe.m2 Death #15 (radius -1.111..-0.833,
//   vertical 0, horizontal pi, speed 2.778, gravity +69.444, flags 0x400/0x401):
//   wide magenta/crimson vertical flame pillar engulfing the rising wrappings.
/// M2 particle `zSource` (track #9, parsed into `em.deceleration`): when non-zero,
/// particles travel away from `(0, 0, zSource)` through their local spawn offset
/// instead of using the default sphere/plane angular direction (465 TBC emitters,
/// e.g. Etherial.m2 #17-#19 and EtherialRobe.m2 #12-#14 with `zSource = -1.11`
/// rise in an upward cone rather than shooting horizontally sideways).
inline glm::vec3 zSourceParticleDirection(const glm::vec3& localOffset, float zSource,
                                          const glm::vec3& fallbackDirection) {
    if (std::abs(zSource) <= 1e-5f) return fallbackDirection;
    const glm::vec3 fromZSource = localOffset - glm::vec3(0.0f, 0.0f, zSource);
    const float lenSq = glm::dot(fromZSource, fromZSource);
    if (lenSq > 1e-10f) return fromZSource / std::sqrt(lenSq);
    return glm::vec3(0.0f, 0.0f, zSource < 0.0f ? 1.0f : -1.0f);
}

inline ParticleEmission sphericalParticleEmission(float length, float width,
    float elevation, float azimuth, float radius, uint32_t flags,
    float zSource = 0.0f) {
    const glm::vec3 direction(std::cos(elevation) * std::sin(azimuth),
                              std::cos(elevation) * std::cos(azimuth), std::sin(elevation));
    const float sampledRadius = glm::mix(length, width, radius);
    const glm::vec3 offset = direction * sampledRadius;
    if ((flags & 0x100u) != 0) {
        return {offset, glm::vec3(0, 0, 1)};
    }
    return {offset, zSourceParticleDirection(offset, zSource, direction)};
}

// Plane emitter (type 1) birth offset in the emitter bone's XY plane.
// M2 emissionAreaLength spans Y and emissionAreaWidth spans X:
// elwynntallwaterfall01.m2 (mesh dX=9.9, dY=25.8, bone 1 rotated 66.5 deg about
// Y) authors length=18.0, width=1.37; newwaterfall.m2 (dX=1.9, dY=4.8) authors
// length=2.61, width=0.69. Placing length on X rotated the 18 yd mist band into
// a vertical wall shooting above the waterfall lip. Across TBC anisotropic
// plane emitters, length-on-Y matches the mesh's long axis 54 to 18.
inline glm::vec3 planeParticleOffset(float length, float width, float randX, float randY) {
    return {randX * width * 0.5f, randY * length * 0.5f, 0.0f};
}

// Outdoor lighting for emitters without kParticleOwnColor (0x1): TBC 2.4.3
// billboard particles have no surface normal facing the directional sun, so
// they take the zone's ambient plus an isotropic spherical-average sun diffuse
// term (0.35 * max(-lightDir.z, 0) ~ 0.25 at mid-morning). Full upward-normal
// diffuse over-lit Ammen Vale waterfalls (elwynntallwaterfall01.m2, Light 454
// ambient 0.25/0.40/0.53 vs warm diffuse 0.78/0.62/0.44) by 1.53x in red,
// while pure ambient under-lit the mist by ~25%.
inline glm::vec3 outdoorParticleLight(const glm::vec3& ambient, const glm::vec3& diffuse,
                                      const glm::vec3& lightDir) {
    return glm::clamp(ambient + diffuse * (0.35f * std::max(-lightDir.z, 0.0f)),
                      glm::vec3(0.0f), glm::vec3(1.0f));
}

/// Outdoor lighting for server-visible entity particles and lit ribbons
/// (GameObjects, units, and spell visuals) without kParticleOwnColor (0x1).
/// Unlike static ADT map doodads (above), TBC 2.4.3 lights dynamic entity
/// emitters with the upward outdoor rig scaled by the zone's AreaTable
/// AmbientMultiplier gain (unitAmbGain = 1 + 2 * AmbientMultiplier): at Mirage
/// Raceway at night (Light 230, ambient 0/0.118/0.235, direct 0.279/0.421/0.709,
/// -lightDir.z = 0.518, Thousand Needles AmbientMultiplier = 0.5 -> gain 2.0),
/// Blacksmith_smoke.m2 (GO 1604) and SandVortex.m2#1 (NPC 19922) render cool
/// blue-grey (~0.29, 0.67, 1.00) rather than near-black (~0.10, 0.27, 0.48).
inline glm::vec3 outdoorEntityParticleLight(const glm::vec3& ambient, const glm::vec3& diffuse,
                                            const glm::vec3& lightDir, float unitAmbGain) {
    const glm::vec3 base = ambient + diffuse * std::max(-lightDir.z, 0.0f);
    return glm::clamp(base * std::max(unitAmbGain, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f));
}

// Plane emitter (type 1) travel direction: inclination from +Z, azimuth from
// +Y (matching sphericalParticleEmission). At horizontal range 0 the fan lies
// in the emitter bone's YZ plane: Hurricane 16914 (LightningStorm_Cloud_State
// #0, vertical pi, horizontal 0 on bone 3, whose Y and Z lie horizontal and
// whose X points up +Z) spins a flat cloud at z = 9.2 yd in native 2.4.3.
inline glm::vec3 planeParticleDirection(float inclination, float azimuth) {
    return {std::sin(inclination) * std::sin(azimuth),
            std::sin(inclination) * std::cos(azimuth),
            std::cos(inclination)};
}

/// Continuous (non-Squirt) emitter accumulator step over `dt` seconds.
/// Scaling each frame's step by `random01` in [0, 1] yields mean emission rate
/// `0.5 * rate` (independent of frame rate) and natural stochastic puff spacing,
/// matching TBC 2.4.3 continuous smoke, mist, and spark emitters (e.g.
/// `d_smoke.m2` on the crashed Exodar escape pod and `ashtreesmoke01.m2` in
/// Ammen Vale).
inline float particleContinuousEmissionIncrement(float rate, float dt, float random01) {
    assert(random01 >= 0.0f && random01 <= 1.0f);
    return rate * dt * random01;
}

/// True when a continuous emitter's rate or enabledIn track contains any idle
/// (`<= 0`) keyframe in `sequenceIndex` (or its global sequence), marking it as
/// a windowed/pulsed emitter rather than a steady-state ambient emitter.
/// In TBC 2.4.3 `M2ParticleOld`, an idle emitter (`rate <= 0` or disabled) keeps
/// its next-spawn countdown at zero so its leading particle spawns immediately
/// upon entering an active `rate > 0` pulse: e.g. all 6 `ArcaneForceShield_*.m2`
/// models (33810 Rock Shell, em[0..4] `0 -> 10.02 -> 0` over 133-167 ms,
/// integral ~0.67-0.84) and 154 short-pulse TBC `Spells/*.m2` emitters.
inline bool emitterHasIdlePhase(const pipeline::M2AnimationTrack& emissionRate,
                                const pipeline::M2AnimationTrack& enabledIn,
                                int sequenceIndex) {
    auto trackHasNonPositive = [&](const pipeline::M2AnimationTrack& track) {
        if (track.sequences.empty()) return false;
        const size_t idx = track.globalSequence >= 0 ? 0u : static_cast<size_t>(std::max(sequenceIndex, 0));
        if (idx >= track.sequences.size()) return false;
        for (const float v : track.sequences[idx].floatValues) {
            if (v <= 0.0f) return true;
        }
        return false;
    };
    return trackHasNonPositive(emissionRate) || trackHasNonPositive(enabledIn);
}

/// Initial continuous-emitter accumulator: `1.0f` for pulsed/windowed emitters
/// (so an immediate `[0..100ms]` pulse also spawns its leading particle at
/// `t = 0`), and `random01` in `[0, 1)` for steady-state ambient emitters (so
/// multiple placed smoke/mist doodads do not puff in lockstep on spawn).
inline float initialContinuousEmitterAccumulator(bool hasIdlePhase, float random01) {
    assert(random01 >= 0.0f && random01 <= 1.0f);
    return hasIdlePhase ? 1.0f : std::min(random01, 0.999999f);
}

/// Discrete Squirt (`0x8000`) particle count for one crossed positive rate
/// keyframe. Like `particleContinuousEmissionIncrement`, pre-264 TBC 2.4.3
/// emitters draw `keyValue * density * random01` with `random01` uniform in
/// [0, 1] (WotLK v264 later replaced this implicit [0, rate] draw with explicit
/// `emissionRateVary`). Rounding `keyValue * density * random01` preserves exact
/// mean `0.5 * keyValue * density` across `[0 .. round(keyValue * density)]`
/// (e.g. `2.0` -> mean `1.0` on `wisp.m2` #10 / `decisivestrike_impact_chest.m2`
/// #2, and `30.0` -> mean `15.0` across `[0..30]` on `Strike_Impact_Chest.m2` #1
/// for Warrior `Flurry` `12970` / `Mortal Strike` `30330`, matching build 8606).
inline uint32_t particleSquirtBurstCount(float keyValue, float density, float random01) {
    assert(random01 >= 0.0f && random01 <= 1.0f);
    const float scaled = keyValue * density;
    if (scaled <= 0.0f) return 0u;
    return static_cast<uint32_t>(std::floor(scaled * random01 + 0.5f));
}

/// Discrete Squirt (0x8000, interpolationType == 0) bursts crossed during one
/// step of an emitter's sequence clock. Each crossed positive rate keyframe
/// whose EnabledIn track is on at that keyframe's timestamp draws one uniform
/// `sampleRandom01()` in [0, 1] and invokes `onBurst(count, ageSeconds, localKeyMs)`
/// when `particleSquirtBurstCount(val, density, random01) > 0`.
template <typename RandomFn, typename Fn>
inline void forEachSquirtBurst(
    const pipeline::M2AnimationTrack& emissionRate,
    const pipeline::M2AnimationTrack& enabledIn,
    int sequenceIndex,
    float prevAnimTimeMs,
    float currAnimTimeMs,
    float animDurationMs,
    bool includeStartBoundary,
    float globalSequenceTimeMs,
    const std::vector<uint32_t>& globalSequenceDurations,
    float density,
    float stepSeconds,
    RandomFn&& sampleRandom01,
    Fn&& onBurst) {
    if (emissionRate.interpolationType != 0) return;
    if (sequenceIndex < 0 || static_cast<size_t>(sequenceIndex) >= emissionRate.sequences.size()) return;
    const auto& keys = emissionRate.sequences[static_cast<size_t>(sequenceIndex)];
    const size_t count = std::min(keys.timestamps.size(), keys.floatValues.size());
    if (count == 0) return;

    auto emitRange = [&](float loMs, float hiMs, bool closedLo, float ageOffsetMs) {
        if (hiMs < loMs || (!closedLo && hiMs == loMs)) return;
        for (size_t k = 0; k < count; ++k) {
            const float val = keys.floatValues[k];
            if (val <= 0.0f) continue;
            const float localKeyMs =
                static_cast<float>(keys.timestamps[k]) - static_cast<float>(keys.timeOffset);
            const bool afterLo = closedLo ? (localKeyMs >= loMs) : (localKeyMs > loMs);
            if (!afterLo || localKeyMs > hiMs) continue;
            if (m2_track::sampleFloat(enabledIn, sequenceIndex, localKeyMs,
                                      globalSequenceTimeMs, globalSequenceDurations, 1.0f) <= 0.0f) {
                continue;
            }
            const uint32_t burstCount = particleSquirtBurstCount(val, density, sampleRandom01());
            if (burstCount == 0) continue;
            const float ageSec = std::clamp(((hiMs - localKeyMs) + ageOffsetMs) * 0.001f,
                                            0.0f, std::max(0.0f, stepSeconds));
            onBurst(burstCount, ageSec, localKeyMs);
        }
    };

    if (!includeStartBoundary && currAnimTimeMs < prevAnimTimeMs && animDurationMs > 0.0f) {
        emitRange(prevAnimTimeMs, animDurationMs, /*closedLo=*/false, currAnimTimeMs);
        emitRange(0.0f, currAnimTimeMs, /*closedLo=*/true, 0.0f);
    } else {
        emitRange(prevAnimTimeMs, currAnimTimeMs, includeStartBoundary, 0.0f);
    }
}

/// Whether a float track takes more than one value in this sequence.
inline bool particleTrackVaries(const pipeline::M2AnimationTrack& track, int sequence) {
    if (track.sequences.empty()) return false;
    const size_t index = track.globalSequence >= 0 ? 0u : static_cast<size_t>(sequence);
    if (index >= track.sequences.size()) return false;
    const auto& values = track.sequences[index].floatValues;
    return std::adjacent_find(values.begin(), values.end(), std::not_equal_to<>()) != values.end();
}

/// Path parameter of a spline birth, from the area length / width tracks.
/// Width held at one value: births spread uniformly between length and
/// width. Shadowmoon_Rune2 (constant 0 / 1; Witch's Sanctum, Ground Rune
/// 183036) draws its #0 streaks as a fence all round its ring in the
/// original; at the path end they stood in one purple column.
/// Only width animated (length constant): births stand at width's current
/// value. FlameCircle.m2 (width 0 -> 1 over each 1 s Stand, length 0) is a
/// flame front running round the Molten Core rune once a second.
/// Both length and width animated: they bound a moving window [length, width]
/// along the path (SummonPet_Impact_Base.m2 [0.0->0.82->0.9, 0.17->1.0->1.0]
/// and SummonPet_Cast_Impact_Base.m2 [0.9->0.2->0.0, 1.0->0.0->0.0]).
inline float splineBirthParameter(float length, float width, bool lengthVaries,
                                  bool widthVaries, float random01) {
    const float t = (widthVaries && !lengthVaries) ? width : glm::mix(length, width, random01);
    return std::clamp(t, 0.0f, 1.0f);
}

inline float splineBirthParameter(float length, float width, bool widthVaries, float random01) {
    return splineBirthParameter(length, width, /*lengthVaries=*/false, widthVaries, random01);
}

/// Converts a raw M2 spline emitter point into M2 bone space (+90 deg about Z).
/// M2 spline control points are authored rotated -90 deg about Z relative to
/// bone space: SummonPet_Cast_Impact_Base.m2 (bones 9..12 map +Y to +Z_world)
/// and SummonPet_Impact_Base.m2 (bones 7..11 map +Y to +Z_world) author their
/// vertical arcs along X; FlameStrike_Impact.m2 (Z-cylindrical billboard bone 0)
/// and BurningSpirit_Impact_Head.m2 (spherical billboard bone 2) author their
/// camera-facing ribbons in XZ rather than the billboard's YZ plane.
inline glm::vec3 splinePointToBoneSpace(const glm::vec3& raw) {
    return {-raw.y, raw.x, raw.z};
}

/// A point on a spline emitter's path at `t` in [0, 1], equal parameter per
/// cubic Bezier segment (knot, handle, handle, knot; 3k + 1 points).
inline glm::vec3 splineEmissionPoint(const std::vector<glm::vec3>& points, float t) {
    assert(points.size() >= 4 && (points.size() - 1) % 3 == 0);
    assert(t >= 0.0f && t <= 1.0f);
    const size_t segments = (points.size() - 1) / 3;
    const float scaled = t * static_cast<float>(segments);
    const size_t segment = std::min(static_cast<size_t>(scaled), segments - 1);
    const float u = scaled - static_cast<float>(segment);
    const float v = 1.0f - u;
    const glm::vec3* p = &points[segment * 3];
    return v * v * v * p[0] + 3.0f * v * v * u * p[1] + 3.0f * v * u * u * p[2] + u * u * u * p[3];
}

/// Which blend state an M2Blend particle mode draws with. The fragment shader
/// takes the mode itself for the per-mode alpha handling.
enum class ParticleBlendPipeline { Alpha, Additive, Mod, Mod2x };
inline ParticleBlendPipeline particleBlendPipeline(uint8_t m2Blend) {
    assert(m2Blend <= 6 && "M2Blend particle mode out of range");
    switch (m2Blend) {
        case 3: case 4: return ParticleBlendPipeline::Additive;
        case 5: return ParticleBlendPipeline::Mod;
        case 6: return ParticleBlendPipeline::Mod2x;
        default: return ParticleBlendPipeline::Alpha;  // 0 opaque, 1 alpha key, 2 alpha
    }
}

/// One authored cell range played over `phase` in [0, 1): `repeat` passes
/// from start to end inclusive, either direction (ShadowWordSilence_Breath
/// runs 31 -> 16). Wrapped into the texture's tile count, so an end equal to
/// the tile count (0..7 / 8..16 on 16 tiles, 680 TBC emitters) shows cell 0
/// for the final step.
inline uint32_t particleCellInRange(const pipeline::M2ParticleEmitter::CellRange& range,
                                    float phase, uint32_t tiles) {
    const int repeat = range.repeat > 0 ? range.repeat : 1;
    float f = phase * static_cast<float>(repeat);
    f -= std::floor(f);
    const int span = range.end - range.start;
    const int count = (span >= 0 ? span : -span) + 1;
    int step = static_cast<int>(f * static_cast<float>(count));
    if (step >= count) step = count - 1;
    const int cell = range.start + (span >= 0 ? step : -step);
    const int t = static_cast<int>(tiles);
    return static_cast<uint32_t>(((cell % t) + t) % t);
}

/// The texture cell a particle shows at `lifeRatio`, head or tail sprite.
inline uint32_t particleCell(const pipeline::M2ParticleEmitter& em, float lifeRatio,
                             bool tail, uint32_t tiles) {
    if (tiles <= 1) return 0;
    lifeRatio = std::fmin(std::fmax(lifeRatio, 0.0f), 0.99999f);
    if (!em.legacyCells) {
        const auto& fb = tail ? em.tailCellTrack : em.headCellTrack;
        if (fb.floatValues.empty()) return 0;
        float value = fb.floatValues.back();
        for (size_t i = 0; i + 1 < fb.timestamps.size() && i + 1 < fb.floatValues.size(); ++i) {
            if (lifeRatio <= fb.timestamps[i + 1]) {
                const float dt = fb.timestamps[i + 1] - fb.timestamps[i];
                const float k = dt > 0.0f ? (lifeRatio - fb.timestamps[i]) / dt : 0.0f;
                value = fb.floatValues[i] + (fb.floatValues[i + 1] - fb.floatValues[i]) * k;
                break;
            }
        }
        return static_cast<uint32_t>(std::floor(value)) % tiles;
    }
    const float mid = em.midPoint;
    const bool head = lifeRatio < mid;
    const float phase = head ? (mid > 0.0f ? lifeRatio / mid : 0.0f)
                             : (mid < 1.0f ? (lifeRatio - mid) / (1.0f - mid) : 0.0f);
    if (tail) return particleCellInRange(head ? em.tailCells : em.tailDecayCells, phase, tiles);
    return particleCellInRange(head ? em.headCells : em.decayCells, phase, tiles);
}
/// Vertical displacement of an M2 ribbon edge whose age advances from
/// `prevAge` to `newAge` under `gravity` (positive rises along +Z).
/// In WoW M2 ribbons (e.g. Shaman elemental totems with gravity=+5,
/// edgeLifetime=0.51s; Cyclone +5; BlackMagic_Precast +10; DemonicSacrifice
/// -0.5), the cumulative vertical offset at age `t` is `gravity * t^2 * scaleZ`,
/// independent of frame rate.
inline float ribbonGravityStepZ(float gravity, float prevAge, float newAge, float scaleZ = 1.0f) {
    return gravity * (newAge * newAge - prevAge * prevAge) * scaleZ;
}

/// Texture U coordinate `[0 -> 1]` for an M2 ribbon edge of `age` seconds over
/// `edgeLifetime` seconds. Mapping U from edge age instead of deque index keeps
/// head-to-tail gradient textures (such as `ArrowFlight_01.m2`
/// `SPELLS\GRADIENT128.BLP`) at their authored world length during initial
/// launch ramp-up and fade-out instead of compressing `[0, 1]` into the first
/// few edges. Native 2.4.3 `M2Ribbon` relies on the ribbon texture's U gradient
/// (and authored `alphaTrack`) rather than multiplying an extra `1 - age /
/// edgeLifetime` vertex-alpha fade on top.
inline float ribbonEdgeU(float age, float edgeLifetime) {
    if (edgeLifetime <= 1e-5f) return 0.0f;
    return std::clamp(age / edgeLifetime, 0.0f, 1.0f);
}

/// Interpolate a looping M2 sequence clock at `fraction` in `[0, 1]` from
/// `prevAnimTimeMs` to `currAnimTimeMs`, unwrapping across `animDurationMs`
/// when a looping sequence wraps (`currAnimTimeMs < prevAnimTimeMs`).
inline float interpolateSequenceTimeMs(float prevAnimTimeMs, float currAnimTimeMs,
                                       float animDurationMs, float fraction) {
    const float t = std::clamp(fraction, 0.0f, 1.0f);
    if (currAnimTimeMs < prevAnimTimeMs && animDurationMs > 0.0f) {
        const float unwrapped = glm::mix(prevAnimTimeMs, currAnimTimeMs + animDurationMs, t);
        float wrapped = std::fmod(unwrapped, animDurationMs);
        if (wrapped < 0.0f) wrapped += animDurationMs;
        return wrapped;
    }
    return glm::mix(prevAnimTimeMs, currAnimTimeMs, t);
}

/// Evaluate the model-space bone matrix of `boneIdx` at `(sequenceIndex,
/// animTimeMs, globalSequenceTimeMs)` by walking only its ancestor chain from
/// root down to `boneIdx` (typically 2-5 bones), including camera-facing
/// billboard bones (`WarpStorm.m2` bone 4 spherical billboard -> bones 18..22 ->
/// ribbon emitter bones 30..35). Used to sub-step fast-spinning ribbon emitters
/// when `edgesPerSecond * dt >= 1` spawns multiple edges in one frame.
inline glm::mat4 evaluateRibbonBoneTransform(
    const std::vector<pipeline::M2Bone>& bones,
    const std::vector<uint32_t>& globalSequenceDurations,
    uint32_t boneIdx,
    int sequenceIndex,
    float animTimeMs,
    float globalSequenceTimeMs,
    const glm::mat4& invModelMatrix,
    const glm::vec3* cameraPosWorld = nullptr) {
    if (boneIdx >= bones.size()) return glm::mat4(1.0f);
    uint32_t chain[32];
    size_t depth = 0;
    int32_t cur = static_cast<int32_t>(boneIdx);
    while (cur >= 0 && static_cast<size_t>(cur) < bones.size() && depth < 32) {
        chain[depth++] = static_cast<uint32_t>(cur);
        const int32_t parent = bones[cur].parentBone;
        if (parent == cur) break;
        cur = parent;
    }

    const glm::vec3 camModel = cameraPosWorld
        ? glm::vec3(invModelMatrix * glm::vec4(*cameraPosWorld, 1.0f))
        : glm::vec3(0.0f);
    glm::vec3 upModel(0.0f, 0.0f, 1.0f);
    if (cameraPosWorld) {
        const glm::vec3 rawUp = glm::vec3(invModelMatrix * glm::vec4(0.0f, 0.0f, 1.0f, 0.0f));
        const float upLen = glm::length(rawUp);
        if (upLen > 1e-6f) upModel = rawUp / upLen;
    }

    glm::mat4 mat(1.0f);
    for (size_t k = depth; k > 0; --k) {
        const auto& bone = bones[chain[k - 1]];
        const glm::vec3 trans = m2_track::sampleVec3(
            bone.translation, sequenceIndex, animTimeMs, globalSequenceTimeMs,
            globalSequenceDurations, glm::vec3(0.0f));
        const glm::quat rot = m2_track::sampleQuat(
            bone.rotation, sequenceIndex, animTimeMs, globalSequenceTimeMs,
            globalSequenceDurations);
        const glm::vec3 scl = m2_track::sampleVec3(
            bone.scale, sequenceIndex, animTimeMs, globalSequenceTimeMs,
            globalSequenceDurations, glm::vec3(1.0f));

        if (cameraPosWorld && isBillboardBone(bone.flags)) {
            mat = billboardBoneMatrix(bone.flags, bone.pivot, trans, scl, mat, camModel, upModel);
        } else {
            glm::mat4 local = glm::translate(glm::mat4(1.0f), bone.pivot + trans);
            local *= glm::toMat4(rot);
            local = glm::scale(local, scl);
            local = glm::translate(local, -bone.pivot);
            mat = mat * local;
        }
    }
    return mat;
}

/// Alpha-blended M2 ribbons (M2Blend <= 2: Opaque/AlphaKey/Alpha) sort before
/// transparent M2 meshes and additive particles so dark/black shadow trails
/// (e.g. Banish_Chest_Purple.m2, Shadow_ImpactDD_Uber_Chest.m2,
/// Shadowbolt_Missile.m2, blend=2 RGB=(0,0,0)) composite underneath the
/// spell's additive glow rather than punching opaque black lines through it.
inline constexpr bool ribbonDrawsBeforeTransparent(uint16_t blendMode) {
    return blendMode <= 2u;
}

/// For a one-shot CastKit/ImpactKit M2 that authors both Stand (0) and Hold (158)
/// state sequences (e.g. Spells\BeastRageState.m2 in Enrage 15716, impact kit 373):
/// if the model has no mesh vertices or ribbons and every particle emitter has
/// enabledIn == 0 throughout Stand (0) (only turning on at Hold 158 / Decay 159),
/// return the Hold (158) sequence index so the one-shot visual actually emits
/// immediately instead of spending its entire Stand lifetime invisible.
/// Returns -1 when normal Stand (0) playback should be used.
inline int oneShotStateEffectStartSequence(
    uint32_t vertexCount,
    size_t ribbonCount,
    const std::vector<pipeline::M2Sequence>& sequences,
    const std::vector<pipeline::M2ParticleEmitter>& emitters,
    const std::vector<uint32_t>& globalSequenceDurations) {
    if (vertexCount != 0 || ribbonCount != 0 || emitters.empty() || sequences.empty())
        return -1;
    int standIdx = -1;
    int holdIdx = -1;
    for (int i = 0; i < static_cast<int>(sequences.size()); ++i) {
        if (sequences[i].id == 0 && standIdx < 0) standIdx = i;
        if (sequences[i].id == 158 && holdIdx < 0) holdIdx = i;
    }
    if (standIdx < 0 || holdIdx < 0) return -1;

    const float standDurMs = static_cast<float>(sequences[standIdx].duration);
    for (const auto& em : emitters) {
        // An emitter with no enabledIn track is always enabled during Stand.
        if (!em.enabledIn.hasData()) return -1;
        if (m2_track::sampleFloat(em.enabledIn, standIdx, 0.0f, 0.0f,
                                  globalSequenceDurations, 1.0f) > 0.0f) {
            return -1;
        }
        const int keySeq = (em.enabledIn.globalSequence >= 0) ? 0 : standIdx;
        if (keySeq >= 0 && keySeq < static_cast<int>(em.enabledIn.sequences.size())) {
            const auto& sk = em.enabledIn.sequences[keySeq];
            const size_t n = std::min(sk.timestamps.size(), sk.floatValues.size());
            for (size_t k = 0; k < n; ++k) {
                const float tLocal = static_cast<float>(sk.timestamps[k]) - static_cast<float>(sk.timeOffset);
                if (tLocal >= 0.0f && tLocal < standDurMs && sk.floatValues[k] > 0.0f) {
                    return -1;
                }
            }
        }
    }
    return holdIdx;
}

} // namespace wowee::rendering
