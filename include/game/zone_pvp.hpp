#pragma once

#include <cstdint>
#include <string>

namespace wowee::game {

// AreaTable.dbc field 4 (Flags) bits relevant to GetZonePVPInfo().
constexpr uint32_t kAreaFlagDuel      = 0x00000040u;
constexpr uint32_t kAreaFlagArena     = 0x00000080u;
constexpr uint32_t kAreaFlagCapital   = 0x00000100u;
constexpr uint32_t kAreaFlagOutland   = 0x00000400u;
constexpr uint32_t kAreaFlagSanctuary = 0x00000800u;

// AreaTable.dbc field 28 (FactionGroupMask / team).
constexpr uint32_t kAreaTeamContested = 0u;
constexpr uint32_t kAreaTeamAlliance  = 2u;
constexpr uint32_t kAreaTeamHorde     = 4u;
constexpr uint32_t kAreaTeamAny       = 6u;

// Auth realm list icon values (realmlist.icon):
//   0 = Normal, 1 = PvP, 4 = Normal, 6 = RP, 8 = RP-PvP.
inline bool isPvpRealmIcon(uint8_t icon) {
    return icon == 1u || icon == 8u;
}

struct ZonePvpInput {
    uint32_t zoneFlags = 0;
    uint32_t zoneTeam = 0;
    uint32_t areaFlags = 0;
    bool     hasSubZone = false;
    // From FactionTemplate.dbc for the player's live UNIT_FIELD_FACTIONTEMPLATE:
    //   Alliance races (1,3,4,115,1629): friend=0x02, hostile=0x0c
    //   Horde races    (2,5,6,116,1610): friend=0x04, hostile=0x0a
    //   GM mode ON     (35):             friend=0x01, hostile=0x00
    uint32_t playerFriendMask = 0;
    uint32_t playerHostileMask = 0;
    bool     isPvpRealm = true;
};

struct ZonePvpResult {
    std::string pvpType;      // "sanctuary", "arena", "friendly", "hostile", "contested", or "" (nil)
    bool        isSubZonePvP = false;
    std::string factionName;  // "Alliance", "Horde", or ""
};

/// Resolve TBC 2.4.3 GetZonePVPInfo() return values from AreaTable.dbc flags/team,
/// the player's live FactionTemplate masks, and realm PvP type.
inline ZonePvpResult resolveZonePvpInfo(const ZonePvpInput& in) {
    // Subzone sanctuary / arena override the parent zone (e.g. The Stair of
    // Destiny 3539 inside Hellfire Peninsula 3483, Battle Ring 2177 inside
    // Stranglethorn Vale 33).
    if (in.hasSubZone) {
        if ((in.areaFlags & kAreaFlagSanctuary) != 0) {
            return {"sanctuary", false, ""};
        }
        if ((in.areaFlags & kAreaFlagArena) != 0) {
            const bool subZoneOnly = (in.zoneFlags & kAreaFlagArena) == 0;
            return {"arena", subZoneOnly, ""};
        }
    }

    if ((in.zoneFlags & kAreaFlagSanctuary) != 0) {
        return {"sanctuary", false, ""};
    }
    if ((in.zoneFlags & kAreaFlagArena) != 0) {
        return {"arena", false, ""};
    }

    if (in.zoneTeam == kAreaTeamAlliance || in.zoneTeam == kAreaTeamHorde) {
        const char* factionName = (in.zoneTeam == kAreaTeamAlliance) ? "Alliance" : "Horde";
        if ((in.playerFriendMask & in.zoneTeam) != 0) {
            return {"friendly", false, factionName};
        }
        if ((in.playerHostileMask & in.zoneTeam) != 0) {
            return {"hostile", false, factionName};
        }
        // Neutral / GM faction template (e.g. UNIT_FIELD_FACTIONTEMPLATE=35
        // has friendMask=0x01, hostileMask=0x00): neither friendly nor hostile
        // to Alliance (0x02) or Horde (0x04), which Native TBC 2.4.3 reports
        // as ("contested", nil, factionName).
        return {"contested", false, factionName};
    }

    if (in.zoneTeam == kAreaTeamContested || in.zoneTeam == kAreaTeamAny) {
        // World contested zones carry AREA_FLAG_DUEL or AREA_FLAG_OUTLAND.
        // Dungeons/instances (flags=0) and Normal/RP realms return nil so
        // ZoneText.lua draws the neutral cream title without "(Contested Territory)".
        if (in.isPvpRealm && (in.zoneFlags & (kAreaFlagDuel | kAreaFlagOutland)) != 0) {
            return {"contested", false, ""};
        }
        return {};
    }

    return {};
}

} // namespace wowee::game
