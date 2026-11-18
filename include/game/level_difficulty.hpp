#pragma once

#include <algorithm>
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

/// GetQuestGreenRange(): how many levels below the player a quest stays
/// green.
///
/// Build 8606 answers 4 up to level 9 and one more every ten levels, 8 from
/// level 40 on (native /script at levels 5, 6, 9, 10, 19, 20, 29, 30, 39, 40,
/// 44, 45, 49, 50, 54, 55, 59, 60, 70; _experiments/20261118T181000Z.
/// pair16_quest_cursor/greenrange.png). Other clients keep AzerothCore's
/// Acore::XP::GetGrayLevel distance (grayLevel), unmeasured here.
inline int questGreenRange(bool tbc, int playerLevel) {
    if (tbc) return std::min(8, 4 + playerLevel / 10);
    return playerLevel - static_cast<int>(grayLevel(static_cast<uint32_t>(playerLevel)));
}

/// Whether the gossip and greeting lists call a quest trivial: grey icon and
/// "(low level)" (TRIVIAL_QUEST_DISPLAY).
///
/// Build 8606 does not take this from the green range: it marks a quest more
/// than 5 levels below the player, at every level measured - Thundris
/// Windweaver's level 12 quests plain at 17 and low level at 18, Rigglefuzz's
/// level 40 quest plain at 45 and low level at 46 (green range 8 there),
/// Smokey LaRue's level 58 quest plain at 63 and low level at 64
/// (_experiments/20261118T181000Z.pair16_quest_cursor/gossip-*.png). Below
/// level 17 unmeasured. Other clients keep the grey line of the green range.
inline bool questIsTrivial(bool tbc, int playerLevel, int questLevel) {
    if (questLevel <= 0 || playerLevel <= 0) return false;
    if (tbc) return playerLevel - questLevel > 5;
    return playerLevel - questLevel > questGreenRange(false, playerLevel);
}

/// A quest's colour band, by GetDifficultyColor's rule in 2.4.3's
/// QuestLogFrame.lua: green while the quest is no more than GetQuestGreenRange()
/// levels below the player (questGreenRange), grey below that.
inline LevelDifficulty questLevelDifficulty(bool tbc, uint32_t playerLevel, int32_t questLevel) {
    const int difference = questLevel - static_cast<int>(playerLevel);
    if (difference >= 5) return LevelDifficulty::Red;
    if (difference >= 3) return LevelDifficulty::Orange;
    if (difference >= -2) return LevelDifficulty::Yellow;
    return -difference <= questGreenRange(tbc, static_cast<int>(playerLevel))
        ? LevelDifficulty::Green : LevelDifficulty::Gray;
}

} // namespace wowee::game
