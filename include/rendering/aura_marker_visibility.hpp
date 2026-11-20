#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wowee::rendering {

inline bool isConcealmentDetectionMarker(std::string_view path) {
    const auto separator = path.find_last_of("/\\");
    std::string name(path.substr(separator == std::string_view::npos ? 0 : separator + 1));
    std::transform(name.begin(), name.end(), name.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return name == "detectstealth_state_head.m2" || name == "detectstealth_state_head.mdx";
}

inline bool auraConcealsObserver(std::span<const uint32_t> effects) {
    return std::any_of(effects.begin(), effects.end(),
        [](uint32_t effect) { return effect == 16u || effect == 18u; });
}

// SPELL_AURA_MOD_SHAPESHIFT (36) and SPELL_AURA_TRANSFORM (56) replay their
// SpellVisual ImpactKit when the aura ends and the unit leaves the form
// (TBC 2.4.3 build 8606: Ghost Wolf 2645 -> Purge_Impact_Chest, Bear/Cat Form
// 5487/768 -> DruidMorph_Impact_Base, Polymorph 118 -> Polymorph_Impact_Base;
// .demorph without an aura plays nothing).
inline bool auraIsTransformOrShapeshift(std::span<const uint32_t> effects) {
    return std::any_of(effects.begin(), effects.end(),
        [](uint32_t effect) { return effect == 36u || effect == 56u; });
}

inline std::vector<uint32_t> removedTransformAuraVisuals(
    std::span<const uint32_t> previous,
    std::span<const uint32_t> current) {
    const bool addedNew = std::any_of(current.begin(), current.end(), [&](uint32_t visualId) {
        return std::find(previous.begin(), previous.end(), visualId) == previous.end();
    });
    if (addedNew) return {};
    std::vector<uint32_t> removed;
    for (uint32_t visualId : previous) {
        if (visualId != 0 && std::find(current.begin(), current.end(), visualId) == current.end())
            removed.push_back(visualId);
    }
    return removed;
}

} // namespace wowee::rendering
