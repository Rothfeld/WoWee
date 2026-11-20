#pragma once

#include <cstdint>

namespace wowee {
namespace rendering {

/// Whether a footfall event authored at `eventMs` into a sequence was passed
/// between two frames whose sequence times were `fromMs` and `toMs`. A looping
/// sequence that wrapped has toMs < fromMs. The first frame of a sequence
/// starts from -1 so an event at 0 ms (Bear.m2 $RR0 in Run) is not lost.
[[nodiscard]] constexpr bool footfallPassed(uint32_t eventMs, float fromMs, float toMs) {
    const auto t = static_cast<float>(eventMs);
    if (toMs >= fromMs) return t > fromMs && t <= toMs;
    return t > fromMs || t <= toMs;
}

/// Footprint decal opacity multiplier by print age (seconds).
/// Measured against original TBC 2.4.3 on Dun Morogh snow: full 0.5x BLP alpha
/// for 3 s, then linear fade to 0 at 6 s.
inline constexpr float kFootprintPeakOpacity = 0.5f;
inline constexpr float kFootprintFadeStart = 3.0f;
inline constexpr float kFootprintLifetime = 6.0f;

[[nodiscard]] constexpr float footprintOpacity(float ageSeconds) {
    if (ageSeconds <= kFootprintFadeStart) return kFootprintPeakOpacity;
    if (ageSeconds >= kFootprintLifetime) return 0.0f;
    const float t = (ageSeconds - kFootprintFadeStart) / (kFootprintLifetime - kFootprintFadeStart);
    return kFootprintPeakOpacity * (1.0f - t);
}

} // namespace rendering
} // namespace wowee
