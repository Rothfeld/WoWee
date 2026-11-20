// Death and resurrection through GameHandler's own dispatch, TBC as CMaNGOS
// writes it. See game_replay.hpp.
#include <catch_amalgamated.hpp>

#include "game_replay.hpp"
#include "rendering/animation/animation_ids.hpp"

#include <algorithm>
#include <string>
#include <vector>

using replay::Session;
using replay::UpdateObject;
using wowee::game::ObjectType;
using wowee::game::UF;

namespace {

struct Events {
    std::vector<std::string> names;
    explicit Events(Session& s) {
        s.handler().setAddonEventCallback(
            [this](const std::string& e, const std::vector<std::string>&) { names.push_back(e); });
    }
    [[nodiscard]] bool has(const std::string& e) const {
        return std::find(names.begin(), names.end(), e) != names.end();
    }
};

void spawnAlivePlayer(Session& s) {
    s.receive(UpdateObject().tbc().create(Session::kPlayerGuid, ObjectType::PLAYER,
                                          {{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}},
                                          true));
}

// Player::KillPlayer: health to zero and, outside an instance, the
// RELEASE_TIMER byte flag, in one values update.
void kill(Session& s, uint32_t playerFieldBytes) {
    s.receive(UpdateObject().tbc().values(Session::kPlayerGuid,
                                          {{UF::UNIT_FIELD_HEALTH, 0},
                                           {UF::PLAYER_FIELD_BYTES, playerFieldBytes}}));
}

// Player::SendInitWorldStates, TBC: map, zone, area, count, pairs.
wowee::network::Packet initWorldStates(uint32_t map, uint32_t zone, uint32_t area) {
    wowee::network::Packet p(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_INIT_WORLD_STATES));
    p.writeUInt32(map);
    p.writeUInt32(zone);
    p.writeUInt32(area);
    p.writeUInt16(1);
    p.writeUInt32(2264);
    p.writeUInt32(0);
    return p;
}

}  // namespace

TEST_CASE("an open-world death counts down six minutes to release", "[replay][death]") {
    Session s("tbc");
    spawnAlivePlayer(s);
    CHECK(s.handler().getReleaseTimeRemainingSec() == 0);
    Events events(s);
    kill(s, 0x08);
    REQUIRE(events.has("PLAYER_DEAD"));
    const double remaining = s.handler().getReleaseTimeRemainingSec();
    CHECK(remaining > 359.0);
    CHECK(remaining <= 360.0);
}

TEST_CASE("an instance death has no release timer", "[replay][death]") {
    Session s("tbc");
    spawnAlivePlayer(s);
    kill(s, 0x00);
    CHECK(s.handler().getReleaseTimeRemainingSec() == -1);
}

TEST_CASE("world states resent on resurrection raise no zone event", "[replay][death][zone]") {
    Session s("tbc");
    spawnAlivePlayer(s);
    Events events(s);
    // Ashenvale / Astranaar area 415: the 13:47 pair's .revive, where
    // ResurrectPlayer -> UpdateZone(force) resent this packet.
    s.receive(initWorldStates(1, 331, 415));
    s.receive(initWorldStates(1, 148, 148));
    CHECK_FALSE(events.has("ZONE_CHANGED_NEW_AREA"));
    CHECK_FALSE(events.has("ZONE_CHANGED"));
    CHECK(s.handler().getWorldStates().at(2264) == 0);
}

TEST_CASE("a map change raises no zone event before the new map is loaded", "[replay][zone]") {
    // Ledger #100. SMSG_NEW_WORLD (CMaNGOS Player::TeleportTo far branch):
    // map, position, orientation. Razorfen Kraul, map 47.
    Session s("tbc");
    spawnAlivePlayer(s);
    Events events(s);
    wowee::network::Packet p(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_NEW_WORLD));
    p.writeUInt32(47);
    p.writeFloat(1943.0f);
    p.writeFloat(1544.0f);
    p.writeFloat(82.0f);
    p.writeFloat(0.0f);
    s.receive(p);
    CHECK(events.has("PLAYER_ENTERING_WORLD"));
    // GetZoneText still names the old zone here; the renderer announces the
    // new one while the load resolves it.
    CHECK_FALSE(events.has("ZONE_CHANGED_NEW_AREA"));
}

TEST_CASE("an exploration discovery prints its experience in chat and on screen", "[replay][explore]") {
    // CMaNGOS Player::CheckAreaExploreAndOutdoor: SMSG_EXPLORATION_EXPERIENCE
    // area id, experience. Razorfen Kraul is area 491.
    Session s("tbc");
    wowee::game::GameHandlerTestAccess::setAreaName(s.handler(), 491, "Razorfen Kraul");
    std::vector<std::string> info;
    s.handler().setAddonEventCallback([&](const std::string& e, const std::vector<std::string>& a) {
        if (e == "UI_INFO_MESSAGE" && !a.empty()) info.push_back(a.front());
    });
    const auto discover = [&](uint32_t xp) {
        wowee::network::Packet p(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_EXPLORATION_EXPERIENCE));
        p.writeUInt32(491);
        p.writeUInt32(xp);
        s.receive(p);
    };
    discover(87);
    // ERR_ZONE_EXPLORED_XP, as the native chat line reads.
    REQUIRE(info.size() == 1);
    CHECK(info.back() == "Discovered Razorfen Kraul: 87 experience gained");
    const auto& chat = s.handler().getChatHistory();
    REQUIRE_FALSE(chat.empty());
    CHECK(chat.back().type == wowee::game::ChatType::COMBAT_XP_GAIN);
    CHECK(chat.back().message == "Discovered Razorfen Kraul: 87 experience gained");

    const auto lines = chat.size();
    discover(0);
    CHECK(info.back() == "Discovered: Razorfen Kraul");
    CHECK(s.handler().getChatHistory().size() == lines);
}

TEST_CASE("a corpse keeps its death pose through server emotes", "[replay][death][emote]") {
    // Vashj'ir Honor Guard (21218): its relay scripts talk on after it died,
    // and DoDisplayText plays each text's emote through Unit::HandleEmote -
    // SMSG_EMOTE (ONESHOT_TALK 1, ONESHOT_ROAR 15) or UNIT_NPC_EMOTESTATE.
    // Native 2.4.3 left the corpse lying through .npc playemote 15, 1, 0 and
    // 64 (_experiments/20261118T1320Z.death_over_emote in the workspace).
    Session s("tbc");
    spawnAlivePlayer(s);
    constexpr uint64_t kHonorGuard = 0xF1300052E2000001ull;
    s.receive(UpdateObject().tbc().create(kHonorGuard, ObjectType::UNIT,
                                          {{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}}));
    struct Played { uint64_t guid; uint32_t animation; bool state; uint32_t emote; };
    std::vector<Played> played;
    s.handler().setEmoteAnimCallback([&](uint64_t guid, uint32_t animation, bool state, uint32_t emote) {
        played.push_back({guid, animation, state, emote});
    });
    // Unit::HandleEmoteCommand: emote id, then the source's full guid.
    const auto emote = [&](uint32_t id) {
        wowee::network::Packet p(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_EMOTE));
        p.writeUInt32(id);
        p.writeUInt64(kHonorGuard);
        s.receive(p);
    };
    const auto emoteState = [&](uint32_t id) {
        s.receive(UpdateObject().tbc().values(kHonorGuard, {{UF::UNIT_NPC_EMOTESTATE, id}}));
    };

    // STATE_DANCE (10): the test has no Emotes.dbc, and the built-in table
    // has no row for 64.
    emote(1);
    emoteState(10);
    REQUIRE(played.size() == 2);
    CHECK(played[0].animation == wowee::rendering::anim::EMOTE_TALK);
    CHECK_FALSE(played[0].state);
    CHECK(played[1].state);
    CHECK(played[1].emote == 10);

    // Death in the middle of the state, as Unit::SetDeathState leaves it: the
    // emote state field is not touched.
    s.receive(UpdateObject().tbc().values(kHonorGuard, {{UF::UNIT_FIELD_HEALTH, 0}}));
    emote(15);
    emote(1);
    emoteState(0);
    emoteState(10);
    CHECK(played.size() == 2);
    // The state is still the unit's; only its animation waits.
    const auto unit = std::dynamic_pointer_cast<wowee::game::Unit>(
        s.handler().getEntityManager().getEntity(kHonorGuard));
    REQUIRE(unit);
    CHECK(unit->getNpcEmoteState() == 10);

    // Respawned, it emotes again.
    s.receive(UpdateObject().tbc().values(kHonorGuard, {{UF::UNIT_FIELD_HEALTH, 100}}));
    emote(15);
    REQUIRE(played.size() == 3);
    CHECK(played[2].animation == wowee::rendering::anim::EMOTE_ROAR);
}

#include "addons/lua_api_helpers.hpp"

TEST_CASE("the interface's first build sees no target, focus or mouseover", "[replay][interface]") {
    // Ledger #107: an attacker auto-targeted during the world load made
    // TargetFrame's OnLoad reach TargetofTargetFrame before the XML created
    // it; the original builds the interface before any target exists.
    Session s("tbc");
    spawnAlivePlayer(s);
    constexpr uint64_t kQuilguard = 0xF130001154596BB4ull;
    s.receive(UpdateObject().tbc().create(kQuilguard, ObjectType::UNIT,
                                          {{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}}));
    s.handler().setTarget(kQuilguard);
    s.handler().setFocus(kQuilguard);
    auto& gh = s.handler();
    gh.setInterfaceLoading(true);
    CHECK(wowee::addons::resolveUnitGuid(&gh, "target") == 0);
    CHECK(wowee::addons::resolveUnitGuid(&gh, "targettarget") == 0);
    CHECK(wowee::addons::resolveUnitGuid(&gh, "focus") == 0);
    CHECK(wowee::addons::resolveUnitGuid(&gh, "mouseover") == 0);
    CHECK(wowee::addons::resolveUnitGuid(&gh, "player") == Session::kPlayerGuid);
    gh.setInterfaceLoading(false);
    CHECK(wowee::addons::resolveUnitGuid(&gh, "target") == kQuilguard);
    CHECK(wowee::addons::resolveUnitGuid(&gh, "focus") == kQuilguard);
}

TEST_CASE("dying with PLAYER_SELF_RES_SPELL populates HasSoulstone before PLAYER_DEAD and sends CMSG_SELF_RES",
          "[replay][death][soulstone]") {
    Session s("tbc");
    auto& gh = s.handler();
    gh.spellNameCacheRef()[3026].name = "Use Soulstone";
    gh.spellNameCacheRef()[21169].name = "Reincarnation";
    gh.spellNameCacheRef()[23700].name = "Twisting Nether";
    gh.spellNameCacheLoadedRef() = true;

    spawnAlivePlayer(s);
    CHECK_FALSE(gh.canSelfRes());
    CHECK(gh.getSelfResSpellId() == 0);
    CHECK(gh.getSelfResOptionText().empty());

    // CMaNGOS TBC Player::SetDeathState(JUST_DIED) sets UNIT_FIELD_HEALTH=0
    // (index 22) and PLAYER_SELF_RES_SPELL (index 1488) in one update block.
    // PLAYER_DEAD must fire only after PLAYER_SELF_RES_SPELL is applied so
    // StaticPopupDialogs["DEATH"].OnShow sees the self-res button text.
    std::string textAtPlayerDead;
    bool canSelfResAtPlayerDead = false;
    uint32_t spellIdAtPlayerDead = 0;
    int playerDeadCount = 0;
    gh.setAddonEventCallback([&](const std::string& e, const std::vector<std::string>&) {
        if (e == "PLAYER_DEAD") {
            ++playerDeadCount;
            canSelfResAtPlayerDead = gh.canSelfRes();
            spellIdAtPlayerDead = gh.getSelfResSpellId();
            textAtPlayerDead = gh.getSelfResOptionText();
        }
    });

    s.receive(UpdateObject().tbc().values(Session::kPlayerGuid,
                                          {{UF::UNIT_FIELD_HEALTH, 0},
                                           {UF::PLAYER_FIELD_BYTES, 0x08},
                                           {UF::PLAYER_SELF_RES_SPELL, 3026}}));
    REQUIRE(playerDeadCount == 1);
    CHECK(canSelfResAtPlayerDead);
    CHECK(spellIdAtPlayerDead == 3026);
    CHECK(textAtPlayerDead == "Use Soulstone");

    // Clicking Button2 calls UseSoulstone() -> CMSG_SELF_RES (0x2B3, empty payload).
    gh.useSelfRes();
    const auto selfResPackets = s.sent().of(wowee::game::Opcode::CMSG_SELF_RES);
    REQUIRE(selfResPackets.size() == 1);
    CHECK(selfResPackets.front().getOpcode() == 0x2B3);
    CHECK(selfResPackets.front().getSize() == 0);
    CHECK_FALSE(gh.canSelfRes());
    CHECK(gh.getSelfResSpellId() == 0);
    CHECK(gh.getSelfResOptionText().empty());

    // Resurrect, then die with Shaman Reincarnation (21169).
    s.receive(UpdateObject().tbc().values(Session::kPlayerGuid,
                                          {{UF::UNIT_FIELD_HEALTH, 100},
                                           {UF::PLAYER_SELF_RES_SPELL, 0}}));
    s.receive(UpdateObject().tbc().values(Session::kPlayerGuid,
                                          {{UF::UNIT_FIELD_HEALTH, 0},
                                           {UF::PLAYER_FIELD_BYTES, 0x08},
                                           {UF::PLAYER_SELF_RES_SPELL, 21169}}));
    REQUIRE(playerDeadCount == 2);
    CHECK(canSelfResAtPlayerDead);
    CHECK(spellIdAtPlayerDead == 21169);
    CHECK(textAtPlayerDead == "Reincarnation");

    // Twisting Nether (23700) updates the option text; clearing to 0 removes it.
    s.receive(UpdateObject().tbc().values(Session::kPlayerGuid,
                                          {{UF::PLAYER_SELF_RES_SPELL, 23700}}));
    CHECK(gh.getSelfResOptionText() == "Twisting Nether");

    s.receive(UpdateObject().tbc().values(Session::kPlayerGuid,
                                          {{UF::PLAYER_SELF_RES_SPELL, 0}}));
    CHECK_FALSE(gh.canSelfRes());
    CHECK(gh.getSelfResOptionText().empty());
}
