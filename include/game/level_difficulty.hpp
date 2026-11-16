#pragma once

#include <cstdint>

namespace wowee::game {

constexpr uint32_t grayLevel(uint32_t playerLevel) {
    if (playerLevel <= 5) return 0;
    if (playerLevel <= 39) return playerLevel - 5 - playerLevel / 10;
    if (playerLevel <= 59) return playerLevel - 1 - playerLevel / 5;
    return playerLevel - 9;
}

enum class LevelDifficulty { Gray, Green, Yellow, Orange, Red };

constexpr LevelDifficulty creatureLevelDifficulty(uint32_t playerLevel, uint32_t level) {
    const int difference = static_cast<int>(level) - static_cast<int>(playerLevel);
    if (difference >= 5) return LevelDifficulty::Red;
    if (difference >= 3) return LevelDifficulty::Orange;
    if (difference >= -2) return LevelDifficulty::Yellow;
    return level > grayLevel(playerLevel) ? LevelDifficulty::Green : LevelDifficulty::Gray;
}

} // namespace wowee::game
