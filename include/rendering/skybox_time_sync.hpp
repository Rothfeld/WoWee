#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace wowee::rendering {

/// LightSkybox.dbc column 2 flag bit: Sequence 0 (`gs == -1`) animation timeline
/// tracks the 24-hour game clock (`0.0 .. 1.0` -> `0 .. sequence[0].duration`),
/// while global sequences (`gs >= 0`) continue advancing in real time.
/// In TBC 2.4.3 `LightSkybox.dbc`, row 12 (`Environments\Stars\NagrandSkyBox.mdx`)
/// sets `flags = 1`, keying dawn/day/dusk/night sky layers (`Trans[2..13]`,
/// `Color[0..3, 27]`) across `0 .. 320000 ms` (`13333.33 ms` per game hour).
inline constexpr uint32_t kLightSkyboxFlagTimeOfDaySync = 0x1u;

/// Map normalized 24-hour day fraction (`0.0` = 00:00, `0.5` = 12:00, `20.15/24` = 20:09)
/// onto a time-of-day synced skybox M2 sequence duration in milliseconds.
/// Clamped strictly below `durationMs` so sequence-end wrap handlers never fire.
inline float computeSkyboxTimeOfDayAnimMs(float dayFraction, float durationMs) noexcept {
    if (durationMs <= 0.0f || !std::isfinite(dayFraction) || !std::isfinite(durationMs)) {
        return 0.0f;
    }
    float wrapped = std::fmod(dayFraction, 1.0f);
    if (wrapped < 0.0f) {
        wrapped += 1.0f;
    }
    const float upper = std::nextafter(durationMs, 0.0f);
    return std::clamp(wrapped * durationMs, 0.0f, std::max(0.0f, upper));
}

}  // namespace wowee::rendering
