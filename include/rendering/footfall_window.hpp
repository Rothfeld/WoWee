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

} // namespace rendering
} // namespace wowee
