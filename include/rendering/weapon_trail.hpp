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
    std::vector<WeaponTrailChannel> channels;
};

/// Sampling cadence and movement thresholds for weapon trail ribbon edges.
inline constexpr float kWeaponTrailSampleIntervalSec = 0.020f;
inline constexpr float kWeaponTrailMinMoveSq = 0.02f * 0.02f;
inline constexpr size_t kWeaponTrailMaxEdges = 64;

/// Advance one weapon trail channel by dt seconds, decaying existing edges and
/// optionally recording the weapon's current [base, tip] world blade segment
/// while emitting is true.
void stepWeaponTrailChannel(WeaponTrailChannel& channel, const WeaponTrailSpec& spec,
                            float dt, bool emitting,
                            const std::optional<std::pair<glm::vec3, glm::vec3>>& bladeWorld);

/// True when the channel has no visible ribbon left to draw (fewer than 2 edges
/// or all remaining edges have faded to zero).
bool isWeaponTrailChannelDead(const WeaponTrailChannel& channel);

}  // namespace rendering
}  // namespace wowee
