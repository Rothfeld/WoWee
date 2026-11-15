#pragma once

#include <algorithm>
#include <cstdint>

namespace wowee::core {

struct CreatureFamilyScale {
    float minScale;
    uint32_t minLevel;
    float maxScale;
    uint32_t maxLevel;
};

// Build 8606 CGUnit_C model scale (0x60bc70). Pets use family size;
// wild creatures use it as a lower bound on display/model size.
inline float resolveCreatureFamilyScale(float baseScale, uint32_t level,
                                       bool pet, const CreatureFamilyScale* family) {
    if (!family) return baseScale;
    const uint32_t span = family->maxLevel - family->minLevel;
    const uint32_t offset = level > family->minLevel ? level - family->minLevel : 0;
    const float fraction = span ? static_cast<float>(std::min(offset, span)) / span : 0.0f;
    const float familyScale = family->minScale + fraction * (family->maxScale - family->minScale);
    return pet ? familyScale : std::max(baseScale, familyScale);
}

} // namespace wowee::core
