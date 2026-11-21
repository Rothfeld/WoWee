#include "game/faction_hostility.hpp"
#include "game/game_handler.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"
#include "core/logger.hpp"
#include <stdexcept>
#include <unordered_map>

namespace wowee {
namespace game {

uint32_t playerRaceFactionTemplate(uint8_t playerRace) {
    // Human=1, Orc=2, Dwarf=3, NightElf=4, Undead=5, Tauren=6, Gnome=115, Troll=116, BloodElf=1610, Draenei=1629
    switch (playerRace) {
        case 1: return 1;
        case 2: return 2;
        case 3: return 3;
        case 4: return 4;
        case 5: return 5;
        case 6: return 6;
        case 7: return 115;
        case 8: return 116;
        case 10: return 1610;
        case 11: return 1629;
        default: return 0;
    }
}

void buildFactionHostilityMap(pipeline::AssetManager& assetManager, GameHandler& gameHandler,
                              uint8_t playerRace, uint32_t playerFtId) {
    if (!assetManager.isInitialized()) return;

    auto ftDbc = assetManager.loadDBC("FactionTemplate.dbc");
    auto fDbc = assetManager.loadDBC("Faction.dbc");
    if (!ftDbc || !ftDbc->isLoaded()) return;

    // Race enum -> race mask bit: race 1=0x1, 2=0x2, 3=0x4, 4=0x8, 5=0x10, 6=0x20, 7=0x40, 8=0x80, 10=0x200, 11=0x400
    uint32_t playerRaceMask = 0;
    if (playerRace >= 1 && playerRace <= 8) {
        playerRaceMask = 1u << (playerRace - 1);
    } else if (playerRace == 10) {
        playerRaceMask = 0x200;  // Blood Elf
    } else if (playerRace == 11) {
        playerRaceMask = 0x400;  // Draenei
    }

    if (playerFtId == 0) playerFtId = playerRaceFactionTemplate(playerRace);
    if (playerFtId == 0) {
        LOG_ERROR("No faction template for player race ", static_cast<int>(playerRace));
        return;
    }

    // The factions a player has a standing with, and whether they start at war.
    //
    // For one of these the standing decides, not the groups: the game treats
    // its units as hostile while the player is at war with the faction and as
    // friendly otherwise. A player starts at war when their starting standing
    // is Hostile or worse, unless the faction's flags force peace. This used to
    // call any starting standing below zero hostile - and every Alliance race
    // starts at -1200 with the Kurenai, Unfriendly and peace-forced, so all of
    // Telaar came out hostile and a right-click on its quest givers attacked.
    //
    // At war by default only; GameHandler::isHostileFaction reads the server's
    // own flag once the standings have arrived.
    const auto* facL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("Faction") : nullptr;
    const auto* ftL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("FactionTemplate") : nullptr;
    struct RepFaction { uint32_t repListId = 0; bool atWar = false; };
    std::unordered_map<uint32_t, RepFaction> repFactions;
    if (fDbc && fDbc->isLoaded()) {
        const uint32_t facID = facL ? (*facL)["ID"] : 0;
        const uint32_t facRepList = facL ? (*facL)["ReputationListID"] : 1;
        const uint32_t facRaceMask0 = facL ? (*facL)["ReputationRaceMask0"] : 2;
        const uint32_t facBase0 = facL ? (*facL)["ReputationBase0"] : 10;
        const uint32_t facFlags0 = facL ? (*facL)["ReputationFlags0"] : 14;
        constexpr int32_t kHostileRankTop = -3001;  // Hostile runs -6000 to -3001
        uint32_t atWarCount = 0;
        for (uint32_t i = 0; i < fDbc->getRecordCount(); i++) {
            const uint32_t repListId =
                facRepList != 0xFFFFFFFF ? fDbc->getUInt32(i, facRepList) : 0xFFFFFFFF;
            if (repListId == 0xFFFFFFFF) continue;  // no standing to have
            int32_t baseRep = 0;
            uint32_t repFlags = 0;
            for (int slot = 0; slot < 4; slot++) {
                uint32_t raceMask = fDbc->getUInt32(i, facRaceMask0 + slot);
                if (raceMask & playerRaceMask) {
                    baseRep = fDbc->getInt32(i, facBase0 + slot);
                    if (facFlags0 != 0xFFFFFFFF) repFlags = fDbc->getUInt32(i, facFlags0 + slot);
                    break;
                }
            }
            const bool atWar = (repFlags & GameHandler::FACTION_FLAG_AT_WAR) != 0 ||
                               (baseRep <= kHostileRankTop &&
                                (repFlags & GameHandler::FACTION_FLAG_PEACE_FORCED) == 0);
            repFactions[fDbc->getUInt32(i, facID)] = {repListId, atWar};
            if (atWar) ++atWarCount;
        }
        LOG_INFO("Faction.dbc: ", repFactions.size(), " factions with a standing, ",
                 atWarCount, " at war with race ", static_cast<int>(playerRace), " from the start");
    }

    // Get player faction template data
    const uint32_t ftID = ftL ? (*ftL)["ID"] : 0;
    const uint32_t ftFaction = ftL ? (*ftL)["Faction"] : 1;
    const uint32_t ftFG = ftL ? (*ftL)["FactionGroup"] : 3;
    const uint32_t ftFriend = ftL ? (*ftL)["FriendGroup"] : 4;
    const uint32_t ftEnemy = ftL ? (*ftL)["EnemyGroup"] : 5;
    const uint32_t ftEnemy0 = ftL ? (*ftL)["Enemy0"] : 6;
    const uint32_t ftFriend0 = ftL ? (*ftL)["Friend0"] : 0xFFFFFFFF;
    if (ftFriend0 == 0xFFFFFFFF)
        throw std::runtime_error("dbc_layouts.json lacks FactionTemplate Friend0; deploy current expansion metadata");
    auto templateRow = [&](uint32_t i) {
        FactionTemplateRow row;
        row.id = ftDbc->getUInt32(i, ftID);
        row.faction = ftDbc->getUInt32(i, ftFaction);
        row.factionGroup = ftDbc->getUInt32(i, ftFG);
        row.friendGroup = ftDbc->getUInt32(i, ftFriend);
        row.enemyGroup = ftDbc->getUInt32(i, ftEnemy);
        for (uint32_t f = 0; f < 4; ++f) {
            row.enemies[f] = ftDbc->getUInt32(i, ftEnemy0 + f);
            row.friends[f] = ftDbc->getUInt32(i, ftFriend0 + f);
        }
        return row;
    };
    FactionTemplateRow playerTemplate;
    bool playerTemplateFound = false;
    for (uint32_t i = 0; i < ftDbc->getRecordCount(); i++) {
        if (ftDbc->getUInt32(i, ftID) == playerFtId) {
            playerTemplate = templateRow(i);
            playerTemplateFound = true;
            break;
        }
    }
    if (!playerTemplateFound) {
        LOG_ERROR("Player faction template ", playerFtId, " absent from FactionTemplate.dbc");
        return;
    }

    // Build hostility map for each faction template
    std::unordered_map<uint32_t, bool> factionMap;
    std::unordered_map<uint32_t, uint32_t> groupMasks;
    // And a friendly map beside it. Not the negation of the other one: a
    // faction can be neither, and most wildlife is. A beneficial spell may only
    // be aimed at a unit that is actually friendly - "not hostile" let a neutral
    // boar hold the cast, and the server refused it as an invalid target
    // instead of the spell falling back to the caster.
    std::unordered_map<uint32_t, bool> friendlyMap;
    // Each template whose faction carries a standing, by the server's index for
    // that standing, so the live at-war flag can be asked for later.
    std::unordered_map<uint32_t, uint32_t> templateRepList;
    // What a game object of each template thinks of the player: the same
    // template rule, without the standing override below.
    std::unordered_map<uint32_t, int> objectReaction;
    // Each template's faction, which forced reactions are keyed by.
    std::unordered_map<uint32_t, uint32_t> templateFaction;
    for (uint32_t i = 0; i < ftDbc->getRecordCount(); i++) {
        uint32_t id = ftDbc->getUInt32(i, ftID);
        uint32_t parentFaction = ftDbc->getUInt32(i, ftFaction);
        templateFaction[id] = parentFaction;
        groupMasks[id] = ftDbc->getUInt32(i, ftFG);

        // How a unit of this template regards the player: the template rule
        // of CGUnit_C::UnitReaction (CMaNGOS GetFactionReaction), unit first.
        // Kobold Vermin (25, monster group, no enemy mask) is neutral to a
        // human, as build 8606 colours it; a Defias Thug (17, enemy mask
        // player) is hostile to a human and neutral to a game master (35).
        const int reaction = factionTemplateReaction(templateRow(i), playerTemplate);
        objectReaction[id] = reaction;
        bool hostile = reaction == 2;
        bool friendly = reaction == 5;

        // A faction with a standing overrides all of it: at war is hostile,
        // anything else friendly. See repFactions above.
        if (auto rep = repFactions.find(parentFaction); rep != repFactions.end()) {
            hostile = rep->second.atWar;
            friendly = !hostile;
            templateRepList[id] = rep->second.repListId;
        }

        factionMap[id] = hostile;
        friendlyMap[id] = friendly;
    }

    uint32_t hostileCount = 0;
    for (const auto& [fid, h] : factionMap) { if (h) hostileCount++; }
    uint32_t friendlyCount = 0;
    for (const auto& [fid, f] : friendlyMap) { if (f) friendlyCount++; }
    LOG_INFO("Faction friendliness: ", friendlyCount, "/", friendlyMap.size(),
             " templates friendly; the rest are hostile or neutral");
    gameHandler.setFactionTemplateRepList(std::move(templateRepList));
    gameHandler.setFactionTemplateFactions(std::move(templateFaction));
    gameHandler.setFactionGroupMasks(std::move(groupMasks));
    gameHandler.setFactionFriendlyMap(std::move(friendlyMap));
    gameHandler.setFactionHostileMap(std::move(factionMap));
    gameHandler.setFactionObjectReactionMap(std::move(objectReaction));
    gameHandler.setAppliedPlayerFactionTemplate(
        playerFtId, playerTemplate.friendGroup, playerTemplate.enemyGroup);
    LOG_INFO("Faction hostility for race ", static_cast<int>(playerRace), " (FT ", playerFtId, "): ",
        hostileCount, "/", ftDbc->getRecordCount(), " hostile");
}

} // namespace game
} // namespace wowee
