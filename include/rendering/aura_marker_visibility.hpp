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

// SPELL_AURA_MOD_SHAPESHIFT (36) and SPELL_AURA_TRANSFORM (56) play their
// SpellVisual ImpactKit on aura transition: when applied without a preceding
// SMSG_SPELL_GO (e.g. server AddAura / .aura 8041) and when removed as the
// unit leaves the form alive or on death (TBC 2.4.3 build 8606: Serpent Form
// 8041 / Bear/Cat Form 5487/768 -> DruidMorph_Impact_Base, Ghost Wolf 2645 ->
// Purge_Impact_Chest, Polymorph 118 -> Polymorph_Impact_Base; .demorph /
// .npc setmodel without an aura plays nothing).
inline bool auraIsTransformOrShapeshift(std::span<const uint32_t> effects) {
    return std::any_of(effects.begin(), effects.end(),
        [](uint32_t effect) { return effect == 36u || effect == 56u; });
}

inline std::vector<uint32_t> addedTransformAuraVisuals(
    std::span<const uint32_t> previous,
    std::span<const uint32_t> current) {
    std::vector<uint32_t> added;
    for (uint32_t visualId : current) {
        if (visualId != 0 && std::find(previous.begin(), previous.end(), visualId) == previous.end())
            added.push_back(visualId);
    }
    return added;
}

inline std::vector<uint32_t> removedTransformAuraVisuals(
    std::span<const uint32_t> previous,
    std::span<const uint32_t> current) {
    const bool addedNew = !addedTransformAuraVisuals(previous, current).empty();
    if (addedNew) return {};
    std::vector<uint32_t> removed;
    for (uint32_t visualId : previous) {
        if (visualId != 0 && std::find(current.begin(), current.end(), visualId) == current.end())
            removed.push_back(visualId);
    }
    return removed;
}

} // namespace wowee::rendering
