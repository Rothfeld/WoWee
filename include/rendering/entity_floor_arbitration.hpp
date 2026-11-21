#pragma once

#include <cmath>
#include <optional>
#include <glm/glm.hpp>

namespace wowee::rendering {

inline constexpr float kEntityFloorMaxStepUp = 1.5f;
inline constexpr float kEntityFloorMaxDrop = 3.0f;
// Raw terrain fast-path tolerance: CMaNGOS UpdateGroundPositionZ adds +0.05 yd
// on bare terrain, whereas low M2 mounds/steps/curbs (+0.15..+0.75 yd) sit
// higher and must still consult WMO and M2 floor geometry.
inline constexpr float kEntityFloorTerrainExactTolerance = 0.08f;
inline constexpr float kEntityFloorStructureSnapTolerance = 0.35f;

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
        if (!previousRenderPos) {
            if (serverDelta > kEntityFloorStructureSnapTolerance) {
                return terrainFloor;
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
                return terrainFloor;
            }
        }
    }
    return best;
}

} // namespace wowee::rendering
