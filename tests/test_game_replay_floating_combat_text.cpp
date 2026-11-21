// Comprehensive TBC 2.4.3 Floating Combat Text (Blizzard_CombatText + C++ events)
// parity tests: loads the real shipped TBC Blizzard_CombatText.xml + .lua, enables
// all optional FCT checkboxes, drives real server packets through replay::Session,
// and asserts every floater's text, RGB color, crit flag, and scroll trajectory.
#include <catch_amalgamated.hpp>

#include "addons/lua_engine.hpp"
#include "game_replay.hpp"
#include "game/game_services.hpp"
#include "ui/framexml_emitter.hpp"
#include "ui/widget_tree.hpp"
#include "ui/xml_parser.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using replay::Session;
using replay::UpdateObject;
using wowee::game::CombatTextEntry;
using wowee::game::GameHandler;
using wowee::game::ObjectType;
using wowee::game::Opcode;
using wowee::game::wireOpcode;
using wowee::network::Packet;
using UF = wowee::game::UF;

namespace {

constexpr uint64_t kMobGuid    = 0xF130000000005001ull;
constexpr uint64_t kHealerGuid = 0x0000000000005002ull;

uint32_t packUnitBytes0(uint8_t race, uint8_t cls, uint8_t gender, uint8_t powerType) {
    return static_cast<uint32_t>(race) |
           (static_cast<uint32_t>(cls) << 8) |
           (static_cast<uint32_t>(gender) << 16) |
           (static_cast<uint32_t>(powerType) << 24);
}

std::string findTbcInterfaceDir() {
    for (const char* candidate : {
             "/workspace/azerothcore/wowee/Data/expansions/tbc/interface",
             "/workspace/azerothcore/client/wowee-data/expansions/tbc/interface",
         }) {
        if (std::filesystem::is_directory(candidate)) return candidate;
    }
    return {};
}

std::string readTextFile(const std::filesystem::path& p) {
    std::ifstream in(p);
    REQUIRE(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

struct RecordedMessage {
    std::string text;
    double r = 0.0, g = 0.0, b = 0.0;
    std::string displayType;
};

struct FctHarness {
    Session session{"tbc"};
    wowee::addons::LuaEngine lua;
    std::vector<std::pair<std::string, std::vector<std::string>>> rawEvents;

    FctHarness(uint8_t playerClass = 1, uint8_t powerType = 1) {
        REQUIRE(lua.initialize());
        lua.setGameHandler(&session.handler());

        // Spawn player & mob
        session.receive(UpdateObject().tbc().create(
            Session::kPlayerGuid, ObjectType::PLAYER,
            {{UF::UNIT_FIELD_HEALTH, 2000},
             {UF::UNIT_FIELD_MAXHEALTH, 2000},
             {UF::UNIT_FIELD_LEVEL, 70},
             {UF::UNIT_FIELD_BYTES_0, packUnitBytes0(1, playerClass, 0, powerType)},
             {UF::UNIT_FIELD_POWER1, 1000},
             {UF::UNIT_FIELD_MAXPOWER1, 1000}},
            true));
        if (auto* player = session.handler().getUnitByGuid(Session::kPlayerGuid)) {
            player->setPowerByType(1, 0);
            player->setMaxPowerByType(1, 1000);
            player->setPowerByType(3, 100);
            player->setMaxPowerByType(3, 100);
        }
        session.receive(UpdateObject().tbc().create(
            kMobGuid, ObjectType::UNIT,
            {{UF::UNIT_FIELD_HEALTH, 5000},
             {UF::UNIT_FIELD_MAXHEALTH, 5000},
             {UF::UNIT_FIELD_LEVEL, 70}}));
        session.handler().setTargetGuidRaw(kMobGuid);

        // Wire GameHandler events into LuaEngine
        session.handler().setAddonEventCallback([this](const std::string& ev,
                                                       const std::vector<std::string>& args) {
            rawEvents.emplace_back(ev, args);
            lua.fireEvent(ev, args);
        });

        // Provide TBC GlobalStrings needed by Blizzard_CombatText.lua and load the real addon
        const std::string iface = findTbcInterfaceDir();
        REQUIRE_FALSE(iface.empty());

        REQUIRE(lua.executeString(R"lua(
            function __WoweeXmlStep() end
            WorldFrame:SetWidth(1024)
            WorldFrame:SetHeight(768)
            AURA_END = "<%s> fades"
            MANA = "Mana"
            RAGE = "Rage"
            FOCUS = "Focus"
            ENERGY = "Energy"
            MISS = "Miss"
            DODGE = "Dodge"
            PARRY = "Parry"
            EVADE = "Evade"
            IMMUNE = "Immune"
            DEFLECT = "Deflect"
            REFLECT = "Reflect"
            BLOCK = "Block"
            ABSORB = "Absorb"
            RESIST = "Resist"
            BLOCK_TRAILER = " (%d blocked)"
            ABSORB_TRAILER = " (%d absorbed)"
            RESIST_TRAILER = " (%d resisted)"
            COMBAT_TEXT_HONOR_GAINED = "%s Honor"
            COMBAT_TEXT_COMBO_POINTS = "%d Combo"
            COMBAT_TEXT_HEALTH_LOW = "Low Health!"
            COMBAT_TEXT_MANA_LOW = "Low Mana!"
            COMBAT_TEXT_ENTERING_COMBAT = "+Combat"
            COMBAT_TEXT_LEAVING_COMBAT = "-Combat"
            MAX_COMBO_POINTS = 5
            SHOW_COMBAT_TEXT = "1"
            COMBAT_TEXT_FLOAT_MODE = "1"
            COMBAT_TEXT_SHOW_COMBAT_STATE = "1"
            COMBAT_TEXT_SHOW_DODGE_PARRY_MISS = "1"
            COMBAT_TEXT_SHOW_RESISTANCES = "1"
            COMBAT_TEXT_SHOW_REPUTATION = "1"
            COMBAT_TEXT_SHOW_REACTIVES = "1"
            COMBAT_TEXT_SHOW_FRIENDLY_NAMES = "1"
            COMBAT_TEXT_SHOW_COMBO_POINTS = "1"
            COMBAT_TEXT_SHOW_LOW_HEALTH_MANA = "1"
            COMBAT_TEXT_SHOW_MANA = "1"
            COMBAT_TEXT_SHOW_HONOR_GAINED = "1"
            COMBAT_TEXT_SHOW_AURAS = "1"
        )lua"));

        // Load real TBC Blizzard_CombatText.lua + Blizzard_CombatText.xml
        const std::filesystem::path ctDir =
            std::filesystem::path(iface) / "addons/blizzard_combattext";
        REQUIRE(lua.executeString(readTextFile(ctDir / "blizzard_combattext.lua")));
        wowee::ui::XmlNode xmlRoot;
        std::string xmlErr;
        REQUIRE(wowee::ui::parseXml(readTextFile(ctDir / "blizzard_combattext.xml"), xmlRoot, xmlErr));
        const auto emitted = wowee::ui::emitFrameXml(xmlRoot);
        REQUIRE(lua.executeString(emitted.lua));

        // Wrap CombatText_AddMessage to record every message while still running real animation logic,
        // and bind TBC globals (this/event/arg1..3) on CombatText's OnEvent just as TBC XML loading does.
        REQUIRE(lua.executeString(R"lua(
            __fct_log = {}
            local __orig_add = CombatText_AddMessage
            function CombatText_AddMessage(message, scrollFunction, r, g, b, displayType, isStaggered)
                table.insert(__fct_log, {
                    text = tostring(message),
                    r = r or 0, g = g or 0, b = b or 0,
                    displayType = displayType or "",
                })
                return __orig_add(message, scrollFunction, r, g, b, displayType, isStaggered)
            end
            CombatText:SetScript("OnEvent", function(self, ev, a1, a2, a3, a4)
                this, event, arg1, arg2, arg3, arg4 = self, ev, a1, a2, a3, a4
                CombatText_OnEvent(ev)
            end)
            CombatText_UpdateDisplayedMessages()
        )lua"));
    }

    std::vector<RecordedMessage> recorded() {
        std::vector<RecordedMessage> out;
        REQUIRE(lua.executeString("__fct_n = tostring(#__fct_log)"));
        const int n = static_cast<int>(std::stod(lua.globalString("__fct_n")));
        for (int i = 1; i <= n; ++i) {
            const std::string idx = std::to_string(i);
            REQUIRE(lua.executeString(
                "__m_t = __fct_log[" + idx + "].text "
                "__m_r = tostring(__fct_log[" + idx + "].r) "
                "__m_g = tostring(__fct_log[" + idx + "].g) "
                "__m_b = tostring(__fct_log[" + idx + "].b) "
                "__m_d = __fct_log[" + idx + "].displayType"));
            RecordedMessage m;
            m.text = lua.globalString("__m_t");
            m.r = std::stod(lua.globalString("__m_r"));
            m.g = std::stod(lua.globalString("__m_g"));
            m.b = std::stod(lua.globalString("__m_b"));
            m.displayType = lua.globalString("__m_d");
            out.push_back(std::move(m));
        }
        return out;
    }

    void clearRecorded() {
        REQUIRE(lua.executeString("__fct_log = {}"));
        rawEvents.clear();
    }
};

Packet buildSetExtraAuraInfo(uint64_t guid, uint8_t slot, uint32_t spellId, int32_t maxMs, uint32_t remMs) {
    Packet p(wireOpcode(Opcode::SMSG_SET_EXTRA_AURA_INFO_NEED_UPDATE));
    p.writePackedGuid(guid);
    p.writeUInt8(slot);
    p.writeUInt32(spellId);
    p.writeUInt32(static_cast<uint32_t>(maxMs));
    p.writeUInt32(remMs);
    return p;
}

Packet buildLearnedSpell(uint32_t spellId) {
    Packet p(wireOpcode(Opcode::SMSG_LEARNED_SPELL));
    p.writeUInt32(spellId);
    return p;
}

Packet buildClearExtraAuraInfo(uint64_t guid, uint8_t slot) {
    Packet p(wireOpcode(Opcode::SMSG_CLEAR_EXTRA_AURA_INFO));
    p.writeUInt64(guid);
    p.writeUInt8(slot);
    return p;
}

Packet buildPowerUpdateTbc(uint64_t guid, uint8_t powerType, uint32_t val) {
    Packet p(wireOpcode(Opcode::SMSG_POWER_UPDATE));
    p.writeUInt64(guid);
    p.writeUInt8(powerType);
    p.writeUInt32(val);
    return p;
}

}  // namespace

TEST_CASE("Blizzard_CombatText receives Rage/Mana/Energy gains and deduplicates energize", "[fct][framexml]") {
    FctHarness h(/*warrior*/ 1, /*rage*/ 1);
    const uint16_t rageField = static_cast<uint16_t>(wowee::game::fieldIndex(UF::UNIT_FIELD_POWER1) + 1);

    // 1. Melee rage gain via UNIT_FIELD_POWER2 (0 -> 50 raw = +5 Rage)
    h.session.receive(UpdateObject().tbc().valuesAt(
        Session::kPlayerGuid, {{rageField, 50}}));
    auto msgs = h.recorded();
    REQUIRE(msgs.size() == 1);
    CHECK(msgs[0].text == "+5 Rage");
    CHECK(msgs[0].r == Catch::Approx(0.1));
    CHECK(msgs[0].g == Catch::Approx(0.1));
    CHECK(msgs[0].b == Catch::Approx(1.0));

    // 2. SMSG_SPELLENERGIZELOG (+10 Rage) immediately followed by UNIT_FIELD_POWER2 (+100 raw = +10 Rage):
    // must fire ONCE, not twice.
    h.clearRecorded();
    Packet energize(wireOpcode(Opcode::SMSG_SPELLENERGIZELOG));
    energize.writePackedGuid(Session::kPlayerGuid);
    energize.writePackedGuid(Session::kPlayerGuid);
    energize.writeUInt32(2687); // Bloodrage
    energize.writeUInt32(1);    // POWER_RAGE
    energize.writeUInt32(100);  // 10 Rage
    h.session.receive(energize);
    h.session.receive(UpdateObject().tbc().valuesAt(
        Session::kPlayerGuid, {{rageField, 150}}));
    msgs = h.recorded();
    REQUIRE(msgs.size() == 1);
    CHECK(msgs[0].text == "+10 Rage");

    // 3. UNIT_FIELD_POWER1 update fires UNIT_MANA (not UNIT_POWER) and reports +80 Mana gain
    h.clearRecorded();
    h.session.receive(UpdateObject().tbc().values(
        Session::kPlayerGuid, {{UF::UNIT_FIELD_POWER1, 1080}}));
    msgs = h.recorded();
    REQUIRE(msgs.size() == 1);
    CHECK(msgs[0].text == "+80 Mana");
    bool sawUnitMana = false;
    bool sawUnitPower = false;
    for (const auto& [ev, _] : h.rawEvents) {
        if (ev == "UNIT_MANA") sawUnitMana = true;
        if (ev == "UNIT_POWER") sawUnitPower = true;
    }
    CHECK(sawUnitMana);
    CHECK_FALSE(sawUnitPower);
}

TEST_CASE("Blizzard_CombatText receives helpful & harmful aura start/fade via ExtraAuraInfo and field updates", "[fct][framexml]") {
    FctHarness h;
    h.session.handler().spellNameCacheRef()[21562].name = "Prayer of Fortitude";
    h.session.handler().spellNameCacheRef()[6788].name  = "Weakened Soul";

    // Helpful aura in slot 0 via SMSG_SET_EXTRA_AURA_INFO
    h.session.receive(buildSetExtraAuraInfo(Session::kPlayerGuid, 0, 21562, 1800000, 1800000));
    // Harmful debuff in slot 40 via SMSG_SET_EXTRA_AURA_INFO
    h.session.receive(buildSetExtraAuraInfo(Session::kPlayerGuid, 40, 6788, 15000, 15000));
    // Subsequent UNIT_FIELD_AURAS update with same slots must NOT duplicate SPELL_AURA_START
    const uint16_t auraBase = wowee::game::fieldIndex(UF::UNIT_FIELD_AURAS);
    h.session.receive(UpdateObject().tbc().valuesAt(
        Session::kPlayerGuid,
        {{auraBase, 21562},
         {static_cast<uint16_t>(auraBase + 40), 6788}}));

    auto msgs = h.recorded();
    REQUIRE(msgs.size() == 2);
    CHECK(msgs[0].text == "<Prayer of Fortitude>");
    CHECK(msgs[0].g == Catch::Approx(1.0));
    CHECK(msgs[1].text == "<Weakened Soul>");
    CHECK(msgs[1].r == Catch::Approx(1.0));
    CHECK(msgs[1].g == Catch::Approx(0.1));

    // Fade both via SMSG_CLEAR_EXTRA_AURA_INFO (before UNIT_FIELD_AURAS clears them)
    h.clearRecorded();
    h.session.receive(buildClearExtraAuraInfo(Session::kPlayerGuid, 0));
    h.session.receive(buildClearExtraAuraInfo(Session::kPlayerGuid, 40));
    msgs = h.recorded();
    REQUIRE(msgs.size() == 2);
    CHECK(msgs[0].text == "<Prayer of Fortitude> fades");
    CHECK(msgs[0].g == Catch::Approx(1.0));
    CHECK(msgs[1].text == "<Weakened Soul> fades");
    CHECK(msgs[1].r == Catch::Approx(1.0));
}

TEST_CASE("Blizzard_CombatText receives reactive SPELL_ACTIVE for Overpower, Revenge, and Execute", "[fct][framexml]") {
    FctHarness h(/*warrior*/ 1, /*rage*/ 1);
    auto& spells = h.session.handler().spellNameCacheRef();

    // Overpower (attrEx 0x40100000)
    spells[7384].name = "Overpower";
    spells[7384].attrEx = 0x40100000u;
    h.session.receive(buildLearnedSpell(7384));

    // Revenge (casterAuraState = 1, AURA_STATE_DEFENSE)
    spells[6572].name = "Revenge";
    spells[6572].casterAuraState = 1;
    h.session.receive(buildLearnedSpell(6572));

    // Execute (targetAuraState = 2, AURA_STATE_HEALTHLESS_20_PERCENT)
    spells[5308].name = "Execute";
    spells[5308].targetAuraState = 2;
    h.session.receive(buildLearnedSpell(5308));
    h.clearRecorded();

    // 1. Overpower proc via SMSG_UPDATE_COMBO_POINTS(target, 1)
    Packet cp(wireOpcode(Opcode::SMSG_UPDATE_COMBO_POINTS));
    cp.writePackedGuid(kMobGuid);
    cp.writeUInt8(1);
    h.session.receive(cp);

    // 2. Revenge proc via player UNIT_FIELD_AURASTATE bit 0 (state 1)
    h.session.receive(UpdateObject().tbc().values(
        Session::kPlayerGuid, {{UF::UNIT_FIELD_AURASTATE, 1u << 0}}));

    // 3. Execute proc via target UNIT_FIELD_AURASTATE bit 1 (state 2)
    h.session.receive(UpdateObject().tbc().values(
        kMobGuid, {{UF::UNIT_FIELD_AURASTATE, 1u << 1}}));

    auto msgs = h.recorded();
    REQUIRE(msgs.size() == 3);
    CHECK(msgs[0].text == "<Overpower>");
    CHECK(msgs[0].displayType == "crit");
    CHECK(msgs[0].r == Catch::Approx(1.0));
    CHECK(msgs[0].g == Catch::Approx(0.82));
    CHECK(msgs[1].text == "<Revenge>");
    CHECK(msgs[1].displayType == "crit");
    CHECK(msgs[2].text == "<Execute>");
    CHECK(msgs[2].displayType == "crit");
}

TEST_CASE("Blizzard_CombatText receives combat state, low HP/mana, combo points, honor, heal, environmental, and float modes", "[fct][framexml]") {
    FctHarness h(/*mage*/ 8, /*mana*/ 0);

    // Combat state: +Combat / -Combat
    constexpr uint32_t kInCombat = 0x00080000u;
    h.session.receive(UpdateObject().tbc().values(
        Session::kPlayerGuid, {{UF::UNIT_FIELD_FLAGS, kInCombat}}));
    h.session.receive(UpdateObject().tbc().values(
        Session::kPlayerGuid, {{UF::UNIT_FIELD_FLAGS, 0u}}));

    // Low health (<= 20% of 2000 -> 300) and low mana (<= 20% of 1000 -> 150)
    h.session.receive(UpdateObject().tbc().values(
        Session::kPlayerGuid, {{UF::UNIT_FIELD_HEALTH, 300}}));
    h.session.receive(UpdateObject().tbc().values(
        Session::kPlayerGuid, {{UF::UNIT_FIELD_POWER1, 150}}));

    // Incoming heal with unknown caster (empty arg2 must not shift arg3 into arg2)
    h.session.handler().addCombatText(
        CombatTextEntry::HEAL, 420, 2061, false, 0, kHealerGuid, Session::kPlayerGuid);

    // Environmental fall damage
    h.session.handler().addCombatText(
        CombatTextEntry::ENVIRONMENTAL, 125, 0, false, 2, 0, Session::kPlayerGuid);

    // Partial spell resist (must display via RESIST, not drop as SPELL_RESISTED)
    h.session.handler().addCombatText(
        CombatTextEntry::SPELL_DAMAGE, 200, 133, false, 0, kMobGuid, Session::kPlayerGuid);
    h.session.handler().addCombatText(
        CombatTextEntry::RESIST, 50, 133, false, 0, kMobGuid, Session::kPlayerGuid, 4, true);

    // Honor gain
    h.session.handler().addCombatText(
        CombatTextEntry::HONOR_GAIN, 25, 0, true, 0, Session::kPlayerGuid, Session::kPlayerGuid);

    auto msgs = h.recorded();
    REQUIRE(msgs.size() == 9);
    CHECK(msgs[0].text == "+Combat");
    CHECK(msgs[1].text == "-Combat");
    CHECK(msgs[2].text == "Low Health!");
    CHECK(msgs[3].text == "Low Mana!");
    CHECK(msgs[4].text == "+420 []");
    CHECK(msgs[5].text == "-125");
    CHECK(msgs[6].text == "-200");
    CHECK(msgs[7].text == "200  (50 resisted)");
    CHECK(msgs[8].text == "+25 Honor");

    // Verify Reputation change (COMBAT_TEXT_UPDATE "FACTION")
    h.clearRecorded();
    h.lua.fireEvent("COMBAT_TEXT_UPDATE", {"FACTION", "Stormwind", "50"});
    h.lua.fireEvent("COMBAT_TEXT_UPDATE", {"FACTION", "Booty Bay", "-25"});
    msgs = h.recorded();
    REQUIRE(msgs.size() == 2);
    CHECK(msgs[0].text == "(Stormwind +50)");
    CHECK(msgs[0].b == Catch::Approx(1.0));
    CHECK(msgs[1].text == "(Booty Bay -25)");

    // Verify all three float modes (1=Up, 2=Down, 3=Arc) animate without error
    for (const char* mode : {"1", "2", "3"}) {
        REQUIRE(h.lua.executeString(
            std::string("COMBAT_TEXT_FLOAT_MODE = '") + mode + "' "
            "CombatText_UpdateDisplayedMessages() "
            "CombatText_AddMessage('-99', COMBAT_TEXT_SCROLL_FUNCTION, 1, 0.1, 0.1) "
            "local s = COMBAT_TEXT_TO_ANIMATE[1] "
            "__y0 = tostring(s.yPos) "
            "CombatText_OnUpdate(0.5) "
            "__y1 = tostring(s.yPos)"));
        const double y0 = std::stod(h.lua.globalString("__y0"));
        const double y1 = std::stod(h.lua.globalString("__y1"));
        if (std::string(mode) == "2")
            CHECK(y1 < y0);  // Scroll Down
        else
            CHECK(y1 > y0);  // Scroll Up & Arc
    }
}
