// TBC 2.4.3 GetZonePVPInfo() resolution across Normal vs PvP realms,
// AreaTable.dbc flags/teams, subzone overrides, and live UNIT_FIELD_FACTIONTEMPLATE
// (including GM mode faction template 35).

#include <catch_amalgamated.hpp>

#include "game/zone_pvp.hpp"

using wowee::game::ZonePvpInput;
using wowee::game::ZonePvpResult;
using wowee::game::isPvpRealmIcon;
using wowee::game::kAreaFlagArena;
using wowee::game::kAreaFlagCapital;
using wowee::game::kAreaFlagDuel;
using wowee::game::kAreaFlagOutland;
using wowee::game::kAreaFlagSanctuary;
using wowee::game::kAreaTeamAlliance;
using wowee::game::kAreaTeamAny;
using wowee::game::kAreaTeamContested;
using wowee::game::kAreaTeamHorde;
using wowee::game::resolveZonePvpInfo;

namespace {

constexpr uint32_t kAllyFriend  = 0x02u;
constexpr uint32_t kAllyHostile = 0x0cu;
constexpr uint32_t kHordeFriend = 0x04u;
constexpr uint32_t kHordeHostile = 0x0au;
// FactionTemplate.dbc id=35 (GM mode ON): ourMask=0x00, friendMask=0x01, hostileMask=0x00
constexpr uint32_t kGmFriend  = 0x01u;
constexpr uint32_t kGmHostile = 0x00u;

} // namespace

TEST_CASE("Realm icon maps Normal/RP vs PvP/RP-PvP", "[zone][pvp]") {
    CHECK_FALSE(isPvpRealmIcon(0)); // Normal
    CHECK(isPvpRealmIcon(1));       // PvP
    CHECK_FALSE(isPvpRealmIcon(4)); // Normal2
    CHECK_FALSE(isPvpRealmIcon(6)); // RP
    CHECK(isPvpRealmIcon(8));       // RP-PvP
}

TEST_CASE("Capital cities and friendly/hostile starter zones across PvP and Normal realms", "[zone][pvp]") {
    // Stormwind City: flags=0x138 (capital), team=2 (Alliance)
    for (bool pvpRealm : {true, false}) {
        ZonePvpInput allyInStormwind{
            .zoneFlags = 0x00000138u, .zoneTeam = kAreaTeamAlliance,
            .playerFriendMask = kAllyFriend, .playerHostileMask = kAllyHostile,
            .isPvpRealm = pvpRealm,
        };
        const auto rAlly = resolveZonePvpInfo(allyInStormwind);
        CHECK(rAlly.pvpType == "friendly");
        CHECK_FALSE(rAlly.isSubZonePvP);
        CHECK(rAlly.factionName == "Alliance");

        ZonePvpInput hordeInStormwind = allyInStormwind;
        hordeInStormwind.playerFriendMask = kHordeFriend;
        hordeInStormwind.playerHostileMask = kHordeHostile;
        const auto rHorde = resolveZonePvpInfo(hordeInStormwind);
        CHECK(rHorde.pvpType == "hostile");
        CHECK(rHorde.factionName == "Alliance");
    }

    // Orgrimmar: flags=0x138 (capital), team=4 (Horde)
    for (bool pvpRealm : {true, false}) {
        ZonePvpInput allyInOrg{
            .zoneFlags = 0x00000138u, .zoneTeam = kAreaTeamHorde,
            .playerFriendMask = kAllyFriend, .playerHostileMask = kAllyHostile,
            .isPvpRealm = pvpRealm,
        };
        const auto rAlly = resolveZonePvpInfo(allyInOrg);
        CHECK(rAlly.pvpType == "hostile");
        CHECK(rAlly.factionName == "Horde");

        ZonePvpInput hordeInOrg = allyInOrg;
        hordeInOrg.playerFriendMask = kHordeFriend;
        hordeInOrg.playerHostileMask = kHordeHostile;
        const auto rHorde = resolveZonePvpInfo(hordeInOrg);
        CHECK(rHorde.pvpType == "friendly");
        CHECK(rHorde.factionName == "Horde");
    }

    // Westfall (40) / Elwynn Forest (12): flags=0x40 (duel), team=2 (Alliance)
    for (bool pvpRealm : {true, false}) {
        ZonePvpInput allyInWestfall{
            .zoneFlags = kAreaFlagDuel, .zoneTeam = kAreaTeamAlliance,
            .areaFlags = kAreaFlagDuel, .hasSubZone = true,
            .playerFriendMask = kAllyFriend, .playerHostileMask = kAllyHostile,
            .isPvpRealm = pvpRealm,
        };
        const auto rAlly = resolveZonePvpInfo(allyInWestfall);
        CHECK(rAlly.pvpType == "friendly");
        CHECK(rAlly.factionName == "Alliance");
    }
}

TEST_CASE("GM mode faction template 35 reports contested with zone faction name", "[zone][pvp]") {
    // Verified live on Native TBC 2.4.3 build 8606: .gm on sets
    // UNIT_FIELD_FACTIONTEMPLATE=35 (friendMask=0x01, hostileMask=0x00), so
    // Alliance zones (Westfall, Stormwind City) return ("contested", nil, "Alliance").
    for (uint32_t flags : {kAreaFlagDuel, 0x00000138u}) {
        ZonePvpInput gmInAllyZone{
            .zoneFlags = flags, .zoneTeam = kAreaTeamAlliance,
            .playerFriendMask = kGmFriend, .playerHostileMask = kGmHostile,
            .isPvpRealm = true,
        };
        const auto res = resolveZonePvpInfo(gmInAllyZone);
        CHECK(res.pvpType == "contested");
        CHECK_FALSE(res.isSubZonePvP);
        CHECK(res.factionName == "Alliance");
    }
}

TEST_CASE("Contested zones and dungeons differ between PvP and Normal realms", "[zone][pvp]") {
    // Stranglethorn Vale (33) / Tanaris (440) / Duskwood (10): flags=0x40, team=0
    ZonePvpInput stvPvp{
        .zoneFlags = kAreaFlagDuel, .zoneTeam = kAreaTeamContested,
        .playerFriendMask = kAllyFriend, .playerHostileMask = kAllyHostile,
        .isPvpRealm = true,
    };
    CHECK(resolveZonePvpInfo(stvPvp).pvpType == "contested");
    CHECK(resolveZonePvpInfo(stvPvp).factionName.empty());

    ZonePvpInput stvNormal = stvPvp;
    stvNormal.isPvpRealm = false;
    CHECK(resolveZonePvpInfo(stvNormal).pvpType.empty());

    // Hellfire Peninsula (3483): flags=0x4440, team=0
    ZonePvpInput hfpPvp{
        .zoneFlags = 0x00004440u, .zoneTeam = kAreaTeamContested,
        .playerFriendMask = kAllyFriend, .playerHostileMask = kAllyHostile,
        .isPvpRealm = true,
    };
    CHECK(resolveZonePvpInfo(hfpPvp).pvpType == "contested");

    // Dungeons (e.g. Zul'Farrak 1176): flags=0, team=0 -> nil on both realm types
    ZonePvpInput dungeon{
        .zoneFlags = 0, .zoneTeam = kAreaTeamContested,
        .playerFriendMask = kAllyFriend, .playerHostileMask = kAllyHostile,
        .isPvpRealm = true,
    };
    CHECK(resolveZonePvpInfo(dungeon).pvpType.empty());
}

TEST_CASE("Sanctuaries and subzone arenas override parent zone", "[zone][pvp]") {
    // Shattrath City (3703): flags=0x4d28 (includes sanctuary), team=6
    ZonePvpInput shatt{
        .zoneFlags = 0x00004d28u, .zoneTeam = kAreaTeamAny,
        .playerFriendMask = kAllyFriend, .playerHostileMask = kAllyHostile,
        .isPvpRealm = true,
    };
    CHECK(resolveZonePvpInfo(shatt).pvpType == "sanctuary");

    // The Stair of Destiny (3539, sanctuary) inside Hellfire Peninsula (3483)
    ZonePvpInput stair{
        .zoneFlags = 0x00004440u, .zoneTeam = kAreaTeamContested,
        .areaFlags = 0x00004c00u, .hasSubZone = true,
        .playerFriendMask = kAllyFriend, .playerHostileMask = kAllyHostile,
        .isPvpRealm = true,
    };
    CHECK(resolveZonePvpInfo(stair).pvpType == "sanctuary");

    // Gurubashi Battle Ring (2177, arena 0xd0) inside Stranglethorn Vale (33)
    ZonePvpInput battleRing{
        .zoneFlags = kAreaFlagDuel, .zoneTeam = kAreaTeamContested,
        .areaFlags = 0x000000d0u, .hasSubZone = true,
        .playerFriendMask = kAllyFriend, .playerHostileMask = kAllyHostile,
        .isPvpRealm = false,
    };
    const auto rArena = resolveZonePvpInfo(battleRing);
    CHECK(rArena.pvpType == "arena");
    CHECK(rArena.isSubZonePvP);

    // Nagrand Arena (3698, zone-level arena 0x14080)
    ZonePvpInput nagrandArena{
        .zoneFlags = 0x00014080u, .zoneTeam = kAreaTeamContested,
        .playerFriendMask = kAllyFriend, .playerHostileMask = kAllyHostile,
        .isPvpRealm = true,
    };
    const auto rZoneArena = resolveZonePvpInfo(nagrandArena);
    CHECK(rZoneArena.pvpType == "arena");
    CHECK_FALSE(rZoneArena.isSubZonePvP);
}
