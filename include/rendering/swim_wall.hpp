#pragma once

#include <optional>

namespace wowee::rendering {

/// A swimmer's horizontal position. Only x and y: the swim clamp owns the
/// vertical, and this decides nothing about it.
struct SwimStep {
    float x = 0.0f;
    float y = 0.0f;
};

/// Where a swimmer ends up when the terrain ahead is a wall rather than a floor.
///
/// The swept collision a swimmer gets knows WMO walls and doodad collision, and
/// a shoreline is neither - it is the heightmap, which the swim path otherwise
/// only ever consults downward, for something to float above. So nothing stopped
/// a swimmer crossing into a hillside, and the floor probe does not push them
/// back out: it accepts a floor only at or just above the feet, and a cliff face
/// ahead of them is far above that. Leaving the water inside the hill then drops
/// them through the world.
///
/// `wallZ` is the height above which terrain counts as a wall rather than
/// something to float over - the swimmer's feet plus the clearance their float
/// gives them. A beach shelving up underneath stays passable; a cliff does not.
///
/// Each axis is tried alone before both are refused, so a swimmer meeting the
/// shore at an angle follows it along instead of sticking to it.
///
/// A swimmer whose own spot is already under the heightfield is in a liquid
/// beneath the ground - a WMO's - and the heightfield is a ceiling there, not
/// a shore: C'Thun's stomach acid (surface -98.0) lies under Ahn'Qiraj's
/// desert (204.8), and build 8606 swims about in it.
template <typename HeightAt>
SwimStep swimStepAgainstTerrain(const SwimStep& from, const SwimStep& to, float wallZ,
                                HeightAt&& heightAt) {
    const auto blocked = [&](float x, float y) {
        const std::optional<float> h = heightAt(x, y);
        return h.has_value() && *h > wallZ;
    };
    if (blocked(from.x, from.y)) return to;
    if (!blocked(to.x, to.y)) return to;
    if (!blocked(to.x, from.y)) return {.x = to.x, .y = from.y};
    if (!blocked(from.x, to.y)) return {.x = from.x, .y = to.y};
    return from;
}

/// The heightfield as the bed of a liquid the swimmer is in: only where it
/// lies at or below `probeZ`, the height the WMO and M2 floor queries probe
/// from. Ground higher than that is above the swimmer. Taking Ahn'Qiraj's
/// desert (204.8) as the bed of C'Thun's stomach acid (surface -98.0, bed
/// -106.6) made the acid 300 yd too shallow to swim in.
inline std::optional<float> liquidBedTerrain(const std::optional<float>& terrain, float probeZ) {
    return terrain && *terrain <= probeZ ? terrain : std::nullopt;
}

/// Whether a character at `feetZ` in water with surface `waterH` and optional
/// floor `floorH` is in swimming state.
///
/// Entering swim (`!alreadySwimming`) requires the character's own feet to be
/// at least `enterDepth` (`swimSurfaceOffset_`) below `waterH` AND the water
/// column over `floorH` (when present) to be at least `enterDepth`. Checking
/// only `waterH - floorH` triggered swimming the instant a falling character's
/// toes touched `waterH` (stopping them at the surface instead of plunging into
/// the water column as native 2.4.3 does in Slave Pens and Dustwallow bay).
/// Grounded walk-in has `feetZ == *floorH`, so it enters at the exact same depth.
///
/// While already swimming, `exitDepth` hysteresis keeps the existing floor/feet
/// depth test so shore exits remain smooth.
inline bool shouldBeInWater(bool alreadySwimming, float waterH, float feetZ,
                            const std::optional<float>& floorH,
                            float enterDepth, float exitDepth) {
    const float depthFromFeet = waterH - feetZ;
    if (!alreadySwimming) {
        if (depthFromFeet < enterDepth) return false;
        return !floorH || (waterH - *floorH) >= enterDepth;
    }
    return (floorH && (waterH - *floorH) >= exitDepth) ||
           (!floorH && depthFromFeet >= exitDepth);
}

/// Retained downward vertical velocity on the frame a falling character enters
/// swimming (`feetZ <= waterH - swimSurfaceOffset_`).
///
/// Native 2.4.3 carries a falling character past float depth and holds them
/// underwater (Dustwallow bay `waterH = 0.0`: 1 yd drop -> -1.84 yd, 6 yd ->
/// -1.80 yd, 15 yd -> -2.22 yd, 20 yd 5-trial mean -> -2.11 yd, 40 yd -> -2.42 yd;
/// Slave Pens `waterH = 8.20`: 12 yd drop -> ~6.17 yd underwater). Retaining 20%
/// of downward entry speed into the existing `SWIM_VERTICAL_DAMPING = 6.0 /s`
/// deceleration yields frame-rate-independent plunge depth `|v| / 30` (~0.23 yd
/// at 1 yd drop up to ~0.95 yd at 40 yd drop), stopping smoothly in ~0.1 s.
inline constexpr float kWaterEntryVelocityRetention = 0.20f;
inline float waterEntryVerticalVelocity(float impactVerticalVelocity) {
    return impactVerticalVelocity < 0.0f
        ? impactVerticalVelocity * kWaterEntryVelocityRetention
        : 0.0f;
}

}  // namespace wowee::rendering
