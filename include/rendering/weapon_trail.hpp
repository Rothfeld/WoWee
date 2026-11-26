#pragma once

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>
#include <glm/glm.hpp>

namespace wowee {
namespace rendering {

/// Parsed SpellVisualKit.dbc CharProc == 8 (weapon swing trail) parameters.
///
/// In TBC 2.4.3 SpellVisualKit.dbc (66 melee/ability kits):
/// - CharParamZero  : 0xRRGGBB base trail color (stored as float)
/// - CharParamOne   : fadeOutRate (alpha units in 0..255 decayed per 50ms tick)
/// - CharParamTwo   : duration in milliseconds; native 2.4.3 truncates via
///                    integer division by 1000 to whole seconds, so kits with
///                    CharParamTwo < 1000 (e.g. Mortal Strike/Heroic Strike
///                    kit 324 at 600ms, Cleave kit 366 at 600ms, Sinister
///                    Strike kit 399 at 400ms) produce 0s (no trail), while
///                    kits >= 1000ms (Whirlwind kit 369 at 2000ms -> 2s,
///                    Charge/Surge kit 44 and Tackle Hit kit 8456 at 1000ms -> 1s)
///                    emit weapon trails for that whole-second duration.
/// - CharParamThree : initial alpha in 0..255 (e.g. 100 -> 100/255 = 0.392)
struct WeaponTrailSpec {
    glm::vec3 color{1.0f};
    float fadeOutRate = 20.0f;
    float durationSec = 0.0f;
    float alpha = 0.0f;
};

/// Parse a SpellVisualKit CharProc == 8 entry. Returns nullopt when the
/// truncated whole-second duration (`uint32_t(paramTwo) / 1000`) is zero or
/// alpha is zero (invisible).
std::optional<WeaponTrailSpec> parseWeaponTrailCharProc(float paramZero, float paramOne,
                                                        float paramTwo, float paramThree);

/// Compute the local-space [base, tip] blade segment of a melee weapon M2 from
/// its tight bind-pose visual bounds.
/// Standard weapons (918/961 TBC weapons) extend along +X from grip (0) to tip (+maxX).
/// Fist/claw weapons (hand_1h_*) extend along -Z.
std::pair<glm::vec3, glm::vec3> weaponBladeLocalSpan(const glm::vec3& visualBoundMin,
                                                     const glm::vec3& visualBoundMax);

/// One sampled cross-section of a weapon swing ribbon in world space.
struct WeaponTrailEdge {
    glm::vec3 base{0.0f};
    glm::vec3 tip{0.0f};
    float alpha = 0.0f;
};

/// Active ribbon state for one equipped weapon during a CharProc == 8 trail.
struct WeaponTrailChannel {
    uint32_t weaponInstanceId = 0;
    float sampleAccum = 0.0f;
    std::vector<WeaponTrailEdge> edges;  // oldest -> newest
};

/// Active CharProc == 8 trail on a unit (may drive both main-hand and off-hand).
struct ActiveUnitWeaponTrail {
    uint32_t ownerInstanceId = 0;
    WeaponTrailSpec spec;
    float elapsed = 0.0f;
    float headAlpha = 0.0f;
    bool fromChannel = false;
    uint32_t sourceAuraVisualId = 0;
    std::vector<WeaponTrailChannel> channels;
};

/// Exponential head-alpha decay time constant (300 ms) over the second half of
/// a CharProc == 8 weapon trail's active duration.
inline constexpr float kWeaponTrailFadeTauSec = 0.300f;

/// Render RGB color for a CharProc == 8 weapon swing trail strip.
/// Side-by-side pixel measurements against native TBC 2.4.3 across dirt, snow,
/// and salt terrains (Whirlwind kits 369/370, Charge kit 44) give
/// `fg / bg == (1 - alpha)` identically across R, G, and B: native renders the
/// ribbon as a subtle translucent black motion-blur shadow (`SrcRGB = (0,0,0)`)
/// modulated by `edge.alpha`, never an unlit neon RGB polygon.
[[nodiscard]] constexpr glm::vec3 weaponTrailRenderColor(const WeaponTrailSpec& /*spec*/) noexcept {
    return glm::vec3(0.0f, 0.0f, 0.0f);
}

/// True when an attached weapon is eligible to emit a SpellVisualKit
/// CharProc == 8 swing trail on native TBC 2.4.3:
/// 1. Player-equipped melee weapon (`!unitLatchesWeaponsOnAnim`; creature
///    virtual weapons on ObjectType::UNIT never emit CharProc == 8 trails,
///    e.g. Rend Blackhand 10429, Illidan 22917, Leotheras 21215, Cabal
///    Executioner 18632).
/// 2. Hand-held melee slot (`attachmentId` 1 or 2, `weaponHeldSheathState == 1`).
/// 3. Currently drawn in hand (`!weaponCurrentlySheathed`, including temporary
///    WeaponFlags 0x20 animation draws such as Whirlwind 126 on a sheathed
///    player; once a 0x20 animation ends on a sheathed player, the weapon
///    returns to its sheath point and stops emitting).
/// 4. Weapon M2 authors both `$WTB` (base) and `$WTT` (tip) event anchors.
[[nodiscard]] constexpr bool canEmitWeaponTrail(
    bool unitLatchesWeaponsOnAnim,
    uint32_t attachmentId,
    int8_t weaponHeldSheathState,
    bool weaponCurrentlySheathed,
    bool hasTrailBaseAndTipAnchors) noexcept {
    if (unitLatchesWeaponsOnAnim) return false;
    if (attachmentId != 1u && attachmentId != 2u) return false;
    if (weaponHeldSheathState != 1) return false;
    if (weaponCurrentlySheathed) return false;
    return hasTrailBaseAndTipAnchors;
}

/// Step the active weapon trail head alpha matching native TBC 2.4.3: holds
/// full alpha until half duration (`elapsedSec <= durationSec * 0.5f`), then
/// decays headAlpha exponentially with tau = 300 ms (minimum 1/255 step) and
/// terminates once headAlpha drops below 1/255 so post-Whirlwind swings and
/// movement do not emit full-opacity broad trails.
[[nodiscard]] float stepWeaponTrailHeadAlpha(float currentHeadAlpha,
                                             float elapsedSec,
                                             float durationSec,
                                             float dt);

/// Cap a ChannelKit CharProc == 8 weapon trail duration (e.g. kit 370 at 10s)
/// to the active channel's remaining duration (e.g. 2s for Cabal Executioner
/// Whirlwind 33500) so the trail never outlives the channel onto subsequent
/// normal melee swings.
[[nodiscard]] float resolveChannelWeaponTrailDuration(float kitDurationSec,
                                                      float channelRemainingSec);

/// Halt new edge emission on an active trail immediately (e.g. when a channel
/// or aura ends) while letting already-emitted quads fade out smoothly.
void stopWeaponTrailEmission(ActiveUnitWeaponTrail& trail);

/// Sampling cadence and movement thresholds for weapon trail ribbon edges.
inline constexpr float kWeaponTrailSampleIntervalSec = 0.020f;
inline constexpr float kWeaponTrailMinMoveSq = 0.02f * 0.02f;
inline constexpr size_t kWeaponTrailMaxEdges = 64;
inline constexpr int kWeaponTrailMaxSubsteps = 16;

/// Number of evenly spaced weapon pose substeps to sample across a single frame
/// so fast spins (e.g. Whirlwind at ~720 deg/s) produce smooth circular arcs
/// instead of coarse straight-edged chords at low frame rates.
[[nodiscard]] int weaponTrailSubstepCount(float deltaTime,
                                          float maxStepSec = kWeaponTrailSampleIntervalSec);

/// Advance one weapon trail channel by dt seconds, decaying existing edges and
/// optionally recording the weapon's current [base, tip] world blade segment
/// while emitting is true. When `headAlphaOverride >= 0.0f`, newly emitted
/// edges use that alpha instead of `spec.alpha`.
void stepWeaponTrailChannel(WeaponTrailChannel& channel, const WeaponTrailSpec& spec,
                            float dt, bool emitting,
                            const std::optional<std::pair<glm::vec3, glm::vec3>>& bladeWorld,
                            float headAlphaOverride = -1.0f);

/// True when the channel has no visible ribbon left to draw (fewer than 2 edges
/// or all remaining edges have faded to zero).
bool isWeaponTrailChannelDead(const WeaponTrailChannel& channel);

}  // namespace rendering
}  // namespace wowee
