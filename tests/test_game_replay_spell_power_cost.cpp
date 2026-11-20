// Spell power cost scaling, Lua API parity (GetSpellInfo, GetSpellPowerCost,
// IsUsableSpell, IsUsableAction), tooltip cost lines (SetSpellByID, SetAction,
// SetSpell, SetHyperlink), and spell cost modifiers across all TBC 2.4.3 power
// types (Mana, Rage, Focus, Energy, Happiness, Health).
//
// Values match real TBC 2.4.3 (build 8606) Spell.dbc entries:
//   - Heroic Strike (78):   PowerType=1 (Rage),   ManaCost=150 (15 Rage)
//   - Rend (772):           PowerType=1 (Rage),   ManaCost=100 (10 Rage)
//   - Whirlwind (1680):     PowerType=1 (Rage),   ManaCost=250 (25 Rage)
//   - Thunder Clap (6343):  PowerType=1 (Rage),   ManaCost=200 (20 Rage)
//   - Sinister Strike (1752): PowerType=3 (Energy), ManaCost=45 (45 Energy)
//   - Fireball (133):       PowerType=0 (Mana),   ManaCost=30  (30 Mana)
//   - Growl (2649):         PowerType=2 (Focus),  ManaCost=15  (15 Focus)
//   - Happiness test (99901): PowerType=4 (Happiness), ManaCost=20 (20 Happiness)
//   - Health Funnel (755):  PowerType=-2 (254/Health), ManaCost=11, ManaPerSecond=5
//   - Crusader Strike (35395): PowerType=0 (Mana), ManaCost=0, ManaCostPercentage=8
#include <catch_amalgamated.hpp>

#include "addons/lua_engine.hpp"
#include "game/item_text.hpp"
#include "game/spell_defines.hpp"
#include "game/spell_prerequisites.hpp"
#include "game_replay.hpp"

#include <string>
#include <vector>

using replay::Session;
using replay::UpdateObject;
using wowee::game::GameHandler;
using wowee::game::ObjectType;
using wowee::game::Opcode;
using wowee::game::UF;
using wowee::game::wireOpcode;
using wowee::network::Packet;

namespace {

constexpr uint32_t kHeroicStrike   = 78;
constexpr uint32_t kRend           = 772;
constexpr uint32_t kWhirlwind      = 1680;
constexpr uint32_t kThunderClap    = 6343;
constexpr uint32_t kSinisterStrike = 1752;
constexpr uint32_t kFireball       = 133;
constexpr uint32_t kGrowl          = 2649;
constexpr uint32_t kHappinessSpell = 99901;
constexpr uint32_t kHealthFunnel   = 755;
constexpr uint32_t kCrusaderStrike = 35395;

void registerTbcSpells(Session& s) {
    auto& gh = s.handler();
    auto& cache = gh.spellNameCacheRef();
    struct Def {
        uint32_t id;
        const char* name;
        const char* rank;
        uint64_t familyFlags;
    };
    for (const Def& d : {
             Def{kHeroicStrike,   "Heroic Strike",   "Rank 1", 64ull},
             Def{kRend,           "Rend",            "Rank 1", 32ull},
             Def{kWhirlwind,      "Whirlwind",       "",       0ull},
             Def{kThunderClap,    "Thunder Clap",    "Rank 1", 128ull},
             Def{kSinisterStrike, "Sinister Strike", "Rank 1", 0ull},
             Def{kFireball,       "Fireball",        "Rank 1", 1ull},
             Def{kGrowl,          "Growl",           "Rank 1", 0ull},
             Def{kHappinessSpell, "Pet Treat",       "",       0ull},
             Def{kHealthFunnel,   "Health Funnel",   "Rank 1", 0ull},
             Def{kCrusaderStrike, "Crusader Strike", "",       0ull},
         }) {
        auto& e = cache[d.id];
        e.name = d.name;
        e.rank = d.rank;
        e.familyFlags = d.familyFlags;
        Packet learned(wireOpcode(Opcode::SMSG_LEARNED_SPELL));
        learned.writeUInt32(d.id);
        s.receive(learned);
    }
    gh.spellNameCacheLoadedRef() = true;

    gh.setSpellDataResolver([](uint32_t id) -> GameHandler::SpellDataInfo {
        GameHandler::SpellDataInfo info;
        switch (id) {
            case kHeroicStrike:
                info.powerType = 1; info.manaCost = 150; info.maxRange = 5.0f;
                break;
            case kRend:
                info.powerType = 1; info.manaCost = 100; info.maxRange = 5.0f;
                break;
            case kWhirlwind:
                info.powerType = 1; info.manaCost = 250;
                break;
            case kThunderClap:
                info.powerType = 1; info.manaCost = 200;
                break;
            case kSinisterStrike:
                info.powerType = 3; info.manaCost = 45; info.maxRange = 5.0f;
                break;
            case kFireball:
                info.powerType = 0; info.manaCost = 30; info.castTimeMs = 1500; info.maxRange = 35.0f;
                break;
            case kGrowl:
                info.powerType = 2; info.manaCost = 15; info.maxRange = 5.0f;
                break;
            case kHappinessSpell:
                info.powerType = 4; info.manaCost = 20;
                break;
            case kHealthFunnel:
                info.powerType = wowee::game::kPowerHealth;
                info.manaCost = 11; info.manaPerSecond = 5; info.maxRange = 20.0f;
                break;
            case kCrusaderStrike:
                info.powerType = 0; info.manaCost = 0; info.manaCostPct = 8; info.maxRange = 5.0f;
                break;
            default:
                break;
        }
        return info;
    });
}

void spawnPlayerWithPowers(Session& s, uint32_t health, uint32_t mana, uint32_t baseMana,
                           uint32_t rawRage, uint32_t focus, uint32_t energy, uint32_t happiness) {
    s.receive(UpdateObject().tbc().create(
        Session::kPlayerGuid, ObjectType::PLAYER,
        {
            {UF::UNIT_FIELD_HEALTH, health},
            {UF::UNIT_FIELD_MAXHEALTH, 1000},
            {UF::UNIT_FIELD_BASE_MANA, baseMana},
            {UF::UNIT_FIELD_POWER1, mana},
            {UF::UNIT_FIELD_MAXPOWER1, baseMana},
            {UF::UNIT_FIELD_LEVEL, 70},
        },
        true));
    auto* unit = s.handler().getUnitByGuid(Session::kPlayerGuid);
    REQUIRE(unit != nullptr);
    unit->setPowerByType(0, mana);
    unit->setMaxPowerByType(0, baseMana);
    unit->setPowerByType(1, rawRage);
    unit->setMaxPowerByType(1, 1000);
    unit->setPowerByType(2, focus);
    unit->setMaxPowerByType(2, 100);
    unit->setPowerByType(3, energy);
    unit->setMaxPowerByType(3, 100);
    unit->setPowerByType(4, happiness);
    unit->setMaxPowerByType(4, 1000);
}

std::string evalString(wowee::addons::LuaEngine& lua, const char* expr) {
    REQUIRE(lua.executeString(std::string("__out = tostring(") + expr + ")"));
    return lua.globalString("__out");
}

}  // namespace

TEST_CASE("displayPowerCost scales Rage and Runic Power by 10 and preserves other power types",
          "[spell][power-cost]") {
    CHECK(wowee::game::displayPowerCost(0, 30) == 30);   // Mana
    CHECK(wowee::game::displayPowerCost(1, 150) == 15);  // Rage (Heroic Strike)
    CHECK(wowee::game::displayPowerCost(1, 100) == 10);  // Rage (Rend)
    CHECK(wowee::game::displayPowerCost(1, 250) == 25);  // Rage (Whirlwind)
    CHECK(wowee::game::displayPowerCost(1, 120) == 12);  // Rage with -30 flat mod
    CHECK(wowee::game::displayPowerCost(2, 15) == 15);   // Focus
    CHECK(wowee::game::displayPowerCost(3, 45) == 45);   // Energy
    CHECK(wowee::game::displayPowerCost(4, 20) == 20);   // Happiness
    CHECK(wowee::game::displayPowerCost(6, 200) == 20);  // Runic Power
    CHECK(wowee::game::displayPowerCost(wowee::game::kPowerHealth, 11) == 11);

    CHECK(std::string(wowee::game::powerTypeName(0)) == "Mana");
    CHECK(std::string(wowee::game::powerTypeName(1)) == "Rage");
    CHECK(std::string(wowee::game::powerTypeName(2)) == "Focus");
    CHECK(std::string(wowee::game::powerTypeName(3)) == "Energy");
    CHECK(std::string(wowee::game::powerTypeName(4)) == "Happiness");
    CHECK(std::string(wowee::game::powerTypeName(6)) == "Runic Power");
    CHECK(std::string(wowee::game::powerTypeName(wowee::game::kPowerHealth)) == "Health");
}

TEST_CASE("GetSpellInfo and GetSpellPowerCost return display costs across all TBC power types",
          "[replay][spell][power-cost]") {
    Session s("tbc");
    registerTbcSpells(s);
    spawnPlayerWithPowers(s, /*health=*/500, /*mana=*/1000, /*baseMana=*/1000,
                          /*rawRage=*/150, /*focus=*/50, /*energy=*/100, /*happiness=*/500);

    wowee::addons::LuaEngine lua;
    REQUIRE(lua.initialize());
    lua.setGameHandler(&s.handler());

    // GetSpellInfo returns: name, rank, icon, cost, isFunnel, powerType, castTime, minRange, maxRange
    const auto costAndType = [&](uint32_t spellId) -> std::string {
        const std::string code =
            "local n, r, ic, cost, f, pt = GetSpellInfo(" + std::to_string(spellId) + "); "
            "__out = tostring(cost) .. ':' .. tostring(pt)";
        REQUIRE(lua.executeString(code));
        return lua.globalString("__out");
    };

    CHECK(costAndType(kHeroicStrike)   == "15:1");
    CHECK(costAndType(kRend)           == "10:1");
    CHECK(costAndType(kWhirlwind)      == "25:1");
    CHECK(costAndType(kThunderClap)    == "20:1");
    CHECK(costAndType(kSinisterStrike) == "45:3");
    CHECK(costAndType(kFireball)       == "30:0");
    CHECK(costAndType(kGrowl)          == "15:2");
    CHECK(costAndType(kHappinessSpell) == "20:4");
    CHECK(costAndType(kHealthFunnel)   == "11:-2");
    CHECK(costAndType(kCrusaderStrike) == "80:0"); // 8% of 1000 base mana

    // GetSpellPowerCost returns whole display points too.
    CHECK(evalString(lua, "GetSpellPowerCost(78)[1].cost") == "15");
    CHECK(evalString(lua, "GetSpellPowerCost(78)[1].name") == "RAGE");
    CHECK(evalString(lua, "GetSpellPowerCost(1752)[1].cost") == "45");
    CHECK(evalString(lua, "GetSpellPowerCost(1752)[1].name") == "ENERGY");
    CHECK(evalString(lua, "GetSpellPowerCost(755)[1].cost") == "11");
    CHECK(evalString(lua, "GetSpellPowerCost(755)[1].name") == "HEALTH");

    // Improved Heroic Strike Rank 3: flat -30 tenths on family bit 6 (mask 64) -> 12 Rage.
    Packet mod(wireOpcode(Opcode::SMSG_SET_FLAT_SPELL_MODIFIER));
    mod.writeUInt8(6);  // SpellFamilyFlags bit 6 (1 << 6 == 64)
    mod.writeUInt8(static_cast<uint8_t>(GameHandler::SpellModOp::Cost));
    mod.writeUInt32(static_cast<uint32_t>(-30));
    s.receive(mod);

    CHECK(costAndType(kHeroicStrike) == "12:1");
    CHECK(evalString(lua, "GetSpellPowerCost(78)[1].cost") == "12");
}

TEST_CASE("Spell tooltip cost lines show whole Rage points and per-second health costs",
          "[replay][spell][power-cost][tooltip]") {
    Session s("tbc");
    registerTbcSpells(s);
    spawnPlayerWithPowers(s, /*health=*/500, /*mana=*/1000, /*baseMana=*/1000,
                          /*rawRage=*/150, /*focus=*/50, /*energy=*/100, /*happiness=*/500);

    wowee::addons::LuaEngine lua;
    REQUIRE(lua.initialize());
    lua.setGameHandler(&s.handler());

    // Place Heroic Strike on action slot 1 so SetAction exercises the action bar path.
    s.handler().actionBarRef()[0].type = wowee::game::ActionBarSlot::SPELL;
    s.handler().actionBarRef()[0].id   = kHeroicStrike;

    REQUIRE(lua.executeString(
        "MELEE_RANGE = 'Melee Range'\n"
        "SPELL_RANGE = '%s yd range'\n"
        "SPELL_CAST_TIME_INSTANT = 'Instant cast'\n"
        "SPELL_CAST_TIME_INSTANT_NO_MANA = 'Instant'\n"
        "TestTip = CreateFrame('GameTooltip', 'TestTip')\n"
        "for i = 1, 8 do\n"
        "    TestTip:CreateFontString('TestTipTextLeft' .. i)\n"
        "    TestTip:CreateFontString('TestTipTextRight' .. i)\n"
        "end\n"));

    const auto tooltipLine2 = [&](const std::string& call) -> std::string {
        REQUIRE(lua.executeString(
            "TestTip:ClearLines()\n" + call + "\n"
            "local fs = _G['TestTipTextLeft2']\n"
            "__out = fs and fs:GetText() or ''\n"));
        return lua.globalString("__out");
    };

    CHECK(tooltipLine2("TestTip:SetSpellByID(78)")          == "15 Rage");
    CHECK(tooltipLine2("TestTip:SetAction(1)")              == "15 Rage");
    CHECK(tooltipLine2("TestTip:SetHyperlink('spell:772')") == "10 Rage");
    CHECK(tooltipLine2("TestTip:SetSpellByID(1680)")        == "25 Rage");
    CHECK(tooltipLine2("TestTip:SetSpellByID(6343)")        == "20 Rage");
    CHECK(tooltipLine2("TestTip:SetSpellByID(1752)")        == "45 Energy");
    CHECK(tooltipLine2("TestTip:SetSpellByID(133)")         == "30 Mana");
    CHECK(tooltipLine2("TestTip:SetSpellByID(2649)")        == "15 Focus");
    CHECK(tooltipLine2("TestTip:SetSpellByID(99901)")       == "20 Happiness");
    CHECK(tooltipLine2("TestTip:SetSpellByID(755)")         == "11 Health, plus 5 per sec");
    CHECK(tooltipLine2("TestTip:SetSpellByID(35395)")       == "80 Mana");
}

TEST_CASE("IsUsableSpell and IsUsableAction check raw Rage tenths and Health cost accurately",
          "[replay][spell][power-cost][usability]") {
    Session s("tbc");
    registerTbcSpells(s);
    // 149 raw rage (14.9 rage): enough for Rend (100), NOT enough for Heroic Strike (150).
    // 10 health: NOT enough for Health Funnel (11).
    spawnPlayerWithPowers(s, /*health=*/10, /*mana=*/30, /*baseMana=*/1000,
                          /*rawRage=*/149, /*focus=*/15, /*energy=*/44, /*happiness=*/20);

    s.handler().actionBarRef()[0].type = wowee::game::ActionBarSlot::SPELL;
    s.handler().actionBarRef()[0].id   = kHeroicStrike;
    s.handler().actionBarRef()[1].type = wowee::game::ActionBarSlot::SPELL;
    s.handler().actionBarRef()[1].id   = kRend;

    wowee::addons::LuaEngine lua;
    REQUIRE(lua.initialize());
    lua.setGameHandler(&s.handler());

    const auto usableSpell = [&](uint32_t spellId) -> std::string {
        REQUIRE(lua.executeString(
            "local u, m = IsUsableSpell(" + std::to_string(spellId) + "); "
            "__out = tostring(u) .. ':' .. tostring(m)"));
        return lua.globalString("__out");
    };
    const auto usableAction = [&](int slot) -> std::string {
        REQUIRE(lua.executeString(
            "local u, m = IsUsableAction(" + std::to_string(slot) + "); "
            "__out = tostring(u) .. ':' .. tostring(m)"));
        return lua.globalString("__out");
    };

    CHECK(usableSpell(kHeroicStrike)   == "false:true");  // 149 < 150
    CHECK(usableAction(1)              == "false:true");  // Heroic Strike on slot 1
    CHECK(usableSpell(kRend)           == "true:false");  // 149 >= 100
    CHECK(usableAction(2)              == "true:false");  // Rend on slot 2
    CHECK(usableSpell(kSinisterStrike) == "false:true");  // 44 < 45
    CHECK(usableSpell(kFireball)       == "true:false");  // 30 >= 30
    CHECK(usableSpell(kGrowl)          == "true:false");  // 15 >= 15
    CHECK(usableSpell(kHappinessSpell) == "true:false");  // 20 >= 20
    CHECK(usableSpell(kHealthFunnel)   == "false:true");  // 10 < 11

    // Bump rage to 150 (15.0 rage) and health to 50 -> Heroic Strike & Health Funnel become usable.
    auto* unit = s.handler().getUnitByGuid(Session::kPlayerGuid);
    REQUIRE(unit != nullptr);
    unit->setPowerByType(1, 150);
    s.receive(UpdateObject().tbc().values(Session::kPlayerGuid, {
        {UF::UNIT_FIELD_HEALTH, 50},
    }));
    CHECK(usableSpell(kHeroicStrike) == "true:false");
    CHECK(usableAction(1)            == "true:false");
    CHECK(usableSpell(kHealthFunnel) == "true:false");
}
