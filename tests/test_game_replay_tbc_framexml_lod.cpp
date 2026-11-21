// TBC 2.4.3 PaperDoll Attack Power, combat-stat events, UseContainerItem taint,
// core widget/override-binding APIs, and full FrameXML + 16 Blizzard_* LoD boot tests.
#include <catch_amalgamated.hpp>

#include "addons/addon_manager.hpp"
#include "addons/expansion_api_surface.hpp"
#include "addons/lua_engine.hpp"
#include "game_replay.hpp"
#include "ui/keybinding_manager.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

using replay::Session;
using replay::UpdateObject;
using wowee::game::ObjectType;
using UF = wowee::game::UF;

namespace {

uint32_t packUnitBytes0(uint8_t race, uint8_t cls, uint8_t gender, uint8_t powerType) {
    return static_cast<uint32_t>(race) |
           (static_cast<uint32_t>(cls) << 8) |
           (static_cast<uint32_t>(gender) << 16) |
           (static_cast<uint32_t>(powerType) << 24);
}

uint32_t floatBits(float f) {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, 4);
    return bits;
}

uint32_t packInt16Pair(int16_t lo, int16_t hi) {
    return static_cast<uint32_t>(static_cast<uint16_t>(lo)) |
           (static_cast<uint32_t>(static_cast<uint16_t>(hi)) << 16);
}

std::filesystem::path resolveTbcDataDir() {
    if (const char* env = std::getenv("WOWEE_TBC_DATA_DIR"); env && *env) {
        if (std::filesystem::is_directory(env)) return env;
    }
    for (const char* candidate : {
             "/workspace/azerothcore/client/wowee-data/expansions/tbc",
         }) {
        if (std::filesystem::is_directory(candidate)) return candidate;
    }
    return {};
}

} // namespace

TEST_CASE("TBC UnitAttackPower includes MODS/MULTIPLIER and combat-stat events fire", "[tbc][stats][events]") {
    Session session{"tbc"};
    wowee::addons::LuaEngine lua;
    REQUIRE(lua.initialize());
    lua.setGameHandler(&session.handler());

    std::vector<std::pair<std::string, std::string>> fired;
    session.handler().setAddonEventCallback(
        [&](const std::string& ev, const std::vector<std::string>& args) {
            fired.emplace_back(ev, args.empty() ? std::string() : args[0]);
            lua.fireEvent(ev, args);
        });

    // Initial CREATE with base AP = 395, +44 posBuff, 0 negBuff, mult = 0.0 -> total 439.
    session.receive(UpdateObject().tbc().create(
        Session::kPlayerGuid, ObjectType::PLAYER,
        {{UF::UNIT_FIELD_HEALTH, 4704},
         {UF::UNIT_FIELD_MAXHEALTH, 4704},
         {UF::UNIT_FIELD_LEVEL, 70},
         {UF::UNIT_FIELD_BYTES_0, packUnitBytes0(1, 4, 0, 3)}, // Human Rogue
         {UF::UNIT_FIELD_ATTACK_POWER, 395},
         {UF::UNIT_FIELD_ATTACK_POWER_MODS, packInt16Pair(44, 0)},
         {UF::UNIT_FIELD_ATTACK_POWER_MULTIPLIER, floatBits(0.0f)},
         {UF::UNIT_FIELD_RANGED_ATTACK_POWER, 200},
         {UF::UNIT_FIELD_RANGED_ATTACK_POWER_MODS, packInt16Pair(30, -10)},
         {UF::UNIT_FIELD_RANGED_ATTACK_POWER_MULTIPLIER, floatBits(0.0f)},
         {UF::UNIT_FIELD_MINDAMAGE, floatBits(301.0f)},
         {UF::UNIT_FIELD_MAXDAMAGE, floatBits(486.0f)},
         {UF::UNIT_FIELD_BASEATTACKTIME, 2800},
         {UF::UNIT_FIELD_RESISTANCES, 362}},
        true));

    CHECK(session.handler().getMeleeAttackPowerBase() == 395);
    CHECK(session.handler().getMeleeAttackPowerPos() == 44);
    CHECK(session.handler().getMeleeAttackPowerNeg() == 0);
    CHECK(session.handler().getMeleeAttackPower() == 439);
    CHECK(session.handler().getRangedAttackPower() == 220);

    REQUIRE(lua.executeString(
        "local b, p, n = UnitAttackPower('player')\n"
        "__ap_res = string.format('%d,%d,%d', b, p, n)\n"
        "local rb, rp, rn = UnitRangedAttackPower('player')\n"
        "__rap_res = string.format('%d,%d,%d', rb, rp, rn)\n"));
    lua_State* L = lua.getState();
    lua_getglobal(L, "__ap_res");
    CHECK(std::string(lua_tostring(L, -1)) == "395,44,0");
    lua_pop(L, 1);
    lua_getglobal(L, "__rap_res");
    CHECK(std::string(lua_tostring(L, -1)) == "200,30,-10");
    lua_pop(L, 1);

    // Apply +10% multiplier via VALUES update and verify all combat-stat events fire.
    fired.clear();
    session.receive(UpdateObject().tbc().values(
        Session::kPlayerGuid,
        {{UF::UNIT_FIELD_ATTACK_POWER_MULTIPLIER, floatBits(0.10f)},
         {UF::UNIT_FIELD_RANGED_ATTACK_POWER_MODS, packInt16Pair(50, 0)},
         {UF::UNIT_FIELD_MINDAMAGE, floatBits(320.0f)},
         {UF::UNIT_FIELD_MINRANGEDDAMAGE, floatBits(150.0f)},
         {UF::UNIT_FIELD_BASEATTACKTIME, 2600},
         {UF::UNIT_FIELD_RESISTANCES, 400},
         // Toggle stealth visibility flag (byte 2 = 0x02) + stealth shapeshift (byte 3 = 30)
         {UF::UNIT_FIELD_BYTES_1, (30u << 24) | (0x02u << 16)}}));

    // Native 2.4.3 scales each part separately by (1 + mult) with rounding:
    // lround(395 * 1.10) = 435, lround(44 * 1.10) = 48, neg = 0 -> total 483
    REQUIRE(lua.executeString(
        "local b, p, n = UnitAttackPower('player')\n"
        "__ap_mult_res = string.format('%d,%d,%d', b, p, n)\n"
        "__stealthed = IsStealthed() and 1 or 0\n"));
    lua_getglobal(L, "__ap_mult_res");
    CHECK(std::string(lua_tostring(L, -1)) == "435,48,0");
    lua_pop(L, 1);
    lua_getglobal(L, "__stealthed");
    CHECK(lua_tointeger(L, -1) == 1);
    lua_pop(L, 1);

    auto hasEvent = [&](const char* name, const char* arg0 = nullptr) {
        for (const auto& [e, a] : fired) {
            if (e == name && (!arg0 || a == arg0)) return true;
        }
        return false;
    };
    CHECK(hasEvent("UNIT_ATTACK_POWER", "player"));
    CHECK(hasEvent("UNIT_RANGED_ATTACK_POWER", "player"));
    CHECK(hasEvent("UNIT_DAMAGE", "player"));
    CHECK(hasEvent("UNIT_RANGEDDAMAGE", "player"));
    CHECK(hasEvent("UNIT_ATTACK_SPEED", "player"));
    CHECK(hasEvent("UNIT_ATTACK", "player"));
    CHECK(hasEvent("UNIT_DEFENSE", "player"));
    CHECK(hasEvent("UPDATE_STEALTH"));
}

TEST_CASE("Frame:RegisterAllEvents, Minimap:GetPingPosition, EditBox:IsInIMECompositionMode, and OverrideBindings work", "[tbc][lua]") {
    Session session{"tbc"};
    wowee::addons::LuaEngine lua;
    REQUIRE(lua.initialize());
    lua.setGameHandler(&session.handler());
    wowee::addons::applyApiConventions(lua.getState(), "tbc");

    REQUIRE(lua.executeString(
        "__all_events_seen = {}\n"
        "local f = CreateFrame('Frame', 'TestAllEvFrame')\n"
        "f:SetScript('OnEvent', function(self, ev, a1) __all_events_seen[#__all_events_seen + 1] = ev .. ':' .. tostring(a1) end)\n"
        "f:RegisterAllEvents()\n"
        "__is_reg = f:IsEventRegistered('CUSTOM_ANY_EVENT')\n"
        "local mm = CreateFrame('Minimap', 'TestMinimapPing')\n"
        "mm:PingLocation(0.25, -0.5)\n"
        "__px, __py = mm:GetPingPosition()\n"
        "local eb = CreateFrame('EditBox', 'TestImeBox')\n"
        "__ime = eb:IsInIMECompositionMode()\n"
        "SetOverrideBinding(f, false, 'F9', 'JUMP')\n"
        "__ov1 = GetBindingAction('F9')\n"
        "SetOverrideBindingClick(f, true, 'F9', 'MyBtn', 'RightButton')\n"
        "__ov2 = GetBindingAction('F9')\n"
        "ClearOverrideBindings(f)\n"
        "__ov3 = GetBindingAction('F9')\n"));

    lua.fireEvent("CUSTOM_ANY_EVENT", {"hello"});
    REQUIRE(lua.executeString("TestAllEvFrame:UnregisterAllEvents()\n__is_reg_after = TestAllEvFrame:IsEventRegistered('CUSTOM_ANY_EVENT')\n"));
    lua.fireEvent("CUSTOM_AFTER_UNREG", {"nope"});

    lua_State* L = lua.getState();
    lua_getglobal(L, "__is_reg");
    CHECK(lua_toboolean(L, -1) != 0);
    lua_pop(L, 1);
    lua_getglobal(L, "__is_reg_after");
    CHECK(lua_toboolean(L, -1) == 0);
    lua_pop(L, 1);

    lua_getglobal(L, "__all_events_seen");
    REQUIRE(lua_istable(L, -1));
    CHECK(lua_objlen(L, -1) == 1);
    lua_rawgeti(L, -1, 1);
    CHECK(std::string(lua_tostring(L, -1)) == "CUSTOM_ANY_EVENT:hello");
    lua_pop(L, 2);

    lua_getglobal(L, "__px");
    CHECK(lua_tonumber(L, -1) == Catch::Approx(0.25));
    lua_pop(L, 1);
    lua_getglobal(L, "__py");
    CHECK(lua_tonumber(L, -1) == Catch::Approx(-0.5));
    lua_pop(L, 1);

    lua_getglobal(L, "__ime");
    CHECK(lua_isnil(L, -1));
    lua_pop(L, 1);

    lua_getglobal(L, "__ov1");
    CHECK(std::string(lua_tostring(L, -1)) == "JUMP");
    lua_pop(L, 1);
    lua_getglobal(L, "__ov2");
    CHECK(std::string(lua_tostring(L, -1)) == "CLICK MyBtn:RightButton");
    lua_pop(L, 1);
    lua_getglobal(L, "__ov3");
    CHECK(std::string(lua_tostring(L, -1)) != "CLICK MyBtn:RightButton");
    lua_pop(L, 1);
}

TEST_CASE("TBC FrameXML + all 16 Blizzard_* LoD addons boot with 0 Lua errors", "[tbc][framexml][lod]") {
    const auto tbcDir = resolveTbcDataDir();
    if (tbcDir.empty()) {
        SKIP("Set WOWEE_TBC_DATA_DIR to client/wowee-data/expansions/tbc");
    }
    const auto frameXmlDir = tbcDir / "interface" / "framexml";
    const auto addonsDir = tbcDir / "interface" / "addons";
    REQUIRE(std::filesystem::is_directory(frameXmlDir));
    REQUIRE(std::filesystem::is_directory(addonsDir));

    Session session{"tbc"};
    session.receive(UpdateObject().tbc().create(
        Session::kPlayerGuid, ObjectType::PLAYER,
        {{UF::UNIT_FIELD_HEALTH, 2000},
         {UF::UNIT_FIELD_MAXHEALTH, 2000},
         {UF::UNIT_FIELD_LEVEL, 70},
         {UF::UNIT_FIELD_BYTES_0, packUnitBytes0(1, 4, 0, 3)},
         {UF::UNIT_FIELD_ATTACK_POWER, 395},
         {UF::UNIT_FIELD_ATTACK_POWER_MODS, packInt16Pair(44, 0)}},
        true));

    wowee::addons::AddonManager mgr;
    REQUIRE(mgr.initialize(&session.handler()));
    auto* lua = mgr.getLuaEngine();
    REQUIRE(lua != nullptr);
    wowee::addons::removeApiAbsentFrom(lua->getState(), "tbc");
    wowee::addons::applyApiConventions(lua->getState(), "tbc");

    std::vector<std::string> errors;
    lua->setLuaErrorCallback([&](const std::string& err) {
        errors.push_back(err);
    });

    mgr.scanAddons(addonsDir.string());
    REQUIRE(mgr.loadFrameXml(frameXmlDir.string()));

    // Fire standard login lifecycle events so FrameXML initializes cleanly.
    mgr.fireEvent("VARIABLES_LOADED", {});
    mgr.fireEvent("PLAYER_LOGIN", {});
    mgr.fireEvent("PLAYER_ENTERING_WORLD", {});

    if (!errors.empty()) {
        for (const auto& e : errors) UNSCOPED_INFO("FrameXML error: " << e);
    }
    CHECK(errors.empty());

    // Load all 16 shipped TBC Blizzard_* LoadOnDemand addons and verify 0 Lua errors.
    static const char* kLodAddons[] = {
        "Blizzard_AuctionUI",
        "Blizzard_BattlefieldMinimap",
        "Blizzard_BindingUI",
        "Blizzard_CombatLog",
        "Blizzard_CombatText",
        "Blizzard_CraftUI",
        "Blizzard_GMSurveyUI",
        "Blizzard_GuildBankUI",
        "Blizzard_InspectUI",
        "Blizzard_ItemSocketingUI",
        "Blizzard_MacroUI",
        "Blizzard_RaidUI",
        "Blizzard_TalentUI",
        "Blizzard_TimeManager",
        "Blizzard_TradeSkillUI",
        "Blizzard_TrainerUI",
    };

    for (const char* addon : kLodAddons) {
        std::string reason;
        const size_t before = errors.size();
        const bool ok = mgr.loadAddOnByName(addon, reason);
        INFO("AddOn: " << addon << " reason=" << reason);
        CHECK(ok);
        for (size_t i = before; i < errors.size(); ++i) {
            UNSCOPED_INFO(addon << " Lua error: " << errors[i]);
        }
        CHECK(errors.size() == before);
    }

    // Exercise key functions that previously nil-crashed in TBC FrameXML / LoD addons.
    const size_t beforeExercise = errors.size();
    CHECK(mgr.runScript(
        "local x, y = Minimap:GetPingPosition()\n"
        "assert(type(x) == 'number' and type(y) == 'number')\n"
        "assert(type(GetCraftRecipeLink) == 'function')\n"
        "GetCraftRecipeLink(1)\n"
        "GMSurveyAnswerSubmit(1, 5, '')\n"
        "GMSurveyCommentSubmit('test')\n"
        "GMSurveySubmit()\n"
        "if SendMailBodyEditBox then SendMailBodyEditBox:IsInIMECompositionMode() end\n"
        "if CharacterFrame then CharacterFrame:Show(); PaperDollFrame_UpdateStats(); CharacterFrame:Hide() end\n"
        "if TalentFrame then TalentFrame:Show(); TalentFrame:Hide() end\n"
        "if MacroFrame then MacroFrame:Show(); MacroFrame:Hide() end\n"
        "if KeyBindingFrame then KeyBindingFrame:Show(); KeyBindingFrame:Hide() end\n"
        "if TimeManagerFrame then TimeManagerFrame:Show(); TimeManagerFrame:Hide() end\n"));
    for (size_t i = beforeExercise; i < errors.size(); ++i) {
        UNSCOPED_INFO("Exercise error: " << errors[i]);
    }
    CHECK(errors.size() == beforeExercise);
}
