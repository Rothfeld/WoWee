#pragma once

#include <algorithm>
#include <cstdint>

namespace wowee::game {

struct CorpseHarvestRequirements {
    uint32_t skill;
    uint32_t toolCategory;
    const char* tooltipGlobal;
};

constexpr CorpseHarvestRequirements corpseHarvestRequirements(uint32_t creatureTypeFlags) {
    if (creatureTypeFlags & 0x100) return {182, 0, "UNIT_SKINNABLE_HERB"};
    if (creatureTypeFlags & 0x200) return {186, 11, "UNIT_SKINNABLE_ROCK"};
    return {393, 1, "UNIT_SKINNABLE_LEATHER"};
}

constexpr uint32_t corpseHarvestRequiredRank(uint32_t level) {
    return level < 10 ? 0 : level < 20 ? (level - 10) * 10 : level * 5;
}

constexpr bool compatibleGatheringTool(uint32_t itemType, uint32_t itemMask,
                                      uint32_t requiredType, uint32_t requiredMask) {
    return itemType == requiredType && (itemMask & requiredMask) == requiredMask;
}

enum class HarvestDifficulty { Unavailable, Orange, Yellow, Green, Gray };

constexpr HarvestDifficulty corpseHarvestDifficulty(uint32_t level, uint32_t rank,
                                                     bool hasTool) {
    const uint32_t required = corpseHarvestRequiredRank(level);
    if (!rank || rank < required || !hasTool) return HarvestDifficulty::Unavailable;
    if (rank >= required + 100) return HarvestDifficulty::Gray;
    if (rank >= required + 50) return HarvestDifficulty::Green;
    if (rank >= required + 25) return HarvestDifficulty::Yellow;
    return HarvestDifficulty::Orange;
}

} // namespace wowee::game
