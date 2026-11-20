// TBC 2.4.3 dynamic equipped-weapon icon parity for Attack (6603), Auto Shot (75),
// Wand Shoot (5019), non-auto-repeat Shoot (3018), and Throw (2764) across
// GetActionTexture (main bar, bonus stance bar, macros), GetSpellTexture,
// GetSpellInfo, and live equip/swap/unequip event refresh (SPELLS_CHANGED +
// ACTIONBAR_SLOT_CHANGED).
#include <catch_amalgamated.hpp>

#include "addons/lua_engine.hpp"
#include "game_replay.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

using replay::Session;
using wowee::game::ActionBarSlot;
using wowee::game::EquipSlot;
using wowee::game::GameHandler;
using wowee::game::GameHandlerTestAccess;
namespace InvType = wowee::game::InvType;
using wowee::game::ItemDef;
using wowee::game::ItemQueryResponseData;
using wowee::game::Opcode;
using wowee::game::wireOpcode;
using wowee::network::Packet;

namespace {

constexpr uint32_t kAttack    = 6603;
constexpr uint32_t kAutoShot  = 75;
constexpr uint32_t kShootBow  = 3018;
constexpr uint32_t kShootWand = 5019;
constexpr uint32_t kThrow     = 2764;
constexpr uint32_t kHeroic    = 78;

constexpr uint32_t kAttrEx2AutoRepeat = 0x20;

constexpr uint32_t kDispZinrokh = 32944;
constexpr uint32_t kDispSword   = 1542;
constexpr uint32_t kDispBow     = 5189;
constexpr uint32_t kDispWand    = 7412;
constexpr uint32_t kDispThrown  = 9100;
constexpr uint32_t kDispRelic   = 9200;

std::string evalStr(wowee::addons::LuaEngine& lua, const char* expr) {
    REQUIRE(lua.executeString(std::string("_R = tostring(") + expr + ")"));
    return lua.globalString("_R");
}

void setupSpellsAndResolvers(Session& s) {
    auto& gh = s.handler();
    auto& cache = gh.spellNameCacheRef();
    struct Def {
        uint32_t id;
        const char* name;
        uint32_t attrEx2;
    };
    for (const Def& d : {
             Def{kAttack,    "Attack",        0},
             Def{kAutoShot,  "Auto Shot",     kAttrEx2AutoRepeat},
             Def{kShootBow,  "Shoot",         0},
             Def{kShootWand, "Shoot",         kAttrEx2AutoRepeat},
             Def{kThrow,     "Throw",         0},
             Def{kHeroic,    "Heroic Strike", 0},
         }) {
        auto& e = cache[d.id];
        e.name = d.name;
        e.attrEx2 = d.attrEx2;
        Packet learned(wireOpcode(Opcode::SMSG_LEARNED_SPELL));
        learned.writeUInt32(d.id);
        s.receive(learned);
    }
    gh.spellNameCacheLoadedRef() = true;

    gh.setSpellIconPathResolver([](uint32_t id) -> std::string {
        switch (id) {
            case kAttack:    return "Interface\\Icons\\Temp";
            case kAutoShot:  return "Interface\\Icons\\Ability_Whirlwind";
            case kShootBow:  return "Interface\\Icons\\Ability_Marksmanship";
            case kShootWand: return "Interface\\Icons\\Ability_ShootWand";
            case kThrow:     return "Interface\\Icons\\Ability_Throw";
            case kHeroic:    return "Interface\\Icons\\Ability_Rogue_Ambush";
            default:         return "";
        }
    });

    gh.setItemIconPathResolver([](uint32_t displayId) -> std::string {
        switch (displayId) {
            case kDispZinrokh: return "Interface\\Icons\\INV_Sword_55";
            case kDispSword:   return "Interface\\Icons\\INV_Sword_04";
            case kDispBow:     return "Interface\\Icons\\INV_Weapon_Bow_05";
            case kDispWand:    return "Interface\\Icons\\INV_Wand_04";
            case kDispThrown:  return "Interface\\Icons\\INV_ThrowingAxe_01";
            case kDispRelic:   return "Interface\\Icons\\INV_Relics_IdolofFerocity";
            default:           return "";
        }
    });
}

void equipDirect(GameHandler& gh, EquipSlot slot, uint32_t itemId, uint32_t displayId, uint8_t invType) {
    if (itemId == 0) {
        gh.inventoryRef().setEquipSlot(slot, ItemDef{});
    } else {
        ItemDef def;
        def.itemId = itemId;
        def.displayInfoId = displayId;
        def.inventoryType = invType;
        def.name = "TestWeapon";
        gh.inventoryRef().setEquipSlot(slot, def);
    }
    GameHandlerTestAccess::fireBagUpdates(gh);
}

}  // namespace

TEST_CASE("Attack and ranged weapon-icon spells substitute equipped weapon icons per TBC 2.4.3",
          "[replay][icons][weapons]") {
    Session s("tbc");
    auto& gh = s.handler();
    setupSpellsAndResolvers(s);

    // Place Attack on main bar slot 1 (index 0) and Battle Stance bonus bar slot 73 (index 72),
    // Auto Shot on slot 2 (index 1), Wand Shoot on slot 3 (index 2), Bow Shoot (3018) on slot 4,
    // Throw on slot 5, Heroic Strike on slot 6.
    gh.setActionBarSlot(0, ActionBarSlot::SPELL, kAttack);
    gh.setActionBarSlot(72, ActionBarSlot::SPELL, kAttack);
    gh.setActionBarSlot(1, ActionBarSlot::SPELL, kAutoShot);
    gh.setActionBarSlot(2, ActionBarSlot::SPELL, kShootWand);
    gh.setActionBarSlot(3, ActionBarSlot::SPELL, kShootBow);
    gh.setActionBarSlot(4, ActionBarSlot::SPELL, kThrow);
    gh.setActionBarSlot(5, ActionBarSlot::SPELL, kHeroic);

    std::vector<std::pair<std::string, std::string>> events;
    gh.setAddonEventCallback([&](const std::string& e, const std::vector<std::string>& args) {
        events.emplace_back(e, args.empty() ? std::string{} : args[0]);
    });

    wowee::addons::LuaEngine lua;
    REQUIRE(lua.initialize());
    lua.setGameHandler(&gh);

    auto hasEvent = [&](const std::string& name, const std::string& arg = "") {
        return std::any_of(events.begin(), events.end(), [&](const auto& ev) {
            return ev.first == name && (arg.empty() || ev.second == arg);
        });
    };

    // 1. Unarmed / no ranged: Attack -> Spell-Reset; ranged spells keep DBC icons.
    CHECK(evalStr(lua, "GetActionTexture(1)") == "Interface\\Buttons\\Spell-Reset");
    CHECK(evalStr(lua, "GetActionTexture(73)") == "Interface\\Buttons\\Spell-Reset");
    CHECK(evalStr(lua, "GetSpellTexture('Attack')") == "Interface\\Buttons\\Spell-Reset");
    CHECK(evalStr(lua, "select(3, GetSpellInfo('Attack'))") == "Interface\\Buttons\\Spell-Reset");
    CHECK(evalStr(lua, "GetActionTexture(2)") == "Interface\\Icons\\Ability_Whirlwind");
    CHECK(evalStr(lua, "GetActionTexture(3)") == "Interface\\Icons\\Ability_ShootWand");
    CHECK(evalStr(lua, "GetActionTexture(4)") == "Interface\\Icons\\Ability_Marksmanship");
    CHECK(evalStr(lua, "GetActionTexture(5)") == "Interface\\Icons\\Ability_Throw");

    // 2. Equip Zin'rokh (2H sword) in MAIN_HAND -> Attack updates on both slot 1 & bonus slot 73,
    //    and fires SPELLS_CHANGED + ACTIONBAR_SLOT_CHANGED(1) + ACTIONBAR_SLOT_CHANGED(73),
    //    without touching unrelated Heroic Strike (slot 6) or Auto Shot (slot 2).
    events.clear();
    equipDirect(gh, EquipSlot::MAIN_HAND, 19854, kDispZinrokh, InvType::TWO_HAND);
    CHECK(evalStr(lua, "GetActionTexture(1)") == "Interface\\Icons\\INV_Sword_55");
    CHECK(evalStr(lua, "GetActionTexture(73)") == "Interface\\Icons\\INV_Sword_55");
    CHECK(evalStr(lua, "GetSpellTexture('Attack')") == "Interface\\Icons\\INV_Sword_55");
    CHECK(evalStr(lua, "select(3, GetSpellInfo('Attack'))") == "Interface\\Icons\\INV_Sword_55");
    CHECK(hasEvent("SPELLS_CHANGED"));
    CHECK(hasEvent("ACTIONBAR_SLOT_CHANGED", "1"));
    CHECK(hasEvent("ACTIONBAR_SLOT_CHANGED", "73"));
    CHECK_FALSE(hasEvent("ACTIONBAR_SLOT_CHANGED", "2"));
    CHECK_FALSE(hasEvent("ACTIONBAR_SLOT_CHANGED", "6"));

    // 3. Swap MAIN_HAND to Worn Shortsword -> Attack icon swaps live + fires events.
    events.clear();
    equipDirect(gh, EquipSlot::MAIN_HAND, 25, kDispSword, InvType::ONE_HAND);
    CHECK(evalStr(lua, "GetActionTexture(1)") == "Interface\\Icons\\INV_Sword_04");
    CHECK(evalStr(lua, "GetSpellTexture('Attack')") == "Interface\\Icons\\INV_Sword_04");
    CHECK(hasEvent("SPELLS_CHANGED"));
    CHECK(hasEvent("ACTIONBAR_SLOT_CHANGED", "1"));

    // 4. Unrelated bag update (no weapon change) -> must NOT fire SPELLS_CHANGED or slot 1.
    events.clear();
    GameHandlerTestAccess::fireBagUpdates(gh);
    CHECK_FALSE(hasEvent("SPELLS_CHANGED"));
    CHECK_FALSE(hasEvent("ACTIONBAR_SLOT_CHANGED", "1"));

    // 5. Unequip MAIN_HAND -> Attack reverts to Spell-Reset + fires events.
    events.clear();
    equipDirect(gh, EquipSlot::MAIN_HAND, 0, 0, 0);
    CHECK(evalStr(lua, "GetActionTexture(1)") == "Interface\\Buttons\\Spell-Reset");
    CHECK(evalStr(lua, "GetSpellTexture('Attack')") == "Interface\\Buttons\\Spell-Reset");
    CHECK(hasEvent("SPELLS_CHANGED"));
    CHECK(hasEvent("ACTIONBAR_SLOT_CHANGED", "1"));

    // 6. Transient displayInfoId == 0 before item query arrives -> safe Spell-Reset fallback,
    //    then once item info arrives in cache and fireBagUpdates runs -> swaps to weapon icon.
    events.clear();
    equipDirect(gh, EquipSlot::MAIN_HAND, 19854, 0, InvType::TWO_HAND);
    CHECK(evalStr(lua, "GetActionTexture(1)") == "Interface\\Buttons\\Spell-Reset");
    ItemQueryResponseData zinrokhInfo;
    zinrokhInfo.entry = 19854;
    zinrokhInfo.valid = true;
    zinrokhInfo.displayInfoId = kDispZinrokh;
    zinrokhInfo.inventoryType = InvType::TWO_HAND;
    gh.itemInfoCacheRef()[19854] = zinrokhInfo;
    events.clear();
    GameHandlerTestAccess::fireBagUpdates(gh);
    CHECK(evalStr(lua, "GetActionTexture(1)") == "Interface\\Icons\\INV_Sword_55");
    CHECK(hasEvent("SPELLS_CHANGED"));
    CHECK(hasEvent("ACTIONBAR_SLOT_CHANGED", "1"));

    // 7. Equip Bow (InvType::RANGED_BOW) -> Auto Shot (75) and Wand Shoot (5019) take bow icon;
    //    non-auto-repeat Shoot (3018) and Throw (2764) keep their DBC icons.
    events.clear();
    equipDirect(gh, EquipSlot::RANGED, 2504, kDispBow, InvType::RANGED_BOW);
    CHECK(evalStr(lua, "GetActionTexture(2)") == "Interface\\Icons\\INV_Weapon_Bow_05");
    CHECK(evalStr(lua, "GetSpellTexture('Auto Shot')") == "Interface\\Icons\\INV_Weapon_Bow_05");
    CHECK(evalStr(lua, "GetActionTexture(3)") == "Interface\\Icons\\INV_Weapon_Bow_05");
    CHECK(evalStr(lua, "GetActionTexture(4)") == "Interface\\Icons\\Ability_Marksmanship");
    CHECK(evalStr(lua, "GetActionTexture(5)") == "Interface\\Icons\\Ability_Throw");
    CHECK(hasEvent("SPELLS_CHANGED"));
    CHECK(hasEvent("ACTIONBAR_SLOT_CHANGED", "2"));
    CHECK(hasEvent("ACTIONBAR_SLOT_CHANGED", "3"));
    CHECK_FALSE(hasEvent("ACTIONBAR_SLOT_CHANGED", "4"));
    CHECK_FALSE(hasEvent("ACTIONBAR_SLOT_CHANGED", "5"));

    // 8. Equip Wand (InvType::RANGED_GUN) -> Auto Shot (75) and Wand Shoot (5019) take wand icon.
    events.clear();
    equipDirect(gh, EquipSlot::RANGED, 5069, kDispWand, InvType::RANGED_GUN);
    CHECK(evalStr(lua, "GetActionTexture(2)") == "Interface\\Icons\\INV_Wand_04");
    CHECK(evalStr(lua, "GetActionTexture(3)") == "Interface\\Icons\\INV_Wand_04");
    CHECK(hasEvent("SPELLS_CHANGED"));
    CHECK(hasEvent("ACTIONBAR_SLOT_CHANGED", "2"));
    CHECK(hasEvent("ACTIONBAR_SLOT_CHANGED", "3"));

    // 9. Equip Thrown (InvType::THROWN) or Relic (28) -> Auto Shot & Wand Shoot revert to DBC icons!
    equipDirect(gh, EquipSlot::RANGED, 25861, kDispThrown, InvType::THROWN);
    CHECK(evalStr(lua, "GetActionTexture(2)") == "Interface\\Icons\\Ability_Whirlwind");
    CHECK(evalStr(lua, "GetActionTexture(3)") == "Interface\\Icons\\Ability_ShootWand");
    CHECK(evalStr(lua, "GetActionTexture(5)") == "Interface\\Icons\\Ability_Throw");

    equipDirect(gh, EquipSlot::RANGED, 22397, kDispRelic, 28);
    CHECK(evalStr(lua, "GetActionTexture(2)") == "Interface\\Icons\\Ability_Whirlwind");
    CHECK(evalStr(lua, "GetActionTexture(3)") == "Interface\\Icons\\Ability_ShootWand");

    // 10. Unequip Ranged -> DBC icons preserved.
    equipDirect(gh, EquipSlot::RANGED, 0, 0, 0);
    CHECK(evalStr(lua, "GetActionTexture(2)") == "Interface\\Icons\\Ability_Whirlwind");
    CHECK(evalStr(lua, "GetActionTexture(3)") == "Interface\\Icons\\Ability_ShootWand");
}
