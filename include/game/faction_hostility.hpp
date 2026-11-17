#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace game {

class GameHandler;

// Builds the per-FactionTemplate hostility map from FactionTemplate.dbc/
// Faction.dbc - each template's reaction to the player's template
// (factionTemplateReaction) and Faction.dbc starting standings - and
// installs it via GameHandler::setFactionHostileMap().
//
// GameHandler::isHostileFaction() defaults an unrecognized faction template
// ID to hostile=true (a reasonable default once the map is actually
// populated, since anything left out by the DBC-driven loop below is
// genuinely unusual). Call this once per session, as soon as playerRace is
// known and the asset manager is initialized, or every faction template
// will fall through to that hostile default.
/// One FactionTemplate.dbc row, as the reaction rule reads it.
struct FactionTemplateRow {
    uint32_t id = 0;
    uint32_t faction = 0;
    uint32_t factionGroup = 0;
    uint32_t friendGroup = 0;
    uint32_t enemyGroup = 0;
    std::array<uint32_t, 4> enemies{};
    std::array<uint32_t, 4> friends{};
};

/// How a holder of template `self` regards a holder of `other`, numbered as
/// UnitReaction numbers it: 2 hostile, 4 neutral, 5 friendly. The
/// template-to-template half of CGUnit_C::UnitReaction in the client's order -
/// hostile tests first, then friendly, else neutral - which CMaNGOS mirrors in
/// Relations.cpp GetFactionReaction(FactionTemplateEntry*, FactionTemplateEntry*).
/// One direction only: a game object's reaction to the player is this with
/// the object's template as `self`.
inline int factionTemplateReaction(const FactionTemplateRow& self, const FactionTemplateRow& other) {
    constexpr int kHostile = 2, kNeutral = 4, kFriendly = 5;
    auto lists = [](const std::array<uint32_t, 4>& ids, uint32_t wanted) {
        return ids[0] != 0 && wanted != 0 && std::find(ids.begin(), ids.end(), wanted) != ids.end();
    };
    if (other.factionGroup & self.enemyGroup) return kHostile;
    if (lists(self.enemies, other.faction)) return kHostile;
    if (other.factionGroup & self.friendGroup) return kFriendly;
    if (lists(self.friends, other.faction)) return kFriendly;
    if (self.factionGroup & other.friendGroup) return kFriendly;
    if (lists(other.friends, self.faction)) return kFriendly;
    return kNeutral;
}

/// The faction template a character of this race starts with; 0 for an
/// unknown race.
uint32_t playerRaceFactionTemplate(uint8_t playerRace);

/// Builds the maps for the player's faction template `playerFtId`, or the
/// race's starting template when it is 0. Reputation starts follow the race.
void buildFactionHostilityMap(pipeline::AssetManager& assetManager, GameHandler& gameHandler,
                              uint8_t playerRace, uint32_t playerFtId = 0);

} // namespace game
} // namespace wowee
