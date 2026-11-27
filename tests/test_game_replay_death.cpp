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

TEST_CASE("an exploration discovery matches TBC 2.4.3 UIErrorsFrame, chat, combat text, and map state", "[replay][explore]") {
    // CMaNGOS Player::CheckAreaExploreAndOutdoor:
    //  - Sub-70 (xp > 0): GiveXP(XP, nullptr) -> SMSG_LOG_XPGAIN (type=1 non-kill),
    //    then SendExplorationExperience(area, XP) -> SMSG_EXPLORATION_EXPERIENCE,
    //    plus SMSG_UPDATE_OBJECT updating PLAYER_EXPLORED_ZONES_1.
    //  - Level 70 (xp == 0): SendExplorationExperience(area, 0) + PLAYER_EXPLORED_ZONES_1.
    // Native TBC 2.4.3 (verified on pair 19):
    //  - UIErrorsFrame (UI_INFO_MESSAGE) ALWAYS shows ERR_ZONE_EXPLORED ("Discovered: %s").
    //  - Chat receives ERR_ZONE_EXPLORED_XP ("Discovered %s: %d experience gained") in
    //    yellow CHAT_MSG_SYSTEM when xp > 0 (while SMSG_LOG_XPGAIN logs "You gain %d
    //    experience." to COMBAT_XP_GAIN and spawns the single XP_GAIN combat text).
    //  - When xp == 0, chat receives nothing and no XP combat text spawns.
    Session s("tbc");
    wowee::game::GameHandlerTestAccess::setAreaName(s.handler(), 491, "Razorfen Kraul");
    wowee::game::GameHandlerTestAccess::setAreaName(s.handler(), 35, "Booty Bay");
    wowee::game::GameHandlerTestAccess::setRaceExplorationSound(s.handler(), 5, 4142); // Undead

    std::vector<std::string> info;
    int worldMapUpdates = 0;
    s.handler().setAddonEventCallback([&](const std::string& e, const std::vector<std::string>& a) {
        if (e == "UI_INFO_MESSAGE" && !a.empty()) info.push_back(a.front());
        if (e == "WORLD_MAP_UPDATE") ++worldMapUpdates;
    });

    // 1. Login CREATE_OBJECT2 with pre-existing explored zones (Sentinel Hill bit 196 = word 6 bit 4)
    //    must populate masks without false "Discovered: ..." or login WORLD_MAP_UPDATE spam.
    const uint16_t exploredStart = wowee::game::fieldIndex(UF::PLAYER_EXPLORED_ZONES_START);
    REQUIRE(exploredStart != 0xFFFF);
    const uint16_t bytes0Idx = wowee::game::fieldIndex(UF::UNIT_FIELD_BYTES_0);
    REQUIRE(bytes0Idx != 0xFFFF);
    s.receive(UpdateObject().tbc().create(
        Session::kPlayerGuid, ObjectType::PLAYER,
        {{UF::UNIT_FIELD_HEALTH, 100},
         {UF::UNIT_FIELD_MAXHEALTH, 100},
         {UF::UNIT_FIELD_BYTES_0, 5u}}, // race = 5 (Undead)
        true));
    s.receive(UpdateObject().tbc().valuesAt(
        Session::kPlayerGuid,
        {{static_cast<uint16_t>(exploredStart + 6), (1u << 4)}}));
    CHECK(s.handler().hasPlayerExploredZoneMasks());
    CHECK((s.handler().getPlayerExploredZoneMasks()[6] & (1u << 4)) != 0);
    CHECK(s.handler().getPlayerRace() == 5);
    CHECK(s.handler().getRaceExplorationSoundId(s.handler().getPlayerRace()) == 4142);
    CHECK(info.empty());
    CHECK(s.handler().getChatHistory().empty());
    CHECK(worldMapUpdates == 0); // first mask receipt initializes baseline without firing

    const auto sendLogXpGainNonKill = [&](uint32_t xp) {
        wowee::network::Packet p(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_LOG_XPGAIN));
        p.writeUInt64(0);   // victim == nullptr
        p.writeUInt32(xp);  // GivenXP
        p.writeUInt8(1);    // type = 1 (non_kill_xp)
        p.writeUInt8(0);    // RAF bonus
        s.receive(p);
    };
    const auto discover = [&](uint32_t areaId, uint32_t xp) {
        wowee::network::Packet p(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_EXPLORATION_EXPERIENCE));
        p.writeUInt32(areaId);
        p.writeUInt32(xp);
        s.receive(p);
    };

    // 2. Sub-70 discovery (xp == 87): UI_INFO_MESSAGE is "Discovered: Razorfen Kraul",
    //    SYSTEM chat is "Discovered Razorfen Kraul: 87 experience gained", COMBAT_XP_GAIN
    //    is "You gain 87 experience.", and floating combat text spawns XP_GAIN once.
    sendLogXpGainNonKill(87);
    discover(491, 87);
    s.receive(UpdateObject().tbc().valuesAt(
        Session::kPlayerGuid,
        {{static_cast<uint16_t>(exploredStart + 16), (1u << 3)}})); // exploreFlag 515

    REQUIRE(info.size() == 1);
    CHECK(info.back() == "Discovered: Razorfen Kraul");
    const auto& chat = s.handler().getChatHistory();
    REQUIRE(chat.size() == 2);
    CHECK(chat[0].type == wowee::game::ChatType::COMBAT_XP_GAIN);
    CHECK(chat[0].message == "You gain 87 experience.");
    CHECK(chat[1].type == wowee::game::ChatType::SYSTEM);
    CHECK(chat[1].message == "Discovered Razorfen Kraul: 87 experience gained");
    int xpFloaters = 0;
    for (const auto& ct : s.handler().getCombatText()) {
        if (ct.type == wowee::game::CombatTextEntry::XP_GAIN) {
            ++xpFloaters;
            CHECK(ct.amount == 87);
        }
    }
    CHECK(xpFloaters == 1);
    CHECK(worldMapUpdates == 1);

    // 3. Level 70 discovery (xp == 0): UI_INFO_MESSAGE is "Discovered: Booty Bay",
    //    zero new chat lines, zero new XP floating combat text, and map mask updates.
    const auto linesBefore = chat.size();
    discover(35, 0);
    s.receive(UpdateObject().tbc().valuesAt(
        Session::kPlayerGuid,
        {{static_cast<uint16_t>(exploredStart + 4), (1u << 14)}})); // exploreFlag 142

    REQUIRE(info.size() == 2);
    CHECK(info.back() == "Discovered: Booty Bay");
    CHECK(s.handler().getChatHistory().size() == linesBefore);
    xpFloaters = 0;
    for (const auto& ct : s.handler().getCombatText()) {
        if (ct.type == wowee::game::CombatTextEntry::XP_GAIN) ++xpFloaters;
    }
    CHECK(xpFloaters == 1);
    CHECK(worldMapUpdates == 2);
}

TEST_CASE("exploration discovery notices hold while interface loads and format GlobalStrings", "[replay][explore]") {
    Session s("tbc");
    wowee::game::GameHandlerTestAccess::setAreaName(s.handler(), 107, "Saldean's Farm");
    s.handler().setGlobalStringLookup([](const char* name) -> std::string {
        const std::string key(name);
        if (key == "ERR_ZONE_EXPLORED") return "Discovered: %s";
        if (key == "ERR_ZONE_EXPLORED_XP") return "Discovered %s: %d experience gained";
        return {};
    });
    std::vector<std::string> info;
    s.handler().setAddonEventCallback([&](const std::string& e, const std::vector<std::string>& a) {
        if (e == "UI_INFO_MESSAGE" && !a.empty()) info.push_back(a.front());
    });

    wowee::game::GameHandlerTestAccess::setNoticesInterfaceReady(s.handler(), false);
    wowee::network::Packet p(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_EXPLORATION_EXPERIENCE));
    p.writeUInt32(107);
    p.writeUInt32(100);
    s.receive(p);

    CHECK(info.empty());
    CHECK(s.handler().getChatHistory().empty());

    s.handler().onInterfaceReady(40);
    REQUIRE(info.size() == 1);
    CHECK(info.back() == "Discovered: Saldean's Farm");
    REQUIRE(s.handler().getChatHistory().size() == 1);
    CHECK(s.handler().getChatHistory().back().type == wowee::game::ChatType::SYSTEM);
    CHECK(s.handler().getChatHistory().back().message == "Discovered Saldean's Farm: 100 experience gained");
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

#include "addons/lua_engine.hpp"

TEST_CASE("StaticPopup paragraph FontString GetHeight updates immediately on text change",
          "[replay][staticpopup][death][revive]") {
    wowee::addons::LuaEngine lua;
    REQUIRE(lua.initialize());

    // Create a StaticPopup-shaped dialog with a wrapsToWidth paragraph FontString
    // (<Size x="290" y="0"/>) and a 20-tall Button1, mirroring StaticPopup.xml.
    REQUIRE(lua.executeString(R"lua(
        local d = CreateFrame("Frame", "TestStaticPopup1", UIParent)
        d:SetWidth(320)
        d:SetHeight(72)
        d:SetPoint("TOP", UIParent, "TOP", 0, -135)
        local t = d:CreateFontString("TestStaticPopup1Text", "ARTWORK", "GameFontHighlight")
        t:SetWidth(290)
        t:SetHeight(0)
        t:SetPoint("TOP", d, "TOP", 0, -16)
        local b1 = CreateFrame("Button", "TestStaticPopup1Button1", d)
        b1:SetWidth(128)
        b1:SetHeight(20)
        b1:SetPoint("TOP", t, "BOTTOM", 0, -8)
        function TestResize()
            d:SetHeight(16 + t:GetHeight() + 8 + b1:GetHeight() + 16)
        end
    )lua"));

    auto& tree = lua.widgets();
    tree.layout(1280.0f, 720.0f);

    wowee::ui::Widget* textWidget = tree.findByName("TestStaticPopup1Text");
    REQUIRE(textWidget != nullptr);

    // 1) Simulate a tall 5-line XP_LOSS popup measured by WidgetRenderer::sizeFontStringWidget.
    const std::string longText =
        "If you find your corpse, you can resurrect for no penalty.  "
        "If I resurrect you all of your items will take 25% durability damage "
        "(equipped and inventory) and you will be afflicted by 10 min of Resurrection Sickness.";
    REQUIRE(lua.executeString(
        "TestStaticPopup1Text:SetText([[" + longText + "]])\n"
        "TestResize()\n"
        "H_LONG_TEXT = TestStaticPopup1Text:GetHeight()\n"
        "H_LONG_DIALOG = TestStaticPopup1:GetHeight()\n"));

    // Mark the paragraph as previously measured at the tall multi-line height,
    // exactly as WidgetRenderer::sizeFontStringWidget does on render.
    textWidget->wrapsToWidth = true;
    textWidget->autoSized = false;
    textWidget->measuredText = longText;
    textWidget->height = 64.0f;
    textWidget->rectH = 64.0f * (textWidget->effScale > 0.0f ? textWidget->effScale : 1.0f);

    // 2) Reuse the same StaticPopup1 for 1-line RECOVER_CORPSE ("Resurrect now?").
    // GetHeight() must NOT return the stale 64.0 height from XP_LOSS.
    REQUIRE(lua.executeString(R"lua(
        TestStaticPopup1Text:SetText("Resurrect now?")
        TestResize()
        H_SHORT_TEXT = TestStaticPopup1Text:GetHeight()
        H_SHORT_DIALOG = TestStaticPopup1:GetHeight()
    )lua"));
    CHECK(lua.evaluateBoolean("H_SHORT_TEXT > 0 and H_SHORT_TEXT < 25"));
    CHECK(lua.evaluateBoolean("H_SHORT_DIALOG > 60 and H_SHORT_DIALOG < 85"));

    // 3) Simulate renderer measuring the 1-line text, then transition to multi-line RESURRECT.
    textWidget->measuredText = "Resurrect now?";
    textWidget->height = 12.0f;
    textWidget->rectH = 12.0f * (textWidget->effScale > 0.0f ? textWidget->effScale : 1.0f);

    REQUIRE(lua.executeString(R"lua(
        TestStaticPopup1Text:SetFormattedText(
            "%s wants to resurrect you.  You will be afflicted with resurrection sickness.",
            "Edrikt")
        TestResize()
        H_REZ_TEXT = TestStaticPopup1Text:GetHeight()
        H_REZ_DIALOG = TestStaticPopup1:GetHeight()
    )lua"));
    CHECK(lua.evaluateBoolean("H_REZ_TEXT > H_SHORT_TEXT"));
    CHECK(lua.evaluateBoolean("H_REZ_DIALOG > H_SHORT_DIALOG"));

    // 4) Plural escape formatting in DEATH / RECOVER_CORPSE_TIMER countdown strings.
    REQUIRE(lua.executeString(R"lua(
        TestStaticPopup1Text:SetFormattedText("%d %s until release", 6, "|4Minute:Minutes;")
        DEATH_PLURAL_OK = (TestStaticPopup1Text:GetText() == "6 Minutes until release")
        TestStaticPopup1Text:SetFormattedText("%d %s until resurrection", 1, "|4Second:Seconds;")
        CORPSE_SINGULAR_OK = (TestStaticPopup1Text:GetText() == "1 Second until resurrection")
    )lua"));
    CHECK(lua.evaluateBoolean("DEATH_PLURAL_OK"));
    CHECK(lua.evaluateBoolean("CORPSE_SINGULAR_OK"));

    // 5) Clearing text on a wrapsToWidth FontString yields 0 height, not stale height.
    REQUIRE(lua.executeString(R"lua(
        TestStaticPopup1Text:SetText("")
        H_EMPTY = TestStaticPopup1Text:GetHeight()
    )lua"));
    CHECK(lua.evaluateBoolean("H_EMPTY == 0"));
}

TEST_CASE("Reviving does not create any custom onscreen 'You have been resurrected' banner",
          "[replay][revive][banner]") {
    wowee::addons::LuaEngine lua;
    REQUIRE(lua.initialize());
    CHECK(lua.evaluateBoolean("__WoweeResurrectBanner == nil"));
    // Firing PLAYER_ALIVE / PLAYER_UNGHOST must not create or show any custom banner.
    lua.fireEvent("PLAYER_ALIVE", {});
    lua.fireEvent("PLAYER_UNGHOST", {});
    CHECK(lua.evaluateBoolean("__WoweeResurrectBanner == nil"));
}

TEST_CASE("SMSG_CORPSE_RECLAIM_DELAY, SMSG_RESURRECT_REQUEST, and SMSG_SPIRIT_HEALER_CONFIRM update revive state",
          "[replay][revive][packets]") {
    Session s("tbc");
    auto& gh = s.handler();
    spawnAlivePlayer(s);

    std::vector<std::pair<std::string, std::vector<std::string>>> events;
    gh.setAddonEventCallback([&](const std::string& e, const std::vector<std::string>& args) {
        events.emplace_back(e, args);
    });

    // 1) SMSG_CORPSE_RECLAIM_DELAY (30 seconds)
    {
        wowee::network::Packet pkt(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_CORPSE_RECLAIM_DELAY));
        pkt.writeUInt32(30000);
        s.receive(pkt);
        CHECK(gh.getCorpseReclaimDelaySec() > 25.0f);
        CHECK(gh.getCorpseReclaimDelaySec() <= 30.0f);
    }

    // 2) TBC SMSG_RESURRECT_REQUEST from a player (isSpiritHealer=0, hasTimer=1)
    // CMaNGOS-TBC Player::SendResurrectRequest: guid(8) + nameLen(4) + sentName + uint8(isSpiritHealer) + uint8(!noResTimer)
    {
        const uint64_t priestGuid = 0x0000000000001234ull;
        const std::string name = "Priestess";
        wowee::network::Packet pkt(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_RESURRECT_REQUEST));
        pkt.writeUInt64(priestGuid);
        pkt.writeUInt32(static_cast<uint32_t>(name.size() + 1));
        pkt.writeString(name);
        pkt.writeUInt8(0);  // isSpiritHealer = false
        pkt.writeUInt8(1);  // !SPELL_ATTR_EX3_NO_RES_TIMER => hasTimer = true
        s.receive(pkt);
        CHECK_FALSE(gh.resurrectIsSpiritHealer());
        CHECK_FALSE(gh.resurrectHasSickness());
        CHECK(gh.resurrectHasTimer());
        REQUIRE_FALSE(events.empty());
        CHECK(events.back().first == "RESURRECT_REQUEST");
        REQUIRE(events.back().second.size() == 1);
        CHECK(events.back().second[0] == "Priestess");
    }

    // 3) TBC SMSG_RESURRECT_REQUEST from a Spirit Healer / instant-res spell (isSpiritHealer=1, hasTimer=0)
    {
        const uint64_t healerGuid = 0xF13000195B000001ull;
        const std::string name = "Spirit Healer";
        wowee::network::Packet pkt(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_RESURRECT_REQUEST));
        pkt.writeUInt64(healerGuid);
        pkt.writeUInt32(static_cast<uint32_t>(name.size() + 1));
        pkt.writeString(name);
        pkt.writeUInt8(1);  // isSpiritHealer = true => sickness = true
        pkt.writeUInt8(0);  // SPELL_ATTR_EX3_NO_RES_TIMER => hasTimer = false
        s.receive(pkt);
        CHECK(gh.resurrectIsSpiritHealer());
        CHECK(gh.resurrectHasSickness());
        CHECK_FALSE(gh.resurrectHasTimer());
        CHECK(events.back().first == "RESURRECT_REQUEST");
        CHECK(events.back().second[0] == "Spirit Healer");
    }

    // 3b) WotLK SMSG_RESURRECT_REQUEST preserves extra-terminator + optional trailing uint32 layout
    {
        Session sWotlk("wotlk");
        auto& ghW = sWotlk.handler();
        const uint64_t healerGuid = 0xF13000195B000001ull;
        const std::string name = "Spirit Healer";
        wowee::network::Packet pkt(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_RESURRECT_REQUEST));
        pkt.writeUInt64(healerGuid);
        pkt.writeUInt32(static_cast<uint32_t>(name.size() + 1));
        pkt.writeString(name);
        pkt.writeUInt8(0);  // WotLK second terminator
        pkt.writeUInt8(1);  // sickness = true
        pkt.writeUInt32(0); // waive reclaim delay => hasTimer = false
        sWotlk.receive(pkt);
        CHECK(ghW.resurrectHasSickness());
        CHECK_FALSE(ghW.resurrectHasTimer());
    }

    // 4) SMSG_SPIRIT_HEALER_CONFIRM fires CONFIRM_XP_LOSS
    {
        const uint64_t spiritGuid = 0xF13000195B000002ull;
        wowee::network::Packet pkt(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_SPIRIT_HEALER_CONFIRM));
        pkt.writeUInt64(spiritGuid);
        s.receive(pkt);
        CHECK(events.back().first == "CONFIRM_XP_LOSS");
    }
}

TEST_CASE("creating player alive after cross-map revive clears dead/ghost state and emits PLAYER_ALIVE once",
          "[replay][death][revive]") {
    constexpr uint32_t kPlayerFlagsGhost = 0x00000010u;

    auto sendNewWorld = [](Session& s, uint32_t mapId) {
        wowee::network::Packet p(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_NEW_WORLD));
        p.writeUInt32(mapId);
        p.writeFloat(100.0f);
        p.writeFloat(200.0f);
        p.writeFloat(30.0f);
        p.writeFloat(0.0f);
        s.receive(p);
    };

    SECTION("dead corpse teleported to new map and revived before CREATE_OBJECT2") {
        Session s("tbc");
        spawnAlivePlayer(s);
        kill(s, 0x08);
        REQUIRE(s.handler().isPlayerDead());
        REQUIRE(s.handler().getReleaseTimeRemainingSec() > 0.0);

        Events events(s);
        sendNewWorld(s, 0);
        s.receive(UpdateObject().tbc().create(
            Session::kPlayerGuid, ObjectType::PLAYER,
            {{UF::UNIT_FIELD_HEALTH, 100},
             {UF::UNIT_FIELD_MAXHEALTH, 100},
             {UF::PLAYER_FLAGS, 0u}},
            true));

        CHECK_FALSE(s.handler().isPlayerDead());
        CHECK_FALSE(s.handler().isPlayerGhost());
        CHECK(s.handler().getReleaseTimeRemainingSec() == 0.0);
        CHECK(std::count(events.names.begin(), events.names.end(), "PLAYER_ALIVE") == 1);
    }

    SECTION("dead corpse released cross-map to graveyard as ghost does not emit spurious PLAYER_ALIVE") {
        Session s("tbc");
        spawnAlivePlayer(s);
        kill(s, 0x08);
        REQUIRE(s.handler().isPlayerDead());

        Events events(s);
        sendNewWorld(s, 0);
        s.receive(UpdateObject().tbc().create(
            Session::kPlayerGuid, ObjectType::PLAYER,
            {{UF::UNIT_FIELD_HEALTH, 1},
             {UF::UNIT_FIELD_MAXHEALTH, 100},
             {UF::PLAYER_FLAGS, kPlayerFlagsGhost}},
            true));

        CHECK(s.handler().isPlayerDead());
        CHECK(s.handler().isPlayerGhost());
        CHECK(s.handler().getReleaseTimeRemainingSec() == 0.0);
        CHECK(std::count(events.names.begin(), events.names.end(), "PLAYER_ALIVE") == 0);
    }

    SECTION("ghost teleported to new map and revived before CREATE_OBJECT2 emits PLAYER_ALIVE once") {
        Session s("tbc");
        s.receive(UpdateObject().tbc().create(
            Session::kPlayerGuid, ObjectType::PLAYER,
            {{UF::UNIT_FIELD_HEALTH, 1},
             {UF::UNIT_FIELD_MAXHEALTH, 100},
             {UF::PLAYER_FLAGS, kPlayerFlagsGhost}},
            true));
        REQUIRE(s.handler().isPlayerDead());
        REQUIRE(s.handler().isPlayerGhost());

        Events events(s);
        sendNewWorld(s, 1);
        s.receive(UpdateObject().tbc().create(
            Session::kPlayerGuid, ObjectType::PLAYER,
            {{UF::UNIT_FIELD_HEALTH, 100},
             {UF::UNIT_FIELD_MAXHEALTH, 100},
             {UF::PLAYER_FLAGS, 0u}},
            true));

        CHECK_FALSE(s.handler().isPlayerDead());
        CHECK_FALSE(s.handler().isPlayerGhost());
        CHECK(std::count(events.names.begin(), events.names.end(), "PLAYER_ALIVE") == 1);
    }
}

TEST_CASE("TBC aura, party kill, and unit death transitions emit COMBAT_LOG_EVENT subevents",
          "[replay][death][combatlog]") {
    Session s("tbc");
    auto& gh = s.handler();
    gh.spellNameCacheRef()[1243].name = "Power Word: Fortitude";
    gh.spellNameCacheRef()[1243].schoolMask = 2;
    gh.spellNameCacheRef()[589].name = "Shadow Word: Pain";
    gh.spellNameCacheRef()[589].schoolMask = 32;
    gh.spellNameCacheRef()[7386].name = "Sunder Armor";
    gh.spellNameCacheRef()[7386].schoolMask = 1;
    gh.spellNameCacheLoadedRef() = true;

    const uint16_t ufAuras = wowee::game::fieldIndex(UF::UNIT_FIELD_AURAS);
    const uint16_t ufAuraApps = wowee::game::fieldIndex(UF::UNIT_FIELD_AURAAPPLICATIONS);
    REQUIRE(ufAuras != 0xFFFF);
    REQUIRE(ufAuraApps != 0xFFFF);

    std::vector<std::vector<std::string>> unfiltered;
    std::vector<std::vector<std::string>> filtered;
    gh.setAddonEventCallback([&](const std::string& e, const std::vector<std::string>& a) {
        if (e == "COMBAT_LOG_EVENT_UNFILTERED") unfiltered.push_back(a);
        else if (e == "COMBAT_LOG_EVENT") filtered.push_back(a);
    });

    // 1. Initial CREATE_OBJECT with existing buff/corpse must NOT emit SPELL_AURA_APPLIED or UNIT_DIED.
    constexpr uint64_t kDeadBoar = 0xF130000071000010ULL;
    constexpr uint64_t kLiveBoar = 0xF130000071000011ULL;
    s.receive(UpdateObject().tbc().create(
        Session::kPlayerGuid, ObjectType::PLAYER,
        {{UF::UNIT_FIELD_HEALTH, 100},
         {UF::UNIT_FIELD_MAXHEALTH, 100}},
        true));
    s.receive(UpdateObject().tbc().create(
        kDeadBoar, ObjectType::UNIT,
        {{UF::OBJECT_FIELD_ENTRY, 113},
         {UF::UNIT_FIELD_HEALTH, 0},
         {UF::UNIT_FIELD_MAXHEALTH, 50}}));
    s.receive(UpdateObject().tbc().create(
        kLiveBoar, ObjectType::UNIT,
        {{UF::OBJECT_FIELD_ENTRY, 113},
         {UF::UNIT_FIELD_HEALTH, 50},
         {UF::UNIT_FIELD_MAXHEALTH, 50}}));
    CHECK(unfiltered.empty());

    // 2. Live VALUES update applying buff (slot 0) on player and debuff (slot 40) on creature.
    s.receive(UpdateObject().tbc().valuesAt(Session::kPlayerGuid, {{ufAuras, 1243u}}));
    s.receive(UpdateObject().tbc().valuesAt(kLiveBoar, {{static_cast<uint16_t>(ufAuras + 40), 7386u}}));
    REQUIRE(unfiltered.size() == 2);
    REQUIRE(filtered.size() == 2);
    CHECK(unfiltered[0][1] == "SPELL_AURA_APPLIED");
    CHECK(unfiltered[0][4] == "2147483648"); // COMBATLOG_OBJECT_NONE when srcGuid == 0
    CHECK(unfiltered[0][8] == "1243");
    CHECK(unfiltered[0][11] == "BUFF");
    CHECK(unfiltered[1][1] == "SPELL_AURA_APPLIED");
    CHECK(unfiltered[1][8] == "7386");
    CHECK(unfiltered[1][11] == "DEBUFF");

    // 3. SMSG_SET_EXTRA_AURA_INFO_NEED_UPDATE refreshes duration -> SPELL_AURA_REFRESH,
    //    and UNIT_FIELD_AURAAPPLICATIONS increment -> SPELL_AURA_APPLIED_DOSE (stack 2).
    {
        wowee::network::Packet p(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_SET_EXTRA_AURA_INFO_NEED_UPDATE));
        // Packed GUID for kLiveBoar (0xF130000071000011ULL -> bytes 0, 2, 6, 7 non-zero)
        p.writeUInt8(0xC5u);
        p.writeUInt8(0x11u);
        p.writeUInt8(0x71u);
        p.writeUInt8(0x30u);
        p.writeUInt8(0xF1u);
        p.writeUInt8(40u);      // negative slot 40
        p.writeUInt32(7386u);   // Sunder Armor
        p.writeUInt32(30000u);  // maxDuration
        p.writeUInt32(30000u);  // duration
        s.receive(p);
    }
    s.receive(UpdateObject().tbc().valuesAt(
        kLiveBoar, {{static_cast<uint16_t>(ufAuraApps + (40 / 4)), 1u}})); // byte 0 = 1 -> 2 stacks
    REQUIRE(unfiltered.size() == 4);
    CHECK(unfiltered[2][1] == "SPELL_AURA_REFRESH");
    CHECK(unfiltered[2][8] == "7386");
    CHECK(unfiltered[2][11] == "DEBUFF");
    CHECK(unfiltered[3][1] == "SPELL_AURA_APPLIED_DOSE");
    CHECK(unfiltered[3][11] == "DEBUFF");
    CHECK(unfiltered[3][12] == "2");

    // 4. Clearing slot 40 emits SPELL_AURA_REMOVED.
    s.receive(UpdateObject().tbc().valuesAt(kLiveBoar, {{static_cast<uint16_t>(ufAuras + 40), 0u}}));
    REQUIRE(unfiltered.size() == 5);
    CHECK(unfiltered[4][1] == "SPELL_AURA_REMOVED");
    CHECK(unfiltered[4][8] == "7386");

    // 5. Hunter Feign Death (UNIT_DYNFLAG_DEAD + UNIT_FIELD_FLAGS_2=0x1 while health > 0) must NOT emit UNIT_DIED.
    s.receive(UpdateObject().tbc().values(
        Session::kPlayerGuid,
        {{UF::UNIT_FIELD_FLAGS_2, 0x1u},
         {UF::UNIT_DYNAMIC_FLAGS, wowee::game::UNIT_DYNFLAG_DEAD}}));
    CHECK(unfiltered.size() == 5);

    // 6. SMSG_PARTYKILLLOG + living->dead VALUES (health=0 + UNIT_DYNFLAG_DEAD in one block)
    //    emits PARTY_KILL once and UNIT_DIED once, with no duplicate on later lootable update.
    {
        wowee::network::Packet pk(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_PARTYKILLLOG));
        pk.writeUInt64(Session::kPlayerGuid);
        pk.writeUInt64(kLiveBoar);
        s.receive(pk);
    }
    s.receive(UpdateObject().tbc().values(
        kLiveBoar,
        {{UF::UNIT_FIELD_HEALTH, 0},
         {UF::UNIT_DYNAMIC_FLAGS, wowee::game::UNIT_DYNFLAG_DEAD}}));
    s.receive(UpdateObject().tbc().values(
        kLiveBoar,
        {{UF::UNIT_DYNAMIC_FLAGS, wowee::game::UNIT_DYNFLAG_DEAD | wowee::game::UNIT_DYNFLAG_LOOTABLE}}));
    REQUIRE(unfiltered.size() == 7);
    CHECK(unfiltered[5][1] == "PARTY_KILL");
    CHECK(unfiltered[5][2] == "0x0000000000000042");
    CHECK(unfiltered[5][5] == "0xF130000071000011");
    CHECK(unfiltered[6][1] == "UNIT_DIED");
    CHECK(unfiltered[6][2] == "0x0000000000000000");
    CHECK(unfiltered[6][4] == "2147483648");
    CHECK(unfiltered[6][5] == "0xF130000071000011");
}
