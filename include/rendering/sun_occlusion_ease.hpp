#pragma once

/// The sun's occlusion as the sky pass uses it: eased towards a yes-or-no
/// sample (Renderer::sampleSunOcclusion) rather than taken, so the flare does
/// not snap when a hill edge crosses the line to the sun.
///
/// The sample is taken on drawn ticks only (docs/draw-schedule.md), and the
/// ease then covers all the time since the last sample. Because the ease is
/// an exponential, n steps towards one target are the same as one step over
/// their summed time: an undrawn tick changes nothing unless the sample
/// would have changed during it.

#include <algorithm>
#include <cmath>

namespace wowee::rendering {

/// Seconds for the eased value to cover 63% of the way to the sample.
inline constexpr float kSunOcclusionEaseSeconds = 0.25f;

[[nodiscard]] inline float easeSunOcclusion(float current, float sample, float seconds) {
    const float rate = 1.0f - std::exp(-seconds / kSunOcclusionEaseSeconds);
    return current + (sample - current) * std::clamp(rate, 0.0f, 1.0f);
}

} // namespace wowee::rendering
