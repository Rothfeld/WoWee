#pragma once

#include <cmath>
#include <cstdint>

namespace wowee {
namespace ui {

/// TBC 2.4.3 LowHealthFrame threshold (UIParent.lua: PlayerFrameHealthBar <= 20%).
inline constexpr float kLowHealthVignetteThreshold = 0.20f;

/// True when a health drop should trigger the red screen-edge damage flash.
/// Native TBC 2.4.3 shows zero red screen-edge vignette at full/normal HP;
/// screen-edge red warnings only appear at critically low health (<= 20% HP).
inline bool shouldTriggerDamageFlash(bool damageFlashEnabled, bool deadOrGhost,
                                     uint32_t lastHp, uint32_t currentHp,
                                     uint32_t maxHp) {
    if (!damageFlashEnabled || deadOrGhost) return false;
    if (lastHp == 0 || currentHp == 0 || maxHp == 0) return false;
    if (currentHp >= lastHp) return false;
    const float hpPct = static_cast<float>(currentHp) / static_cast<float>(maxHp);
    return hpPct <= kLowHealthVignetteThreshold;
}

/// Pulsing red screen-edge vignette alpha [0..90] when alive and below 20% HP.
/// Returns 0 at full/normal HP (>= 20%) or while dead/ghost.
inline int computeLowHealthVignetteAlpha(bool enabled, bool deadOrGhost,
                                         uint32_t currentHp, uint32_t maxHp,
                                         double timeSec) {
    if (!enabled || deadOrGhost || currentHp == 0 || maxHp == 0) return 0;
    const float hpPct = static_cast<float>(currentHp) / static_cast<float>(maxHp);
    if (hpPct >= kLowHealthVignetteThreshold) return 0;
    const float danger = (kLowHealthVignetteThreshold - hpPct) / kLowHealthVignetteThreshold;
    const float pulse = 0.55f + 0.45f * std::sin(static_cast<float>(timeSec) * 9.4f);
    return static_cast<int>(danger * pulse * 90.0f);
}

} // namespace ui
} // namespace wowee
