#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <glm/glm.hpp>

namespace wowee::rendering {

inline constexpr float kEntityFloorMaxStepUp = 1.5f;
// TBC 2.4.3 downward ground-snap reach for grounded units: empirically verified
// on Native TBC 2.4.3 across Mana Snapper 18883 (+4.5 yd), Nexus-King Salhadaar
// 20454 (+7.56 yd), and Shimmerwing Moth 20611 (+14.04 yd and +19.73 yd), while
// preventing 90+ yd drops into deep under-WMO terrain pits (e.g. Auchindoun -8.92
// over -101.40) while WMO/M2 geometry is still streaming in.
inline constexpr float kEntityFloorMaxDrop = 50.0f;
// Active spline/predicted movers only accept an uncontinuous >3.0 yd drop when
// stationary; across transient 1-frame WMO/M2 bridge seams, an elevated walker
// must not snap down to distant terrain below.
inline constexpr float kEntityFloorMaxActiveDrop = 3.0f;
// Raw terrain fast-path tolerance: CMaNGOS UpdateGroundPositionZ adds +0.05 yd
// on bare terrain, whereas low M2 mounds/steps/curbs (+0.15..+0.75 yd) sit
// higher and must still consult WMO and M2 floor geometry.
inline constexpr float kEntityFloorTerrainExactTolerance = 0.08f;
inline constexpr float kEntityFloorStructureSnapTolerance = 0.35f;

// TBC 2.4.3 MovementFlags bits that keep a unit at its authoritative server/spline Z
// instead of snapping it down to ground (verified via static_assert in application.cpp).
inline constexpr uint32_t kMoveFlagOnTransport = 0x00000200u;
inline constexpr uint32_t kMoveFlagLevitating  = 0x00000400u;
inline constexpr uint32_t kMoveFlagFalling     = 0x00001000u;
inline constexpr uint32_t kMoveFlagFallingFar  = 0x00002000u;
inline constexpr uint32_t kMoveFlagSwimming    = 0x00200000u;
inline constexpr uint32_t kMoveFlagFlying      = 0x02000000u;
inline constexpr uint32_t kMoveFlagHover       = 0x40000000u;

inline constexpr uint32_t kSplineFlagFalling   = 0x00000002u;
inline constexpr uint32_t kSplineFlagFlying    = 0x00000200u;

/// Returns true when a unit's movement flags or active server spline place it in
/// an airborne, levitating, hovering, falling, swimming, or transport-attached
/// state that must preserve authoritative Z rather than snapping to floor.
/// Note: CAN_FLY (0x01000000) alone is intentionally NOT included because a unit
/// with flight capability walking on the ground still snaps to ground until
/// FLYING, LEVITATING, or HOVER is active.
inline constexpr bool unitSuppressesFloorSnap(uint32_t moveFlags,
                                              uint32_t serverSplineFlags = 0u,
                                              bool activelyMoving = false) noexcept {
    constexpr uint32_t kNonGroundMask =
        kMoveFlagOnTransport |
        kMoveFlagLevitating |
        kMoveFlagFalling |
        kMoveFlagFallingFar |
        kMoveFlagSwimming |
        kMoveFlagFlying |
        kMoveFlagHover;
    if ((moveFlags & kNonGroundMask) != 0u) {
        return true;
    }
    if (activelyMoving && (serverSplineFlags & (kSplineFlagFalling | kSplineFlagFlying)) != 0u) {
        return true;
    }
    return false;
}

/// Arbitrates between terrain, WMO, and M2 floor heights for a grounded unit.
///
/// - `renderPos`: unit's reference render position (authoritative server Z when
///   `!isSplineOrMoving`, or interpolated spline/movement Z when `isSplineOrMoving`).
/// - `previousRenderPos`: unit's previous frame rendered position, if any.
/// - `isSplineOrMoving`: true when the unit is actively moving or holding a
///   server-spline arrival position (such as CMaNGOS MoveCharge, which adds +1.0 yd
///   to spline destination Z and therefore must not snap onto a nearby ~1 yd M2
///   fence or crate without horizontal continuity).
inline std::optional<float> arbitrateEntityFloor(
    const glm::vec3& renderPos,
    const std::optional<glm::vec3>& previousRenderPos,
    bool isSplineOrMoving,
    std::optional<float> terrainHeight,
    std::optional<float> wmoHeight,
    std::optional<float> m2Height) {
    const float probeZ = renderPos.z + kEntityFloorMaxStepUp;
    const float minZ = renderPos.z - kEntityFloorMaxDrop;

    std::optional<float> terrainFloor;
    std::optional<float> wmoFloor;
    std::optional<float> m2Floor;
    std::optional<float> best;
    bool bestIsTerrain = false;

    const auto inWindow = [&](const std::optional<float>& floor) {
        return floor && *floor <= probeZ && *floor >= minZ;
    };

    const auto consider = [&](const std::optional<float>& floor, bool isTerrain = false) {
        if (!inWindow(floor)) return;
        if (isTerrain) terrainFloor = floor;
        if (!best || std::abs(*floor - renderPos.z) < std::abs(*best - renderPos.z)) {
            best = floor;
            bestIsTerrain = isTerrain;
        }
    };

    consider(terrainHeight, true);
    if (inWindow(wmoHeight)) wmoFloor = wmoHeight;
    if (inWindow(m2Height)) m2Floor = m2Height;
    consider(wmoHeight, false);
    consider(m2Height, false);

    if (best && !bestIsTerrain) {
        const float serverDelta = std::abs(*best - renderPos.z);
        // A stationary non-spline unit's renderPos.z is its authoritative server Z
        // (never inflated by MoveCharge's +1.0 yd spline lift). When an authored
        // WMO/M2 floor matches that server Z within 0.35 yd, accept it even if
        // previousRenderPos temporarily fell to terrain while the model streamed in.
        if (!isSplineOrMoving && serverDelta <= kEntityFloorStructureSnapTolerance) {
            return best;
        }
        // A grounded unit spawned or placed above a WMO floor (or, when stationary,
        // above a collidable M2 rock/platform such as Netherstorm overhang rocks)
        // within kEntityFloorMaxDrop must ground onto the solid surface below it,
        // matching outdoor terrain grounding. Never snap UP to an overhead ceiling
        // or arch (*floor > renderPos.z).
        std::optional<float> structureFloorBelow;
        if (wmoFloor && *wmoFloor <= renderPos.z &&
            (!terrainFloor || *wmoFloor >= *terrainFloor)) {
            structureFloorBelow = wmoFloor;
        }
        if (!isSplineOrMoving && m2Floor && *m2Floor <= renderPos.z &&
            (!terrainFloor || *m2Floor >= *terrainFloor)) {
            structureFloorBelow = structureFloorBelow
                ? std::optional<float>(std::max(*structureFloorBelow, *m2Floor))
                : m2Floor;
        }
        if (!previousRenderPos) {
            if (serverDelta > kEntityFloorStructureSnapTolerance) {
                return structureFloorBelow ? structureFloorBelow : terrainFloor;
            }
        } else {
            const glm::vec2 planarDelta = glm::vec2(renderPos) - glm::vec2(*previousRenderPos);
            const float planarLen = glm::length(planarDelta);
            const float maxContinuousStep = 0.45f + planarLen * 1.5f;
            const auto isContinuous = [&](const std::optional<float>& cand) {
                return inWindow(cand) &&
                       std::abs(*cand - previousRenderPos->z) <= maxContinuousStep;
            };
            if ((planarLen < 4.0f || serverDelta > kEntityFloorStructureSnapTolerance) &&
                !isContinuous(best)) {
                if (isContinuous(wmoFloor)) return wmoFloor;
                if (isContinuous(terrainFloor)) return terrainFloor;
                if (structureFloorBelow) return structureFloorBelow;
                if (isSplineOrMoving && terrainFloor &&
                    (renderPos.z - *terrainFloor) > kEntityFloorMaxActiveDrop) {
                    return std::nullopt;
                }
                return terrainFloor;
            }
        }
    } else if (best && bestIsTerrain && isSplineOrMoving && previousRenderPos) {
        // Active mover on an elevated bridge/platform hitting a transient 1-frame
        // WMO/M2 seam miss: preserve elevated spline Z if terrain is >3 yd below
        // renderPos.z and discontinuous with previousRenderPos->z. If the unit was
        // already grounded near terrainFloor on the previous frame, continuity passes
        // and keeps it smoothly on terrainFloor.
        const glm::vec2 planarDelta = glm::vec2(renderPos) - glm::vec2(*previousRenderPos);
        const float maxContinuousStep = 0.45f + glm::length(planarDelta) * 1.5f;
        if ((renderPos.z - *best) > kEntityFloorMaxActiveDrop &&
            std::abs(*best - previousRenderPos->z) > maxContinuousStep) {
            return std::nullopt;
        }
    }
    return best;
}

} // namespace wowee::rendering
