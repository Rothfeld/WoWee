#pragma once

#include <cstdint>

namespace wowee::game {

// TBC MoveSplineFlag::Flying (0x200): CMaNGOS MoveSplineInit sets it on every
// spline of a unit with FLYING, HOVER, CAN_FLY or LEVITATING, and the original
// flies a unit on such a spline (Fly 135) whatever its own movement flags say
// (_experiments/20261118T085229Z.sapphiron_liftoff). SMSG_MONSTER_MOVE and the
// spline a create block carries for a unit already under way are the same
// spline: Dragonmaw Sky Stalkers (spawn template HOVER) come into view mid-path
// with HOVER in their flags and Flying only on the spline.
inline constexpr uint32_t kTbcSplineFlying = 0x200u;
inline constexpr uint32_t kTbcSplineDone = 0x1u;

/// Movement flags a create block reports for a unit, with FLYING
/// (`flyingFlag`) added while it follows an unfinished Flying spline.
inline constexpr uint32_t createBlockUnitMoveFlags(uint32_t moveFlags, uint32_t splineFlags,
                                                   uint32_t splineNodes, uint32_t splineDurationMs,
                                                   uint32_t flyingFlag) {
    const bool following = splineNodes >= 2 && splineDurationMs != 0 &&
                           (splineFlags & kTbcSplineDone) == 0;
    return following && (splineFlags & kTbcSplineFlying) ? moveFlags | flyingFlag : moveFlags;
}

} // namespace wowee::game
