#pragma once

#include "rendering/wmo_fog.hpp"

namespace wowee::rendering {

/// Clamp dry WMO `MFOG` `end` to the active `viewDistance` (`farclip`) while
/// preserving the authored `start / end` ratio (`startFactor`), matching TBC
/// 2.4.3 indoor WMO fog distance scaling (e.g. `Blackrock_lower_guild.wmo`
/// `MFOG[0]` `endDist = 1111.11 yd` at `farclip = 477 yd` -> `end = 477 yd`,
/// `start = 0 yd`, `color = (255, 37, 8)`). Underwater `MFOG` (`endDist2`)
/// stays independent of `farclip`.
inline void clampWmoFogToViewDistance(WMOFogSample& fog, float viewDistance, bool underwater) {
    if (!underwater && viewDistance > 0.0f && fog.end > viewDistance) {
        const float scalar = (fog.end > 1e-4f) ? (fog.start / fog.end) : 0.0f;
        fog.end = viewDistance;
        fog.start = fog.end * scalar;
    }
}

} // namespace wowee::rendering
