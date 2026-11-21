// TBC 2.4.3 parity tests for:
// 1. GetComboPoints() class gating (Warrior Overpower proc -> 0 Lua combo points
//    while IsUsableSpell("Overpower") stays true; Rogue >= 1; Druid Cat/Caster/Bear >= 1).
// 2. SPELL_ATTR0_ON_NEXT_SWING (0x4) tooltip lines ("Next melee", no "Melee Range")
//    vs normal instant melee spells ("Melee Range" + "Instant").
// 3. Unmet vs met EquippedItem* weapon/armor tooltip line color (red when unarmed,
//    wrong subclass/slot, or broken durability==0; white when satisfied).
#include <catch_amalgamated.hpp>

#include "addons/lua_engine.hpp"
#include "game_replay.hpp"
#include "game/game_services.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>

using replay::Session;
using replay::UpdateObject;
using wowee::game::EquipSlot;
using wowee::game::GameHandler;
using wowee::game::GameServices;
using wowee::game::ItemDef;
using wowee::game::ItemQueryResponseData;
using wowee::game::ObjectType;
using wowee::game::Opcode;
using wowee::game::wireOpcode;
using wowee::network::Packet;
using UF = wowee::game::UF;

namespace {

constexpr uint64_t kMobA = 0xF130000000003001ull;
constexpr uint64_t kMobB = 0xF130000000003002ull;

constexpr uint32_t kBackstab       = 53u;
constexpr uint32_t kShieldBash     = 72u;
constexpr uint32_t kHeroicStrike   = 78u;
constexpr uint32_t kCleave         = 845u;
constexpr uint32_t kWhirlwind      = 1680u;
constexpr uint32_t kSinisterStrike = 1752u;
constexpr uint32_t kRaptorStrike   = 2973u;
constexpr uint32_t kMaul           = 6807u;
constexpr uint32_t kOverpowerR1    = 7384u;
constexpr uint32_t kOverpowerR4    = 11585u;
constexpr uint32_t kMortalStrike   = 12294u;
constexpr uint32_t kSteadyShot     = 34120u;

constexpr uint8_t kClassWarrior = 1;
constexpr uint8_t kClassRogue   = 4;
constexpr uint8_t kClassPriest  = 5;
constexpr uint8_t kClassDruid   = 11;

uint32_t packUnitBytes0(uint8_t race, uint8_t cls, uint8_t gender, uint8_t powerType) {
    return static_cast<uint32_t>(race) |
           (static_cast<uint32_t>(cls) << 8) |
           (static_cast<uint32_t>(gender) << 16) |
           (static_cast<uint32_t>(powerType) << 24);
}

Packet buildComboPoints(uint64_t target, uint8_t points) {
    Packet p(wireOpcode(Opcode::SMSG_UPDATE_COMBO_POINTS));
    p.writePackedGuid(target);
    p.writeUInt8(points);
    return p;
}

std::string evalString(wowee::addons::LuaEngine& lua, const std::string& expr) {
    REQUIRE(lua.executeString("__out = tostring(" + expr + ")"));
    return lua.globalString("__out");
}

void spawnPlayerWithClass(Session& s, uint8_t cls, uint8_t powerType) {
    s.receive(UpdateObject().tbc().create(
        Session::kPlayerGuid, ObjectType::PLAYER,
        {{UF::UNIT_FIELD_HEALTH, 2000},
         {UF::UNIT_FIELD_MAXHEALTH, 2000},
         {UF::UNIT_FIELD_LEVEL, 70},
         {UF::UNIT_FIELD_BYTES_0, packUnitBytes0(1, cls, 0, powerType)},
         {UF::UNIT_FIELD_POWER1, 1000},
         {UF::UNIT_FIELD_MAXPOWER1, 1000}},
        true));
    auto* unit = s.handler().getUnitByGuid(Session::kPlayerGuid);
    REQUIRE(unit != nullptr);
    unit->setPowerByType(0, 1000);
    unit->setMaxPowerByType(0, 1000);
    unit->setPowerByType(1, 500);
    unit->setMaxPowerByType(1, 1000);
    unit->setPowerByType(3, 100);
    unit->setMaxPowerByType(3, 100);
    s.receive(UpdateObject().tbc().create(
        kMobA, ObjectType::UNIT,
        {{UF::UNIT_FIELD_HEALTH, 1000}, {UF::UNIT_FIELD_MAXHEALTH, 1000}, {UF::UNIT_FIELD_LEVEL, 70}}));
    s.receive(UpdateObject().tbc().create(
        kMobB, ObjectType::UNIT,
        {{UF::UNIT_FIELD_HEALTH, 1000}, {UF::UNIT_FIELD_MAXHEALTH, 1000}, {UF::UNIT_FIELD_LEVEL, 70}}));
    s.handler().setTargetGuidRaw(kMobA);
}

void setClassBytes(Session& s, uint8_t cls, uint8_t powerType) {
    s.receive(UpdateObject().tbc().values(
        Session::kPlayerGuid,
        {{UF::UNIT_FIELD_BYTES_0, packUnitBytes0(1, cls, 0, powerType)}}));
}

std::string findTbcDataDir() {
    if (const char* env = std::getenv("WOWEE_TBC_DATA_DIR"); env && *env && std::filesystem::is_directory(env))
        return env;
    const std::string fallback = "/workspace/azerothcore/client/wowee-data/expansions/tbc";
    if (std::filesystem::is_directory(fallback)) return fallback;
    return {};
}

ItemDef makeEquipItem(GameHandler& gh, uint32_t itemId, const std::string& name,
                      uint32_t itemClass, uint32_t subClass, uint32_t invType,
                      uint32_t curDur, uint32_t maxDur) {
    ItemQueryResponseData info{};
    info.entry = itemId;
    info.itemClass = itemClass;
    info.subClass = subClass;
    info.inventoryType = invType;
    info.name = name;
    info.maxDurability = maxDur;
    info.valid = true;
    gh.itemInfoCacheRef()[itemId] = info;

    ItemDef def{};
    def.itemId = itemId;
    def.name = name;
    def.stackCount = 1;
    def.curDurability = curDur;
    def.maxDurability = maxDur;
    def.inventoryType = static_cast<uint8_t>(invType);
    return def;
}

} // namespace

TEST_CASE("TBC GetComboPoints hides Warrior Overpower proc dots while preserving usability and Druid/Rogue points",
          "[replay][tbc][combo-points]") {
    Session s("tbc");
    auto& cache = s.handler().spellNameCacheRef();
    cache[kOverpowerR4].name = "Overpower";
    cache[kOverpowerR4].attr = 0x00250010u;
    cache[kOverpowerR4].attrEx = 0x48100200u; // FINISHING_MOVE_DAMAGE | COMBO_ON_BLOCK
    s.handler().spellNameCacheLoadedRef() = true;
    {
        Packet learned(wireOpcode(Opcode::SMSG_LEARNED_SPELL));
        learned.writeUInt32(kOverpowerR4);
        s.receive(learned);
    }

    spawnPlayerWithClass(s, kClassWarrior, /*Rage=*/1);

    wowee::addons::LuaEngine lua;
    REQUIRE(lua.initialize());
    lua.setGameHandler(&s.handler());

    // Warrior: before dodge -> Overpower unusable, GetComboPoints() == 0.
    CHECK(evalString(lua, "GetComboPoints()") == "0");
    CHECK(evalString(lua, "IsUsableSpell(11585)") == "false");

    // Target dodges -> SMSG_UPDATE_COMBO_POINTS(kMobA, 1).
    // Warrior Overpower lights up, but GetComboPoints() stays 0 so ComboFrame shows no red dot.
    s.receive(buildComboPoints(kMobA, 1));
    CHECK(evalString(lua, "GetComboPoints()") == "0");
    CHECK(evalString(lua, "IsUsableSpell(11585)") == "true");

    // Switching targets greys Overpower out; returning restores it; GetComboPoints() always 0.
    s.handler().setTargetGuidRaw(kMobB);
    CHECK(evalString(lua, "IsUsableSpell(11585)") == "false");
    CHECK(evalString(lua, "GetComboPoints()") == "0");
    s.handler().setTargetGuidRaw(kMobA);
    CHECK(evalString(lua, "IsUsableSpell(11585)") == "true");
    CHECK(evalString(lua, "GetComboPoints()") == "0");

    // Rogue: GetComboPoints() reports exact points on combo target, 0 on other targets.
    setClassBytes(s, kClassRogue, /*Energy=*/3);
    s.receive(buildComboPoints(kMobA, 3));
    CHECK(evalString(lua, "GetComboPoints()") == "3");
    s.handler().setTargetGuidRaw(kMobB);
    CHECK(evalString(lua, "GetComboPoints()") == "0");
    s.handler().setTargetGuidRaw(kMobA);
    CHECK(evalString(lua, "GetComboPoints()") == "3");

    // Druid: TBC 2.4.3 reports combo points across Cat, Caster, Bear, and Dire Bear forms.
    setClassBytes(s, kClassDruid, /*Energy=*/3);
    s.receive(buildComboPoints(kMobA, 2));
    s.handler().shapeshiftFormIdRef() = 1; // Cat Form
    CHECK(evalString(lua, "GetComboPoints()") == "2");
    setClassBytes(s, kClassDruid, /*Mana=*/0);
    s.handler().shapeshiftFormIdRef() = 0; // Caster Form
    CHECK(evalString(lua, "GetComboPoints()") == "2");
    setClassBytes(s, kClassDruid, /*Rage=*/1);
    s.handler().shapeshiftFormIdRef() = 5; // Bear Form
    CHECK(evalString(lua, "GetComboPoints()") == "2");
    s.handler().shapeshiftFormIdRef() = 8; // Dire Bear Form
    CHECK(evalString(lua, "GetComboPoints()") == "2");

    // Non-combo class (Priest) returns 0 even if in Cat Form powerType=Energy.
    setClassBytes(s, kClassPriest, /*Energy=*/3);
    s.handler().shapeshiftFormIdRef() = 1;
    CHECK(evalString(lua, "GetComboPoints()") == "0");
}

TEST_CASE("TBC ON_NEXT_SWING tooltips show Next melee without Melee Range and color unmet/broken weapon requirements red",
          "[replay][tbc][tooltip][warrior]") {
    const std::string tbcRoot = findTbcDataDir();
    if (tbcRoot.empty()) {
        SKIP("TBC extracted data directory unavailable");
    }

    Session s("tbc");
    auto& gh = s.handler();

    wowee::pipeline::AssetManager assets;
    REQUIRE(assets.initialize(tbcRoot));
    wowee::pipeline::DBCLayout dbcLayout;
    REQUIRE(dbcLayout.loadFromJson(tbcRoot + "/dbc_layouts.json"));
    auto* prevLayout = wowee::pipeline::getActiveDBCLayout();
    wowee::pipeline::setActiveDBCLayout(&dbcLayout);
    const_cast<GameServices&>(gh.services()).assetManager = &assets;

    struct LayoutGuard {
        const wowee::pipeline::DBCLayout* prev;
        GameHandler& handler;
        ~LayoutGuard() {
            const_cast<GameServices&>(handler.services()).assetManager = nullptr;
            wowee::pipeline::setActiveDBCLayout(prev);
        }
    } guard{prevLayout, gh};

    gh.loadSpellNameCache();
    gh.setSpellDataResolver([](uint32_t spellId) {
        GameHandler::SpellDataInfo info{};
        switch (spellId) {
            case kHeroicStrike:   info.powerType = 1; info.manaCost = 150; info.maxRange = 5.0f; break;
            case kCleave:         info.powerType = 1; info.manaCost = 200; info.maxRange = 5.0f; break;
            case kMaul:           info.powerType = 1; info.manaCost = 150; info.maxRange = 5.0f; break;
            case kRaptorStrike:   info.powerType = 0; info.manaCost = 15;  info.maxRange = 5.0f; break;
            case kMortalStrike:   info.powerType = 1; info.manaCost = 300; info.maxRange = 5.0f; break;
            case kOverpowerR1:    info.powerType = 1; info.manaCost = 50;  info.maxRange = 5.0f; break;
            case kSinisterStrike: info.powerType = 3; info.manaCost = 45;  info.maxRange = 5.0f; break;
            case kShieldBash:     info.powerType = 1; info.manaCost = 100; info.maxRange = 5.0f; break;
            case kWhirlwind:      info.powerType = 1; info.manaCost = 250; info.maxRange = 0.0f; break;
            case kBackstab:       info.powerType = 3; info.manaCost = 60;  info.maxRange = 5.0f; break;
            case kSteadyShot:     info.powerType = 0; info.manaCost = 110; info.castTimeMs = 1500; info.maxRange = 35.0f; break;
            default: break;
        }
        return info;
    });
    spawnPlayerWithClass(s, kClassWarrior, /*Rage=*/1);

    wowee::addons::LuaEngine lua;
    REQUIRE(lua.initialize());
    lua.setGameHandler(&gh);

    REQUIRE(lua.executeString(
        "MELEE_RANGE = 'Melee Range'\n"
        "SPELL_RANGE = '%s yd range'\n"
        "SPELL_ON_NEXT_SWING = 'Next melee'\n"
        "SPELL_CAST_TIME_INSTANT = 'Instant cast'\n"
        "SPELL_CAST_TIME_INSTANT_NO_MANA = 'Instant'\n"
        "SPELL_EQUIPPED_ITEM = 'Requires %s'\n"
        "SPELL_REQUIRED_FORM = 'Requires %s'\n"
        "SPELL_TOTEMS = 'Tools: '\n"
        "SPELL_REAGENTS = 'Reagents: '\n"
        "TestTip = CreateFrame('GameTooltip', 'TestTip')\n"
        "for i = 1, 8 do\n"
        "    TestTip:CreateFontString('TestTipTextLeft' .. i)\n"
        "    TestTip:CreateFontString('TestTipTextRight' .. i)\n"
        "end\n"));

    const auto tipLine = [&](uint32_t spellId, int idx) -> std::string {
        REQUIRE(lua.executeString(
            "TestTip:ClearLines()\n"
            "TestTip:SetSpellByID(" + std::to_string(spellId) + ")\n"
            "local L = _G['TestTipTextLeft" + std::to_string(idx) + "']\n"
            "local R = _G['TestTipTextRight" + std::to_string(idx) + "']\n"
            "local lt = (L and L:GetText()) or ''\n"
            "local rt = (R and R:GetText()) or ''\n"
            "__out = lt .. '|' .. rt\n"));
        return lua.globalString("__out");
    };

    const auto isLineRed = [&](uint32_t spellId, int idx) -> bool {
        REQUIRE(lua.executeString(
            "TestTip:ClearLines()\n"
            "TestTip:SetSpellByID(" + std::to_string(spellId) + ")\n"
            "local L = _G['TestTipTextLeft" + std::to_string(idx) + "']\n"
            "local r, g, b = L:GetTextColor()\n"
            "__out = (r > 0.9 and g < 0.2 and b < 0.2) and 'red' or "
            "((r > 0.9 and g > 0.9 and b > 0.9) and 'white' or 'other')\n"));
        return lua.globalString("__out") == "red";
    };

    const auto isLineWhite = [&](uint32_t spellId, int idx) -> bool {
        REQUIRE(lua.executeString(
            "TestTip:ClearLines()\n"
            "TestTip:SetSpellByID(" + std::to_string(spellId) + ")\n"
            "local L = _G['TestTipTextLeft" + std::to_string(idx) + "']\n"
            "local r, g, b = L:GetTextColor()\n"
            "__out = (r > 0.9 and g > 0.9 and b > 0.9) and 'white' or 'other'\n"));
        return lua.globalString("__out") == "white";
    };

    // 1. SPELL_ATTR0_ON_NEXT_SWING (0x4): no "Melee Range" right text, "Next melee" cast line.
    CHECK(tipLine(kHeroicStrike, 2) == "15 Rage|");
    CHECK(tipLine(kHeroicStrike, 3) == "Next melee|");
    CHECK(tipLine(kCleave, 2)       == "20 Rage|");
    CHECK(tipLine(kCleave, 3)       == "Next melee|");
    CHECK(tipLine(kMaul, 2)         == "15 Rage|");
    CHECK(tipLine(kMaul, 3)         == "Next melee|");
    CHECK(tipLine(kRaptorStrike, 2) == "15 Mana|");
    CHECK(tipLine(kRaptorStrike, 3) == "Next melee|6 sec cooldown");

    // Normal instant melee spells (RangeIndex=2 without 0x4): keep "Melee Range" + "Instant".
    CHECK(tipLine(kMortalStrike, 2)   == "30 Rage|Melee Range");
    CHECK(tipLine(kMortalStrike, 3)   == "Instant|6 sec cooldown");
    CHECK(tipLine(kOverpowerR1, 2)    == "5 Rage|Melee Range");
    CHECK(tipLine(kOverpowerR1, 3)    == "Instant|5 sec cooldown");
    CHECK(tipLine(kSinisterStrike, 2) == "45 Energy|Melee Range");
    CHECK(tipLine(kSinisterStrike, 3) == "Instant|");
    CHECK(tipLine(kShieldBash, 2)     == "10 Rage|Melee Range");
    CHECK(tipLine(kShieldBash, 3)     == "Instant|12 sec cooldown");
    CHECK(tipLine(kWhirlwind, 2)      == "25 Rage|");
    CHECK(tipLine(kWhirlwind, 3)      == "Instant|10 sec cooldown");

    // 2. Unarmed: all weapon/shield requirements render in RED.
    CHECK(tipLine(kHeroicStrike, 4) == "Requires Melee Weapon|");
    CHECK(isLineRed(kHeroicStrike, 4));
    CHECK(tipLine(kShieldBash, 4)   == "Requires Shields|");
    CHECK(isLineRed(kShieldBash, 4));
    CHECK(tipLine(kBackstab, 4)     == "Requires Daggers|");
    CHECK(isLineRed(kBackstab, 4));
    CHECK(tipLine(kSteadyShot, 4)   == "Requires Ranged Weapon|");
    CHECK(isLineRed(kSteadyShot, 4));

    // 3. Intact 1H sword in MH: Heroic Strike white; Backstab (requires MH dagger) & Shield Bash red.
    const ItemDef intactSword = makeEquipItem(gh, 90001, "Test Sword", 2, 7, 13, 50, 50);
    gh.inventoryRef().setEquipSlot(EquipSlot::MAIN_HAND, intactSword);
    CHECK(isLineWhite(kHeroicStrike, 4));
    CHECK(isLineRed(kBackstab, 4));
    CHECK(isLineRed(kShieldBash, 4));

    // 4. Broken 1H sword (durability 0/50) in MH: Heroic Strike turns RED and unusable!
    const ItemDef brokenSword = makeEquipItem(gh, 90001, "Test Sword", 2, 7, 13, 0, 50);
    gh.inventoryRef().setEquipSlot(EquipSlot::MAIN_HAND, brokenSword);
    CHECK(isLineRed(kHeroicStrike, 4));
    CHECK(evalString(lua, "IsUsableSpell(78)") == "false");

    // 5. Intact shield in OH: Shield Bash white; broken shield -> red.
    const ItemDef intactShield = makeEquipItem(gh, 90002, "Test Shield", 4, 6, 14, 100, 100);
    gh.inventoryRef().setEquipSlot(EquipSlot::OFF_HAND, intactShield);
    CHECK(isLineWhite(kShieldBash, 4));
    const ItemDef brokenShield = makeEquipItem(gh, 90002, "Test Shield", 4, 6, 14, 0, 100);
    gh.inventoryRef().setEquipSlot(EquipSlot::OFF_HAND, brokenShield);
    CHECK(isLineRed(kShieldBash, 4));

    // 6. Dagger in OH with sword in MH -> Backstab still RED; intact dagger in MH -> WHITE.
    const ItemDef intactDagger = makeEquipItem(gh, 90003, "Test Dagger", 2, 15, 13, 40, 40);
    gh.inventoryRef().setEquipSlot(EquipSlot::MAIN_HAND, intactSword);
    gh.inventoryRef().setEquipSlot(EquipSlot::OFF_HAND, intactDagger);
    CHECK(isLineRed(kBackstab, 4));
    gh.inventoryRef().setEquipSlot(EquipSlot::MAIN_HAND, intactDagger);
    CHECK(isLineWhite(kBackstab, 4));

    // 7. Ranged weapon (Bow, class 2 subclass 2) in RANGED: Steady Shot white; broken bow -> red.
    const ItemDef intactBow = makeEquipItem(gh, 90004, "Test Bow", 2, 2, 15, 60, 60);
    gh.inventoryRef().setEquipSlot(EquipSlot::RANGED, intactBow);
    CHECK(isLineWhite(kSteadyShot, 4));
    const ItemDef brokenBow = makeEquipItem(gh, 90004, "Test Bow", 2, 2, 15, 0, 60);
    gh.inventoryRef().setEquipSlot(EquipSlot::RANGED, brokenBow);
    CHECK(isLineRed(kSteadyShot, 4));

    // 8. Two-Handed Melee Weapon requirement (class 2, 2H sword subclass 8 vs 1H sword subclass 7).
    constexpr uint32_t kSynthetic2HSpell = 999901u;
    auto& synth = gh.spellNameCacheRef()[kSynthetic2HSpell];
    synth.name = "Test Two-Handed Strike";
    synth.equippedItemClass = 2;
    synth.equippedItemSubClassMask = 0x162u; // 2H Axes/Maces/Swords/Polearms/Staves
    synth.attrEx3 = 0x400u;                  // SPELL_ATTR_EX3_REQUIRES_MAIN_HAND_WEAPON
    gh.inventoryRef().setEquipSlot(EquipSlot::MAIN_HAND, intactSword); // 1H sword -> unmet
    CHECK(isLineRed(kSynthetic2HSpell, 3));
    const ItemDef intact2H = makeEquipItem(gh, 90005, "Test Greatsword", 2, 8, 17, 80, 80);
    gh.inventoryRef().setEquipSlot(EquipSlot::MAIN_HAND, intact2H);
    CHECK(isLineWhite(kSynthetic2HSpell, 3));
    const ItemDef broken2H = makeEquipItem(gh, 90005, "Test Greatsword", 2, 8, 17, 0, 80);
    gh.inventoryRef().setEquipSlot(EquipSlot::MAIN_HAND, broken2H);
    CHECK(isLineRed(kSynthetic2HSpell, 3));
}
