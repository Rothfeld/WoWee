// Comprehensive TBC 2.4.3 Floating Combat Text (Blizzard_CombatText + C++ events)
// parity tests: loads the real shipped TBC Blizzard_CombatText.xml + .lua, enables
// all optional FCT checkboxes, drives real server packets through replay::Session,
// and asserts every floater's text, RGB color, crit flag, and scroll trajectory.
#include <catch_amalgamated.hpp>

#include "addons/lua_engine.hpp"
#include "game_replay.hpp"
#include "game/game_services.hpp"
#include "ui/floating_combat_text.hpp"
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

Packet buildClearExtraAuraInfo(uint64_t guid, uint32_t spellId) {
    Packet p(wireOpcode(Opcode::SMSG_CLEAR_EXTRA_AURA_INFO));
    p.writePackedGuid(guid);
    p.writeUInt32(spellId);
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

TEST_CASE("Blizzard_CombatText receives spell power gains without melee swing or natural regen spam", "[fct][framexml]") {
    FctHarness h(/*warrior*/ 1, /*rage*/ 1);
    const uint16_t rageField = static_cast<uint16_t>(wowee::game::fieldIndex(UF::UNIT_FIELD_POWER1) + 1);

    // 1. Melee rage gain via UNIT_FIELD_POWER2 and SMSG_POWER_UPDATE must NOT spam +Rage FCT
    // (verified on Native TBC 2.4.3 Pair 3 & AQ20 captures), while still firing UNIT_RAGE.
    h.session.receive(UpdateObject().tbc().valuesAt(
        Session::kPlayerGuid, {{rageField, 50}}));
    h.session.receive(buildPowerUpdateTbc(Session::kPlayerGuid, 1, 120));
    CHECK(h.recorded().empty());
    bool sawUnitRage = false;
    for (const auto& [ev, _] : h.rawEvents) {
        if (ev == "UNIT_RAGE") sawUnitRage = true;
    }
    CHECK(sawUnitRage);

    // 2. SMSG_SPELLENERGIZELOG (+10 Rage Bloodrage) followed by UNIT_FIELD_POWER2 fires ONCE.
    h.clearRecorded();
    Packet energize(wireOpcode(Opcode::SMSG_SPELLENERGIZELOG));
    energize.writePackedGuid(Session::kPlayerGuid);
    energize.writePackedGuid(Session::kPlayerGuid);
    energize.writeUInt32(2687); // Bloodrage
    energize.writeUInt32(1);    // POWER_RAGE
    energize.writeUInt32(100);  // 10 Rage
    h.session.receive(energize);
    h.session.receive(UpdateObject().tbc().valuesAt(
        Session::kPlayerGuid, {{rageField, 220}}));
    auto msgs = h.recorded();
    REQUIRE(msgs.size() == 1);
    CHECK(msgs[0].text == "+10 Rage");
    CHECK(msgs[0].r == Catch::Approx(0.1));
    CHECK(msgs[0].g == Catch::Approx(0.1));
    CHECK(msgs[0].b == Catch::Approx(1.0));

    // 3. SMSG_PERIODICAURALOG (PERIODIC_ENERGIZE = 24, e.g. Bloodrage tick or Vaelastrasz
    // Essence of the Red 23513 ticking +500 Mana / +50 Energy / +20 Rage) must NOT fire
    // COMBAT_TEXT_UPDATE in TBC 2.4.3 (verified on Pair 8; only instant SMSG_SPELLENERGIZELOG does).
    h.clearRecorded();
    Packet periodicEnergize(wireOpcode(Opcode::SMSG_PERIODICAURALOG));
    periodicEnergize.writePackedGuid(Session::kPlayerGuid);
    periodicEnergize.writePackedGuid(Session::kPlayerGuid);
    periodicEnergize.writeUInt32(23513); // Essence of the Red
    periodicEnergize.writeUInt32(3);     // count
    periodicEnergize.writeUInt32(24);    // SPELL_AURA_PERIODIC_ENERGIZE
    periodicEnergize.writeUInt32(0);     // POWER_MANA
    periodicEnergize.writeUInt32(500);
    periodicEnergize.writeUInt32(24);    // SPELL_AURA_PERIODIC_ENERGIZE
    periodicEnergize.writeUInt32(3);     // POWER_ENERGY
    periodicEnergize.writeUInt32(50);
    periodicEnergize.writeUInt32(24);    // SPELL_AURA_PERIODIC_ENERGIZE
    periodicEnergize.writeUInt32(1);     // POWER_RAGE
    periodicEnergize.writeUInt32(200);   // 20 Rage in tenths
    h.session.receive(periodicEnergize);
    CHECK(h.recorded().empty());

    // 4. Natural UNIT_FIELD_POWER1 mana regeneration tick fires UNIT_MANA (not UNIT_POWER)
    // and does NOT emit +Mana FCT.
    h.clearRecorded();
    h.session.receive(UpdateObject().tbc().values(
        Session::kPlayerGuid, {{UF::UNIT_FIELD_POWER1, 1080}}));
    CHECK(h.recorded().empty());
    bool sawUnitMana = false;
    bool sawUnitPower = false;
    for (const auto& [ev, _] : h.rawEvents) {
        if (ev == "UNIT_MANA") sawUnitMana = true;
        if (ev == "UNIT_POWER") sawUnitPower = true;
    }
    CHECK(sawUnitMana);
    CHECK_FALSE(sawUnitPower);
}

TEST_CASE("Blizzard_CombatText receives helpful & harmful aura start/fade via UNIT_FIELD_AURAS without refresh spam", "[fct][framexml]") {
    FctHarness h;
    h.session.handler().spellNameCacheRef()[21562].name = "Prayer of Fortitude";
    h.session.handler().spellNameCacheRef()[6788].name  = "Weakened Soul";
    h.session.handler().spellNameCacheRef()[35099].name = "Rapid Killing";

    // Helpful aura in slot 0 and harmful debuff in slot 40: SMSG_SET_EXTRA_AURA_INFO
    // provides caster duration metadata only; COMBAT_TEXT_UPDATE fires once when
    // UNIT_FIELD_AURAS VALUES confirms the aura is present.
    h.session.receive(buildSetExtraAuraInfo(Session::kPlayerGuid, 0, 21562, 1800000, 1800000));
    h.session.receive(buildSetExtraAuraInfo(Session::kPlayerGuid, 40, 6788, 15000, 15000));
    CHECK(h.recorded().empty());

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

    // Same-tick refresh (CMaNGOS AURA_REMOVE_BY_STACK: SMSG_CLEAR_EXTRA_AURA_INFO +
    // SMSG_SET_EXTRA_AURA_INFO while UNIT_FIELD_AURAS keeps the aura active, e.g.
    // Hunter Rapid Killing on back-to-back kills) must NOT emit fade + gain spam.
    h.clearRecorded();
    h.session.receive(buildSetExtraAuraInfo(Session::kPlayerGuid, 1, 35099, 20000, 20000));
    h.session.receive(UpdateObject().tbc().valuesAt(
        Session::kPlayerGuid, {{static_cast<uint16_t>(auraBase + 1), 35099}}));
    REQUIRE(h.recorded().size() == 1);
    CHECK(h.recorded()[0].text == "<Rapid Killing>");

    h.clearRecorded();
    for (int kill = 0; kill < 5; ++kill) {
        h.session.receive(buildClearExtraAuraInfo(Session::kPlayerGuid, 35099));
        h.session.receive(buildSetExtraAuraInfo(Session::kPlayerGuid, 1, 35099, 20000, 20000));
        h.session.receive(UpdateObject().tbc().valuesAt(
            Session::kPlayerGuid, {{static_cast<uint16_t>(auraBase + 1), 35099}}));
    }
    CHECK(h.recorded().empty());

    // Actual fade occurs when UNIT_FIELD_AURAS clears the slots.
    h.clearRecorded();
    h.session.receive(buildClearExtraAuraInfo(Session::kPlayerGuid, 21562));
    h.session.receive(buildClearExtraAuraInfo(Session::kPlayerGuid, 6788));
    h.session.receive(buildClearExtraAuraInfo(Session::kPlayerGuid, 35099));
    CHECK(h.recorded().empty());
    h.session.receive(UpdateObject().tbc().valuesAt(
        Session::kPlayerGuid,
        {{auraBase, 0},
         {static_cast<uint16_t>(auraBase + 1), 0},
         {static_cast<uint16_t>(auraBase + 40), 0}}));
    msgs = h.recorded();
    REQUIRE(msgs.size() == 3);
    CHECK(msgs[0].text == "<Prayer of Fortitude> fades");
    CHECK(msgs[0].g == Catch::Approx(1.0));
    CHECK(msgs[1].text == "<Rapid Killing> fades");
    CHECK(msgs[1].g == Catch::Approx(1.0));
    CHECK(msgs[2].text == "<Weakened Soul> fades");
    CHECK(msgs[2].r == Catch::Approx(1.0));
}

TEST_CASE("Blizzard_CombatText suppresses DO_NOT_DISPLAY and DO_NOT_LOG auras across all TBC update paths", "[fct][framexml]") {
    FctHarness h(/*warrior*/ 1, /*rage*/ 1);
    auto& spells = h.session.handler().spellNameCacheRef();

    // Real TBC 2.4.3 Spell.dbc entries:
    // Suppressed:
    spells[33380].name = "Rampage Enabler (DND)";
    spells[33380].attr = 0x00000180u;  // SPELL_ATTR_DO_NOT_DISPLAY | SPELL_ATTR_DO_NOT_LOG
    spells[5320].name  = "Echeyakee's Grace";
    spells[5320].attr  = 0x08000080u;  // SPELL_ATTR_DO_NOT_DISPLAY only
    spells[433].name   = "Food";
    spells[433].attr   = 0x18000100u;  // SPELL_ATTR_DO_NOT_LOG only (visible buff icon)
    spells[29479].name = "Unscathed (DND)";
    spells[29479].attr = 0x00000100u;  // SPELL_ATTR_DO_NOT_LOG only
    // Logged:
    spells[71].name     = "Defensive Stance";
    spells[71].attr     = 0x29050010u;
    spells[71].attrEx   = 0x10000400u; // SPELL_ATTR_EX_NO_AURA_ICON (no BuffFrame icon, still logged in FCT)
    spells[12970].name  = "Flurry";
    spells[12970].attr  = 0x00040000u;
    spells[25174].name  = "Sundering Cleave";
    spells[25174].attr  = 0x00040014u; // AQ20 Scarab Terrace debuff
    spells[32025].name  = "Test Buff (DND)";
    spells[32025].attr  = 0x00010000u; // "(DND)" in name without 0x80/0x100 -> must NOT be string-filtered

    const uint16_t auraBase = wowee::game::fieldIndex(UF::UNIT_FIELD_AURAS);

    // 1. Gain + fade via SMSG_SET_EXTRA_AURA_INFO / SMSG_CLEAR_EXTRA_AURA_INFO
    h.session.receive(buildSetExtraAuraInfo(Session::kPlayerGuid, 1, 33380, 5000, 5000));
    h.session.receive(buildSetExtraAuraInfo(Session::kPlayerGuid, 2, 5320, 10000, 10000));
    h.session.receive(buildSetExtraAuraInfo(Session::kPlayerGuid, 3, 433, 30000, 30000));
    h.session.receive(buildSetExtraAuraInfo(Session::kPlayerGuid, 4, 29479, 10000, 10000));
    h.session.receive(buildClearExtraAuraInfo(Session::kPlayerGuid, 33380));
    h.session.receive(buildClearExtraAuraInfo(Session::kPlayerGuid, 5320));
    h.session.receive(buildClearExtraAuraInfo(Session::kPlayerGuid, 433));
    h.session.receive(buildClearExtraAuraInfo(Session::kPlayerGuid, 29479));
    CHECK(h.recorded().empty());

    // 2. Gain + fade via UNIT_FIELD_AURAS VALUES updates (suppressed spells stay silent;
    //    real visible/AQ20/stance/unflagged spells fire START & END)
    h.session.receive(UpdateObject().tbc().valuesAt(
        Session::kPlayerGuid,
        {{static_cast<uint16_t>(auraBase + 1), 33380},
         {static_cast<uint16_t>(auraBase + 2), 433},
         {static_cast<uint16_t>(auraBase + 3), 71},
         {static_cast<uint16_t>(auraBase + 4), 12970},
         {static_cast<uint16_t>(auraBase + 5), 32025},
         {static_cast<uint16_t>(auraBase + 40), 25174}}));
    auto msgs = h.recorded();
    REQUIRE(msgs.size() == 4);
    CHECK(msgs[0].text == "<Defensive Stance>");
    CHECK(msgs[1].text == "<Flurry>");
    CHECK(msgs[2].text == "<Test Buff (DND)>");
    CHECK(msgs[3].text == "<Sundering Cleave>");
    CHECK(msgs[3].r == Catch::Approx(1.0));

    h.clearRecorded();
    h.session.receive(UpdateObject().tbc().valuesAt(
        Session::kPlayerGuid,
        {{static_cast<uint16_t>(auraBase + 1), 0},
         {static_cast<uint16_t>(auraBase + 2), 0},
         {static_cast<uint16_t>(auraBase + 3), 0},
         {static_cast<uint16_t>(auraBase + 4), 0},
         {static_cast<uint16_t>(auraBase + 5), 0},
         {static_cast<uint16_t>(auraBase + 40), 0}}));
    msgs = h.recorded();
    REQUIRE(msgs.size() == 4);
    CHECK(msgs[0].text == "<Defensive Stance> fades");
    CHECK(msgs[1].text == "<Flurry> fades");
    CHECK(msgs[2].text == "<Test Buff (DND)> fades");
    CHECK(msgs[3].text == "<Sundering Cleave> fades");
    CHECK(msgs[3].r == Catch::Approx(1.0));
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
    CHECK(msgs[7].text == "-200  (50 resisted)");
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

TEST_CASE("Healing & HoT combat text routes self-heals to 2D Blizzard_CombatText only and target heals to 3D world only", "[fct][framexml]") {
    FctHarness h(/*warrior*/ 1, /*rage*/ 1);
    // Spawn friendly ally for target heals
    h.session.receive(UpdateObject().tbc().create(
        kHealerGuid, ObjectType::PLAYER,
        {{UF::UNIT_FIELD_HEALTH, 3000},
         {UF::UNIT_FIELD_MAXHEALTH, 5000},
         {UF::UNIT_FIELD_LEVEL, 70}}));

    auto countUnitCombat = [&](const std::string& unit, const std::string& action) {
        int count = 0;
        for (const auto& [ev, args] : h.rawEvents) {
            if (ev == "UNIT_COMBAT" && args.size() >= 2 && args[0] == unit && args[1] == action)
                ++count;
        }
        return count;
    };

    // 1. Self direct heal (SMSG_SPELLHEALLOG caster == player, target == player):
    //    - 2D Blizzard_CombatText shows "+1500" ONCE
    //    - 3D world CombatUI::renderCombatText skips it (showsForUnit == false, NO duplicate!)
    //    - Portrait UNIT_COMBAT("player", "HEAL") fires ONCE
    Packet selfHeal(wireOpcode(Opcode::SMSG_SPELLHEALLOG));
    selfHeal.writePackedGuid(Session::kPlayerGuid);
    selfHeal.writePackedGuid(Session::kPlayerGuid);
    selfHeal.writeUInt32(23881); // Bloodthirst heal
    selfHeal.writeUInt32(1500);
    selfHeal.writeUInt8(0);      // normal
    selfHeal.writeUInt8(0);
    h.session.receive(selfHeal);

    auto msgs = h.recorded();
    REQUIRE(msgs.size() == 1);
    CHECK(msgs[0].text == "+1500");
    CHECK(msgs[0].r == Catch::Approx(0.1));
    CHECK(msgs[0].g == Catch::Approx(1.0));
    CHECK(msgs[0].b == Catch::Approx(0.1));
    CHECK(countUnitCombat("player", "HEAL") == 1);
    REQUIRE(h.session.handler().getCombatText().size() == 1);
    CHECK_FALSE(wowee::ui::fct::showsForUnit(
        h.session.handler().getCombatText().back(), Session::kPlayerGuid, 0));

    // 2. Self HoT tick (SMSG_PERIODICAURALOG SPELL_AURA_OBS_MOD_HEALTH = 20, e.g. Warrior Blood Craze +100000):
    //    - 2D Blizzard_CombatText shows "+100000" ONCE
    //    - 3D world CombatUI::renderCombatText skips it (showsForUnit == false, fixing duplicate +100000!)
    //    - Portrait UNIT_COMBAT does NOT fire on HoT ticks!
    h.clearRecorded();
    h.session.handler().clearCombatText();
    Packet selfHot(wireOpcode(Opcode::SMSG_PERIODICAURALOG));
    selfHot.writePackedGuid(Session::kPlayerGuid);
    selfHot.writePackedGuid(Session::kPlayerGuid);
    selfHot.writeUInt32(16491);  // Blood Craze
    selfHot.writeUInt32(1);      // count
    selfHot.writeUInt32(20);     // SPELL_AURA_OBS_MOD_HEALTH
    selfHot.writeUInt32(100000); // +100000 tick
    h.session.receive(selfHot);

    msgs = h.recorded();
    REQUIRE(msgs.size() == 1);
    CHECK(msgs[0].text == "+100000");
    CHECK(msgs[0].r == Catch::Approx(0.1));
    CHECK(msgs[0].g == Catch::Approx(1.0));
    CHECK(msgs[0].b == Catch::Approx(0.1));
    CHECK(countUnitCombat("player", "HEAL") == 0);
    REQUIRE(h.session.handler().getCombatText().size() == 1);
    CHECK_FALSE(wowee::ui::fct::showsForUnit(
        h.session.handler().getCombatText().back(), Session::kPlayerGuid, 0));

    // 3. Outgoing direct crit heal & HoT on friendly target (caster == player, target == kHealerGuid):
    //    - 2D Blizzard_CombatText shows NOTHING
    //    - 3D world CombatUI::renderCombatText shows "+2400" (crit) and "+450" (HoT) in (0.1, 1.0, 0.1) green
    //    - Direct heal fires UNIT_COMBAT("target", "HEAL", "CRITICAL"), HoT fires ZERO UNIT_COMBAT
    h.clearRecorded();
    h.session.handler().clearCombatText();
    h.session.handler().setTargetGuidRaw(kHealerGuid);

    Packet allyCritHeal(wireOpcode(Opcode::SMSG_SPELLHEALLOG));
    allyCritHeal.writePackedGuid(kHealerGuid);
    allyCritHeal.writePackedGuid(Session::kPlayerGuid);
    allyCritHeal.writeUInt32(2060); // Greater Heal
    allyCritHeal.writeUInt32(2400);
    allyCritHeal.writeUInt8(1);     // crit
    allyCritHeal.writeUInt8(0);
    h.session.receive(allyCritHeal);

    Packet allyHot(wireOpcode(Opcode::SMSG_PERIODICAURALOG));
    allyHot.writePackedGuid(kHealerGuid);
    allyHot.writePackedGuid(Session::kPlayerGuid);
    allyHot.writeUInt32(139);   // Renew
    allyHot.writeUInt32(1);
    allyHot.writeUInt32(8);     // SPELL_AURA_PERIODIC_HEAL
    allyHot.writeUInt32(450);
    h.session.receive(allyHot);

    CHECK(h.recorded().empty());
    CHECK(countUnitCombat("target", "HEAL") == 1);
    const auto& worldEntries = h.session.handler().getCombatText();
    REQUIRE(worldEntries.size() == 2);
    CHECK(wowee::ui::fct::showsForUnit(worldEntries[0], Session::kPlayerGuid, 0));
    CHECK(wowee::ui::fct::showsForUnit(worldEntries[1], Session::kPlayerGuid, 0));
    CHECK(wowee::ui::fct::presentationFor(worldEntries[0]).critical);
    CHECK(wowee::ui::fct::presentationFor(worldEntries[0]).heal);
    CHECK_FALSE(wowee::ui::fct::presentationFor(worldEntries[1]).critical);
    CHECK(wowee::ui::fct::presentationFor(worldEntries[1]).heal);
}

TEST_CASE("Melee vs spell Dodge/Resist/Block colors, portrait UNIT_COMBAT, and TBC CVar defaults match 2.4.3", "[fct][framexml]") {
    FctHarness h(/*warrior*/ 1, /*rage*/ 1);
    h.session.handler().spellNameCacheRef()[133].name = "Fireball";
    h.session.handler().spellNameCacheRef()[133].schoolMask = 0x04; // Fire
    h.session.handler().spellNameCacheRef()[686].name = "Shadow Bolt";
    h.session.handler().spellNameCacheRef()[686].schoolMask = 0x20; // Shadow

    // 1. Incoming melee dodge:
    //    Blizzard_CombatText "DODGE" -> red (1.0, 0.1, 0.1) + UNIT_COMBAT("player", "DODGE")
    h.clearRecorded();
    h.session.handler().addCombatText(
        CombatTextEntry::DODGE, 0, 0, false, 0, kMobGuid, Session::kPlayerGuid, 1, false, true);

    // 2. Incoming spell dodge & full spell resist via SMSG_SPELLLOGMISS:
    //    SPELL_DODGE -> white (1.0, 1.0, 1.0) + UNIT_COMBAT("player", "DODGE")
    //    SPELL_RESIST -> UNIT_COMBAT("player", "RESIST", "", "0", "4") (portrait "Resist" as seen on Legless!)
    Packet spellMiss(wireOpcode(Opcode::SMSG_SPELLLOGMISS));
    spellMiss.writeUInt32(133); // Fireball
    spellMiss.writeUInt64(kMobGuid);
    spellMiss.writeUInt8(0);
    spellMiss.writeUInt32(2);   // 2 miss entries
    spellMiss.writeUInt64(Session::kPlayerGuid);
    spellMiss.writeUInt8(wowee::game::SpellMissInfo::DODGE);  // 3 = SPELL_MISS_DODGE
    spellMiss.writeUInt64(Session::kPlayerGuid);
    spellMiss.writeUInt8(wowee::game::SpellMissInfo::RESIST); // 2 = SPELL_MISS_RESIST
    h.session.receive(spellMiss);

    // 3. Spell IMMUNE (SMSG_SPELLLOGMISS SPELL_MISS_IMMUNE = 7):
    //    SPELL_IMMUNE -> white "Immune" in Blizzard_CombatText, ZERO UNIT_COMBAT!
    Packet spellImmune(wireOpcode(Opcode::SMSG_SPELLLOGMISS));
    spellImmune.writeUInt32(686); // Shadow Bolt
    spellImmune.writeUInt64(Session::kPlayerGuid);
    spellImmune.writeUInt8(0);
    spellImmune.writeUInt32(1);
    spellImmune.writeUInt64(kMobGuid);
    spellImmune.writeUInt8(7);    // SPELL_MISS_IMMUNE
    h.session.receive(spellImmune);

    // 4. Partial spell block via TBC SMSG_SPELLNONMELEEDAMAGELOG (data.blocked = 40 on 110 hit):
    Packet spellBlock(wireOpcode(Opcode::SMSG_SPELLNONMELEEDAMAGELOG));
    spellBlock.writePackedGuid(Session::kPlayerGuid);
    spellBlock.writePackedGuid(kMobGuid);
    spellBlock.writeUInt32(78);   // Heroic Strike
    spellBlock.writeUInt32(110);  // damage
    spellBlock.writeUInt8(0x01);  // physical schoolMask
    spellBlock.writeUInt32(0);    // absorbed
    spellBlock.writeUInt32(0);    // resisted
    spellBlock.writeUInt8(0);     // periodicLog
    spellBlock.writeUInt8(0);     // unused
    spellBlock.writeUInt32(40);   // blocked!
    spellBlock.writeUInt32(0);    // flags
    h.session.receive(spellBlock);

    auto msgs = h.recorded();
    REQUIRE(msgs.size() == 4);
    CHECK(msgs[0].text == "Dodge");
    CHECK(msgs[0].r == Catch::Approx(1.0));
    CHECK(msgs[0].g == Catch::Approx(0.1));
    CHECK(msgs[0].b == Catch::Approx(0.1));
    CHECK(msgs[1].text == "Dodge");
    CHECK(msgs[1].r == Catch::Approx(1.0));
    CHECK(msgs[1].g == Catch::Approx(1.0));
    CHECK(msgs[1].b == Catch::Approx(1.0));
    CHECK(msgs[2].text == "-110");
    CHECK(msgs[3].text == "-110  (40 blocked)");

    bool sawPlayerSpellResistPortrait = false;
    bool sawTargetSpellImmunePortrait = false;
    for (const auto& [ev, args] : h.rawEvents) {
        if (ev == "UNIT_COMBAT" && args.size() >= 5) {
            if (args[0] == "player" && args[1] == "RESIST" && args[3] == "0" && args[4] == "4")
                sawPlayerSpellResistPortrait = true;
            if (args[0] == "target" && args[1] == "IMMUNE")
                sawTargetSpellImmunePortrait = true;
        }
    }
    CHECK(sawPlayerSpellResistPortrait);
    CHECK_FALSE(sawTargetSpellImmunePortrait);

    // 5. TBC 2.4.3 CVar defaults match GetCVarDefault on build 8606:
    REQUIRE(h.lua.executeString(R"lua(
        __cv_ect = tostring(GetCVar("enableCombatText"))
        __cv_dpm = tostring(GetCVar("fctDodgeParryMiss"))
        __cv_lmh = tostring(GetCVar("fctLowManaHealth"))
        __cv_aur = tostring(GetCVar("fctAuras"))
        __cv_cd  = tostring(GetCVar("CombatDamage"))
        __cv_ch  = tostring(GetCVar("CombatHealing"))
        __cv_clp = tostring(GetCVar("CombatLogPeriodicSpells"))
        __cv_pmd = tostring(GetCVar("PetMeleeDamage"))
        __cv_psd = tostring(GetCVar("PetSpellDamage"))
    )lua"));
    CHECK(h.lua.globalString("__cv_ect") == "0");
    CHECK(h.lua.globalString("__cv_dpm") == "0");
    CHECK(h.lua.globalString("__cv_lmh") == "0");
    CHECK(h.lua.globalString("__cv_aur") == "0");
    CHECK(h.lua.globalString("__cv_cd")  == "1");
    CHECK(h.lua.globalString("__cv_ch")  == "1");
    CHECK(h.lua.globalString("__cv_clp") == "1");
    CHECK(h.lua.globalString("__cv_pmd") == "1");
    CHECK(h.lua.globalString("__cv_psd") == "1");
}

TEST_CASE("SMSG_ENVIRONMENTALDAMAGELOG fires UNIT_COMBAT with physical schoolMask=1 for Fall/Drowning/Exhausted (white portrait) and elemental masks for Lava/Slime/Fire (yellow portrait)", "[fct][framexml][env]") {
    using namespace wowee::game;
    CHECK(environmentalDamageSchoolMask(0) == 0x01u); // Exhausted -> Physical
    CHECK(environmentalDamageSchoolMask(1) == 0x01u); // Drowning  -> Physical
    CHECK(environmentalDamageSchoolMask(2) == 0x01u); // Fall      -> Physical
    CHECK(environmentalDamageSchoolMask(3) == 0x04u); // Lava      -> Fire
    CHECK(environmentalDamageSchoolMask(4) == 0x08u); // Slime     -> Nature
    CHECK(environmentalDamageSchoolMask(5) == 0x04u); // Fire      -> Fire

    FctHarness h(/*warrior*/ 1, /*rage*/ 1);

    auto sendEnv = [&](uint64_t victim, uint8_t envType, uint32_t dmg, uint32_t abs = 0, uint32_t res = 0) {
        Packet pkt(wireOpcode(Opcode::SMSG_ENVIRONMENTAL_DAMAGE_LOG));
        pkt.writeUInt64(victim);
        pkt.writeUInt8(envType);
        pkt.writeUInt32(dmg);
        pkt.writeUInt32(abs);
        pkt.writeUInt32(res);
        h.session.receive(pkt);
    };

    const uint64_t self = Session::kPlayerGuid;
    h.rawEvents.clear();
    sendEnv(self, 2, 234); // Fall
    sendEnv(self, 1, 150); // Drowning
    sendEnv(self, 0, 120); // Exhausted
    sendEnv(self, 3, 300); // Lava
    sendEnv(self, 4, 200); // Slime
    sendEnv(self, 5, 180); // Fire
    sendEnv(self, 2, 0, 250, 0); // Fully absorbed Fall

    std::vector<std::tuple<std::string, std::string, std::string, std::string, std::string>> uc;
    for (const auto& [ev, args] : h.rawEvents) {
        if (ev == "UNIT_COMBAT" && args.size() >= 5) {
            uc.emplace_back(args[0], args[1], args[2], args[3], args[4]);
        }
    }
    REQUIRE(uc.size() == 7);
    CHECK(uc[0] == std::make_tuple("player", "WOUND", "", "234", "1"));
    CHECK(uc[1] == std::make_tuple("player", "WOUND", "", "150", "1"));
    CHECK(uc[2] == std::make_tuple("player", "WOUND", "", "120", "1"));
    CHECK(uc[3] == std::make_tuple("player", "WOUND", "", "300", "4"));
    CHECK(uc[4] == std::make_tuple("player", "WOUND", "", "200", "8"));
    CHECK(uc[5] == std::make_tuple("player", "WOUND", "", "180", "4"));
    CHECK(uc[6] == std::make_tuple("player", "WOUND", "ABSORB", "0", "1"));
}

TEST_CASE("Incoming melee swings, on-next-swing Cleave, and normal ranged attacks match TBC 2.4.3 red Miss/Damage parity",
          "[fct][framexml][incoming]") {
    FctHarness h(/*warrior*/ 1, /*rage*/ 1);
    auto& spells = h.session.handler().spellNameCacheRef();
    spells[75].name = "Auto Shot";
    spells[75].attrEx3 = wowee::game::kSpellAttrEx3NormalRangedAttack;
    spells[75].schoolMask = 0x01;
    spells[5019].name = "Shoot";
    spells[5019].attrEx3 = wowee::game::kSpellAttrEx3NormalRangedAttack;
    spells[5019].schoolMask = 0x01;
    spells[6660].name = "Shoot";
    spells[6660].attrEx3 = 0u;  // Creature Shoot (no SPELL_ATTR_EX3_NORMAL_RANGED_ATTACK)
    spells[6660].schoolMask = 0x01;
    spells[15496].name = "Cleave";
    spells[15496].attr = 0x00040014u;
    spells[15496].schoolMask = 0x01;
    spells[78].name = "Heroic Strike";
    spells[78].attr = 0x00040014u;
    spells[78].schoolMask = 0x01;

    auto makeMeleeSwing = [](uint64_t attacker, uint64_t target, uint32_t hitInfo,
                             uint32_t victimState, int32_t damage = 0) {
        Packet p(wireOpcode(Opcode::SMSG_ATTACKERSTATEUPDATE));
        p.writeUInt32(hitInfo);
        p.writePackedGuid(attacker);
        p.writePackedGuid(target);
        p.writeUInt32(static_cast<uint32_t>(damage));
        p.writeUInt8(1);
        p.writeUInt32(1);
        p.writeFloat(static_cast<float>(damage));
        p.writeUInt32(static_cast<uint32_t>(damage));
        p.writeUInt32(0);
        p.writeUInt32(0);
        p.writeUInt32(victimState);
        p.writeUInt32(0);
        p.writeUInt32(0);
        p.writeUInt32(0);
        return p;
    };

    auto makeSpellDamage = [](uint64_t target, uint64_t attacker, uint32_t spellId,
                              uint32_t damage, bool isCrit = false) {
        Packet p(wireOpcode(Opcode::SMSG_SPELLNONMELEEDAMAGELOG));
        p.writePackedGuid(target);
        p.writePackedGuid(attacker);
        p.writeUInt32(spellId);
        p.writeUInt32(damage);
        p.writeUInt8(0x01);
        p.writeUInt32(0);
        p.writeUInt32(0);
        p.writeUInt8(0);
        p.writeUInt8(0);
        p.writeUInt32(0);
        p.writeUInt32(isCrit ? 0x02u : 0u);
        return p;
    };

    // 1. Incoming melee swing miss (0x12), on-next-swing Cleave miss (0x10012),
    //    dodge (state 2), parry (state 3), evade (0x10 + state 6), immune (state 7), deflect (state 8):
    //    all render in red (1.0, 0.1, 0.1) in Blizzard_CombatText when COMBAT_TEXT_SHOW_DODGE_PARRY_MISS == "1",
    //    and none draw in 3D world space over the player.
    h.clearRecorded();
    h.session.handler().clearCombatText();
    h.session.receive(makeMeleeSwing(kMobGuid, Session::kPlayerGuid, 0x12u, 0u));     // MISS
    h.session.receive(makeMeleeSwing(kMobGuid, Session::kPlayerGuid, 0x10012u, 0u));  // HITINFO_NOACTION | HITINFO_MISS
    h.session.receive(makeMeleeSwing(kMobGuid, Session::kPlayerGuid, 0x02u, 2u));     // DODGE
    h.session.receive(makeMeleeSwing(kMobGuid, Session::kPlayerGuid, 0x02u, 3u));     // PARRY
    h.session.receive(makeMeleeSwing(kMobGuid, Session::kPlayerGuid, 0x12u, 6u));     // EVADE (with HITINFO_MISS bit)
    h.session.receive(makeMeleeSwing(kMobGuid, Session::kPlayerGuid, 0x02u, 7u));     // IMMUNE
    h.session.receive(makeMeleeSwing(kMobGuid, Session::kPlayerGuid, 0x02u, 8u));     // DEFLECT

    auto msgs = h.recorded();
    REQUIRE(msgs.size() == 7);
    const char* expectedWords[7] = {"Miss", "Miss", "Dodge", "Parry", "Evade", "Immune", "Deflect"};
    for (size_t i = 0; i < 7; ++i) {
        CHECK(msgs[i].text == expectedWords[i]);
        CHECK(msgs[i].r == Catch::Approx(1.0));
        CHECK(msgs[i].g == Catch::Approx(0.1));
        CHECK(msgs[i].b == Catch::Approx(0.1));
    }
    for (const auto& entry : h.session.handler().getCombatText()) {
        CHECK_FALSE(wowee::ui::fct::showsForUnit(entry, Session::kPlayerGuid, 0));
    }

    // When COMBAT_TEXT_SHOW_DODGE_PARRY_MISS == "0", Blizzard_CombatText suppresses incoming Miss/Dodge/Parry
    // while portrait UNIT_COMBAT("player", "MISS") still fires.
    REQUIRE(h.lua.executeString("COMBAT_TEXT_SHOW_DODGE_PARRY_MISS = '0' CombatText_UpdateDisplayedMessages()"));
    h.clearRecorded();
    h.session.receive(makeMeleeSwing(kMobGuid, Session::kPlayerGuid, 0x12u, 0u));
    CHECK(h.recorded().empty());
    bool sawPortraitMiss = false;
    for (const auto& [ev, args] : h.rawEvents) {
        if (ev == "UNIT_COMBAT" && args.size() >= 2 && args[0] == "player" && args[1] == "MISS") {
            sawPortraitMiss = true;
        }
    }
    CHECK(sawPortraitMiss);
    REQUIRE(h.lua.executeString("COMBAT_TEXT_SHOW_DODGE_PARRY_MISS = '1' CombatText_UpdateDisplayedMessages()"));

    // 2. Incoming Normal Ranged Weapon Attacks (Auto Shot 75, Wand Shoot 5019 -> attrEx3 & 0x8000):
    //    emit red "DAMAGE" / "DAMAGE_CRIT" / "MISS" (1.0, 0.1, 0.1) and RANGE_DAMAGE / RANGE_MISSED,
    //    whereas Creature Shoot (6660, attrEx3 == 0) emits purple "SPELL_DAMAGE" (0.79, 0.3, 0.85)
    //    and white "SPELL_MISS" (1.0, 1.0, 1.0).
    h.clearRecorded();
    h.session.receive(makeSpellDamage(Session::kPlayerGuid, kMobGuid, 75, 35, false));
    h.session.receive(makeSpellDamage(Session::kPlayerGuid, kMobGuid, 5019, 33, true));
    h.session.receive(makeSpellDamage(Session::kPlayerGuid, kMobGuid, 6660, 65, false));

    Packet rangedMisses(wireOpcode(Opcode::SMSG_SPELLLOGMISS));
    rangedMisses.writeUInt32(75);
    rangedMisses.writeUInt64(kMobGuid);
    rangedMisses.writeUInt8(0);
    rangedMisses.writeUInt32(1);
    rangedMisses.writeUInt64(Session::kPlayerGuid);
    rangedMisses.writeUInt8(wowee::game::SpellMissInfo::MISS);
    h.session.receive(rangedMisses);

    Packet npcShootMiss(wireOpcode(Opcode::SMSG_SPELLLOGMISS));
    npcShootMiss.writeUInt32(6660);
    npcShootMiss.writeUInt64(kMobGuid);
    npcShootMiss.writeUInt8(0);
    npcShootMiss.writeUInt32(1);
    npcShootMiss.writeUInt64(Session::kPlayerGuid);
    npcShootMiss.writeUInt8(wowee::game::SpellMissInfo::MISS);
    h.session.receive(npcShootMiss);

    msgs = h.recorded();
    REQUIRE(msgs.size() == 5);
    // Auto Shot (75) normal hit -> red "-35"
    CHECK(msgs[0].text == "-35");
    CHECK(msgs[0].r == Catch::Approx(1.0));
    CHECK(msgs[0].g == Catch::Approx(0.1));
    CHECK(msgs[0].b == Catch::Approx(0.1));
    // Wand Shoot (5019) crit -> red "-33" crit
    CHECK(msgs[1].text == "-33");
    CHECK(msgs[1].displayType == "crit");
    CHECK(msgs[1].r == Catch::Approx(1.0));
    CHECK(msgs[1].g == Catch::Approx(0.1));
    // Creature Shoot (6660) hit -> purple "-65"
    CHECK(msgs[2].text == "-65");
    CHECK(msgs[2].r == Catch::Approx(0.79));
    CHECK(msgs[2].g == Catch::Approx(0.3));
    CHECK(msgs[2].b == Catch::Approx(0.85));
    // Auto Shot (75) miss -> red "Miss"
    CHECK(msgs[3].text == "Miss");
    CHECK(msgs[3].r == Catch::Approx(1.0));
    CHECK(msgs[3].g == Catch::Approx(0.1));
    // Creature Shoot (6660) miss -> white "Miss"
    CHECK(msgs[4].text == "Miss");
    CHECK(msgs[4].r == Catch::Approx(1.0));
    CHECK(msgs[4].g == Catch::Approx(1.0));
    CHECK(msgs[4].b == Catch::Approx(1.0));

    bool sawRangeDamage = false;
    bool sawRangeMissed = false;
    for (const auto& [ev, args] : h.rawEvents) {
        if (ev == "COMBAT_LOG_EVENT_UNFILTERED" && args.size() >= 2) {
            if (args[1] == "RANGE_DAMAGE") sawRangeDamage = true;
            if (args[1] == "RANGE_MISSED") sawRangeMissed = true;
        }
    }
    CHECK(sawRangeDamage);
    CHECK(sawRangeMissed);

    // 3. On-next-swing Cleave (15496 incoming) & Heroic Strike (78 outgoing):
    //    0-damage HITINFO_NOACTION (0x10002) swing carrier must NOT emit SWING_DAMAGE 0
    //    or draw a fake 3D world "Miss" over the mob!
    h.clearRecorded();
    h.session.handler().clearCombatText();
    h.session.receive(makeMeleeSwing(kMobGuid, Session::kPlayerGuid, 0x10002u, 1u, 0));
    h.session.receive(makeSpellDamage(Session::kPlayerGuid, kMobGuid, 15496, 599, false));
    h.session.receive(makeMeleeSwing(Session::kPlayerGuid, kMobGuid, 0x10002u, 1u, 0));
    h.session.receive(makeSpellDamage(kMobGuid, Session::kPlayerGuid, 78, 310, false));

    msgs = h.recorded();
    REQUIRE(msgs.size() == 1);
    CHECK(msgs[0].text == "-599");
    CHECK(msgs[0].r == Catch::Approx(0.79));

    for (const auto& [ev, args] : h.rawEvents) {
        if (ev == "COMBAT_LOG_EVENT_UNFILTERED" && args.size() >= 9 && args[1] == "SWING_DAMAGE") {
            CHECK(args[8] != "0");
        }
    }
    // Only the real 310 Heroic Strike spell damage appears over the mob (no fake "Miss" from 0-damage carrier!)
    std::vector<wowee::ui::fct::Presentation> worldShown;
    for (const auto& entry : h.session.handler().getCombatText()) {
        if (wowee::ui::fct::showsForUnit(entry, Session::kPlayerGuid, 0)) {
            worldShown.push_back(wowee::ui::fct::presentationFor(entry));
        }
    }
    REQUIRE(worldShown.size() == 1);
    CHECK(worldShown[0].kind == wowee::ui::fct::Presentation::Number);
    CHECK(worldShown[0].number == 310);
}

TEST_CASE("SMSG_PERIODICAURALOG periodic damage and absorbs emit red DAMAGE/ABSORB and portrait UNIT_COMBAT across all schools", "[fct][framexml][periodic]") {
    FctHarness h(/*warrior*/ 1, /*rage*/ 1);
    REQUIRE(h.lua.executeString("SetCVar('fctDamageReduction', '1'); COMBAT_TEXT_TYPE_INFO['ABSORB'].show = 1; COMBAT_TEXT_TYPE_INFO['RESIST'].show = 1"));

    auto sendPeriodicTick = [&](uint32_t spellId, uint32_t dmg, uint32_t schoolMask, uint32_t absorbed = 0, uint32_t resisted = 0) {
        Packet p(wireOpcode(Opcode::SMSG_PERIODICAURALOG));
        p.writePackedGuid(Session::kPlayerGuid);
        p.writePackedGuid(kMobGuid);
        p.writeUInt32(spellId);
        p.writeUInt32(1);          // count
        p.writeUInt32(3);          // SPELL_AURA_PERIODIC_DAMAGE
        p.writeUInt32(dmg);
        p.writeUInt32(schoolMask);
        p.writeUInt32(absorbed);
        p.writeUInt32(resisted);
        h.session.receive(p);
    };

    // 1. Physical bleed (19771 Serrated Bite, school 1), Shadow DoT (589 SW:P, school 32), Fire DoT (348 Immolate, school 4)
    sendPeriodicTick(19771, 240, 1);
    sendPeriodicTick(589, 85, 32);
    sendPeriodicTick(348, 62, 4);
    // 2. Full periodic absorb (PW:S absorbing Serrated Bite & SW:P) and full periodic resist
    sendPeriodicTick(19771, 0, 1, /*absorbed=*/240, 0);
    sendPeriodicTick(589, 0, 32, /*absorbed=*/85, 0);
    sendPeriodicTick(589, 0, 32, 0, /*resisted=*/85);

    const auto msgs = h.recorded();
    REQUIRE(msgs.size() == 6);
    // All three periodic ticks are red ("DAMAGE": r=1.0, g=0.1, b=0.1), never purple ("SPELL_DAMAGE": 0.79, 0.3, 0.85)
    CHECK(msgs[0].text == "-240");
    CHECK(msgs[0].r == Catch::Approx(1.0));
    CHECK(msgs[0].g == Catch::Approx(0.1));
    CHECK(msgs[0].b == Catch::Approx(0.1));
    CHECK(msgs[1].text == "-85");
    CHECK(msgs[1].r == Catch::Approx(1.0));
    CHECK(msgs[1].g == Catch::Approx(0.1));
    CHECK(msgs[1].b == Catch::Approx(0.1));
    CHECK(msgs[2].text == "-62");
    CHECK(msgs[2].r == Catch::Approx(1.0));
    CHECK(msgs[2].g == Catch::Approx(0.1));
    CHECK(msgs[2].b == Catch::Approx(0.1));
    // Periodic absorbs and resists are also red ("ABSORB" / "RESIST"), never purple ("SPELL_ABSORB" / "SPELL_RESIST")
    CHECK(msgs[3].text == "Absorb");
    CHECK(msgs[3].r == Catch::Approx(1.0));
    CHECK(msgs[3].g == Catch::Approx(0.1));
    CHECK(msgs[4].text == "Absorb");
    CHECK(msgs[4].r == Catch::Approx(1.0));
    CHECK(msgs[4].g == Catch::Approx(0.1));
    CHECK(msgs[5].text == "Resist");
    CHECK(msgs[5].r == Catch::Approx(1.0));
    CHECK(msgs[5].g == Catch::Approx(0.1));

    // Portrait UNIT_COMBAT fires WOUND with exact schoolMask for ticks, absorbs, and resists
    std::vector<std::vector<std::string>> playerUnitCombat;
    for (const auto& [ev, args] : h.rawEvents) {
        if (ev == "UNIT_COMBAT" && !args.empty() && args[0] == "player") {
            playerUnitCombat.push_back(args);
        }
    }
    REQUIRE(playerUnitCombat.size() == 6);
    CHECK(playerUnitCombat[0] == std::vector<std::string>{"player", "WOUND", "", "240", "1"});
    CHECK(playerUnitCombat[1] == std::vector<std::string>{"player", "WOUND", "", "85", "32"});
    CHECK(playerUnitCombat[2] == std::vector<std::string>{"player", "WOUND", "", "62", "4"});
    CHECK(playerUnitCombat[3] == std::vector<std::string>{"player", "WOUND", "ABSORB", "0", "1"});
    CHECK(playerUnitCombat[4] == std::vector<std::string>{"player", "WOUND", "ABSORB", "0", "32"});
    CHECK(playerUnitCombat[5] == std::vector<std::string>{"player", "WOUND", "RESIST", "0", "32"});
}
