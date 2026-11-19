#include "addons/expansion_api_surface.hpp"

#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

namespace wowee::addons {

namespace {

// Measured on the 2.4.3 client (build 8606, Windows under Wine) with
// `/script` checks of type(_G[name]) for every name this client answers with
// a fixed value or an empty body; each of these read "nil". The shipped TBC
// FrameXML and Blizzard_* addons call none of them outside IsMacClient()
// branches (MovieRecording_*) or comments (SaveMacros in Blizzard_MacroUI).
// Evidence: _experiments/20261117T2356Z.lua_api_stubs/.
constexpr const char* kAbsentFromTbc[] = {
    // Battle.net, 3.3.5.
    "BNFeaturesEnabled", "BNFeaturesEnabledAndConnected",
    "BNGetConversationInfo", "BNGetConversationMemberInfo", "BNGetFriendInfo",
    "BNGetFriendInfoByID", "BNGetFriendInviteInfo", "BNGetMaxPlayersInConversation",
    "BNGetNumConversationMembers", "BNGetNumFriendInvites", "BNGetToonInfo",
    "BNInviteToConversation", "BNIsFriend", "BNIsToonBlocked", "BNLeaveConversation",
    "BNListConversation", "BNRequestFOFInfo", "BNSendConversationMessage",
    "BNSendWhisper", "BNSetMatureLanguageFilter", "BNSetToonBlocked",
    "GetAutoCompletePresenceID",
    // Vehicles, Wintergrasp and totem bar, 3.x.
    "VehicleAimDownStart", "VehicleAimDownStop", "VehicleAimGetNormAngle",
    "VehicleAimGetNormPower", "VehicleAimUpStart", "VehicleAimUpStop",
    "UnitVehicleSkin", "GetNumBattlefieldVehicles", "GetBattlefieldVehicleInfo",
    "CanQueueForWintergrasp", "GetWintergraspWaitTime",
    "GetMultiCastBarOffset", "GetMultiCastTotemSpells",
    // Dungeon map levels and map debugging, 3.x.
    "GetNumDungeonMapLevels", "GetCurrentMapDungeonLevel", "SetDungeonMapLevel",
    "DungeonUsesTerrainMap", "ClickLandmark", "HasDebugZoneMap", "GetDebugZoneMap",
    "GetNumMapDebugObjects", "GetMapDebugObjectInfo",
    // Dungeon finder, currencies, achievements, trade skill links, 3.x.
    "GetLFGProposalEncounter", "GetPartyLFGBackfillInfo",
    "GetInstanceLockTimeRemainingEncounter", "SearchLFGGetResults",
    "SearchLFGGetPartyResults", "SearchLFGGetEncounterResults", "SearchLFGSort",
    "ExpandCurrencyList", "SetCurrencyBackpack", "SetCurrencyUnused",
    "GetBackpackCurrencyInfo", "InspectAchievements", "GetTradeSkillListLink",
    "IsTradeSkillLinked", "CanChangePlayerDifficulty", "GetNumArenaOpponents",
    "GetPreviousArenaSeason", "SortBGList", "QuestGetAutoAccept",
    "GetNumQuestItemDrops", "GetQuestLogItemDrop",
    // Options and system, 3.x.
    "RestoreVideoEffectsDefaults", "RestoreVideoResolutionDefaults",
    "RestoreVideoStereoDefaults", "IsStereoVideoAvailable",
    "IsPlayerResolutionAvailable", "DetectWowMouse", "RegisterStaticConstants",
    "FillLocalizedClassList", "ConsoleAddMessage", "SaveMacros",
    "GMSurveyAnswer", "GMReportLag", "GMResponseNeedMoreHelp",
    // The Mac movie recorder; the Windows client has none of it.
    "MovieRecording_Cancel", "MovieRecording_DataRate",
    "MovieRecording_GetAspectRatio", "MovieRecording_GetMovieFullPath",
    "MovieRecording_GetViewportWidth", "MovieRecording_IsCompressing",
    "MovieRecording_IsCursorRecordingSupported", "MovieRecording_IsRecording",
    "MovieRecording_IsSupported", "MovieRecording_MaxLength",
    "MovieRecording_Toggle", "MovieRecording_ToggleGUI",
};


// Measured on build 8606 with LuaConvProbe (every registered query called with
// thirteen argument tuples on both clients, dumps diffed): each of these
// answered 1 or nil where this client answered true or false, and none of
// them answered a Lua boolean in any probe. 2.4.3 does have boolean answers -
// IsPetAttackActive, GetTotemInfo, GetLookingForGroup's flags, GetSpellInfo's
// isFunnel, KBSetup_IsLoaded, IsPossessBarVisible, GetLFGResults - so the rule
// is per function rather than global. A caller comparing `== 1` or printing
// the answer sees the difference. Evidence:
// _experiments/20261118T010104Z.lua_conventions/.
constexpr const char* kFlagAnswersTbc[] = {
    "ActionHasRange", "CanComplainChat", "CanComplainInboxItem", "CanEditGuildInfo",
    "CanEditGuildTabInfo", "CanEditMOTD", "CanEditOfficerNote", "CanEditPublicNote",
    "CanGrantLevel", "CanGuildBankRepair", "CanGuildDemote", "CanGuildInvite",
    "CanGuildPromote", "CanGuildRemove", "CanInspect", "CanJoinBattlefieldAsGroup",
    "CanMerchantRepair", "CanSummonFriend", "CanViewOfficerNote", "GetFriendInfo",
    "CheckInteractDistance", "CursorCanGoInSlot", "CursorHasItem", "CursorHasMacro",
    "CursorHasMoney", "CursorHasSpell",
    "GetActionBarToggles", "GetAddOnInfo", "GetBattlefieldStatus",
    "GetChannelDisplayInfo", "GetContainerItemInfo", "GetCraftFilter", "GetFactionInfo",
    "GetGuildRosterInfo", "GetGuildRosterShowOffline", "GuildControlGetRankFlags",
    "GetInventoryItemBroken", "GetOptOutOfLoot",
    "GetQuestLogLeaderBoard", "GetQuestLogPushable", "GetQuestLogTitle",
    "GetSkillLineInfo", "GetSpellAutocast", "GetTrackingInfo",
    "GetTradeSkillInvSlotFilter", "GetTradeSkillSubClassFilter", "GetVideoCaps",
    "GetWeaponEnchantInfo", "HasAction", "HasFullControl", "HasInspectHonorData",
    "HasKey", "HasPetUI", "HasWandEquipped", "InCinematic", "InCombatLockdown",
    "InRepairMode", "IsAddOnLoaded", "IsAltKeyDown", "IsArenaTeamCaptain",
    "IsAttackAction", "IsAutoRepeatAction", "IsAutoRepeatSpell", "IsBattlefieldArena",
    "IsConsumableAction", "IsControlKeyDown", "IsCurrentAction", "IsCurrentQuestFailed",
    "IsCurrentSpell", "IsDisplayChannelModerator", "IsDisplayChannelOwner",
    "IsDressableItem", "IsEquippableItem", "IsEquippedAction", "IsEquippedItem",
    "IsFactionInactive", "IsFalling", "IsFishingLoot", "IsFlying", "IsGuildLeader",
    "IsInGuild", "IsInInstance", "IsInLFGQueue", "IsIndoors", "IsInventoryItemLocked",
    "IsLeftAltKeyDown", "IsLeftControlKeyDown", "IsLeftShiftKeyDown", "IsModifiedClick",
    "IsModifierKeyDown", "IsMounted", "IsMouseButtonDown", "IsMuted", "IsOutOfBounds",
    "IsOutdoors", "IsPartyLeader", "IsQuestCompletable", "IsQuestWatched", "IsRealPartyLeader",
    "IsResting", "IsRightAltKeyDown", "IsRightControlKeyDown", "IsRightShiftKeyDown",
    "IsSelectedSpell", "IsShiftKeyDown", "IsSilenced", "IsStackableAction",
    "IsStealthed", "IsSubZonePVPPOI", "IsSwimming", "IsUnitOnQuest", "IsUsableAction",
    "IsUsableSpell", "IsVoiceChatAllowedByServer", "ItemTextHasNextPage",
    "LootSlotIsCoin", "LootSlotIsItem", "OffhandHasWeapon", "PetCanBeAbandoned",
    "PetCanBeRenamed", "PetHasActionBar", "PlayerHasSpells", "ResurrectHasSickness",
    "ShowingCloak", "ShowingHelm", "SpellCanTargetItem", "SpellIsTargeting",
    "UnitAffectingCombat", "UnitCanAssist", "UnitCanAttack", "UnitCanCooperate",
    "UnitExists", "UnitHasRelicSlot", "UnitInRange", "UnitIsCharmed", "UnitIsConnected",
    "UnitIsCorpse", "UnitIsDead", "UnitIsDeadOrGhost", "UnitIsEnemy", "UnitIsFriend",
    "UnitIsGhost", "UnitIsInMyGuild", "UnitIsPVP", "UnitIsPVPFreeForAll",
    "UnitIsPVPSanctuary", "UnitIsPartyLeader", "UnitIsPlayer", "UnitIsPossessed",
    "UnitIsRaidOfficer", "UnitIsSameServer", "UnitIsSilenced", "UnitIsTalking",
    "UnitIsTapped", "UnitIsTappedByPlayer", "UnitIsUnit", "UnitIsVisible", "UnitOnTaxi",
    "UnitPlayerControlled", "UnitPlayerOrPetInParty", "UnitPlayerOrPetInRaid",
    "issecurevariable",
};

/// Most values build 8606 answers, where this client answered more. Only
/// entries whose leading values were measured to agree; the extra values are
/// later expansions' additions (UnitClass's class index, UnitPowerType's token,
/// GetQuestLogTitle's quest id, GetItemInfo's sell price, GetNumFriends' online
/// count, GetContainerItemInfo's link, Northrend in GetMapContinents).
struct ResultCount { const char* name; int count; };
constexpr ResultCount kResultCountsTbc[] = {
    {"UnitClass", 2}, {"UnitRace", 2}, {"UnitPowerType", 1}, {"UnitPVPName", 1},
    {"UnitInRange", 1}, {"GetNumFriends", 1}, {"GetNumGuildMembers", 1},
    {"GetInboxNumItems", 1}, {"GetArenaCurrency", 1}, {"GetHonorCurrency", 1},
    {"GetPVPLifetimeStats", 2}, {"GetLanguageByIndex", 1}, {"GetCorpseMapPosition", 2},
    {"GetDeathReleasePosition", 2}, {"GetQuestLogTitle", 8}, {"GetContainerItemInfo", 6},
    {"GetItemInfo", 10}, {"GetMapContinents", 3},
    // Sixteen rank rights; the seventeenth, Create Guild Event, is 3.x's
    // (pair 11: 1 x13, nil, 1, 1 for a Guild Master).
    {"GuildControlGetRankFlags", 16},
};

/// Functions 2.4.3 refuses to run from insecure code - a /script line or any
/// AddOn - firing MACRO_ACTION_FORBIDDEN("Name()") or
/// ADDON_ACTION_FORBIDDEN(addon, "Name()") instead. Measured out of combat
/// with arguments that reach the check: an invalid one returns first
/// (TargetUnit("none") fires nothing, TargetUnit("player") is refused). The
/// refused call answers nothing, except the three that answered one nil.
/// Measured as not protected: Dismount, FollowUnit, Pickup*/PlaceAction,
/// SetRaidTarget, InviteUnit, UninviteUnit, PromoteToLeader,
/// ClearPartyAssignment, SetBinding*, ChangeActionBarPage, RegisterUnitWatch.
struct Protected { const char* name; int refusedResults; };
constexpr Protected kProtectedTbc[] = {
    {"AssistUnit", 0}, {"AttackTarget", 0}, {"CastSpell", 0}, {"CastSpellByName", 0},
    {"ClearFocus", 0}, {"ClearTarget", 1}, {"FocusUnit", 0}, {"PetAggressiveMode", 0},
    {"PetAttack", 0}, {"PetDefensiveMode", 0}, {"PetFollow", 0}, {"PetPassiveMode", 0},
    {"PetWait", 0}, {"RunMacroText", 0}, {"SetPartyAssignment", 0}, {"SpellStopCasting", 1},
    {"SpellStopTargeting", 1}, {"SpellTargetItem", 0}, {"SpellTargetUnit", 0},
    {"StartAttack", 0}, {"StopMacro", 0}, {"TargetLastTarget", 0}, {"TargetNearestEnemy", 0},
    {"TargetNearestEnemyPlayer", 0}, {"TargetNearestFriend", 0},
    {"TargetNearestFriendPlayer", 0}, {"TargetNearestPartyMember", 0},
    {"TargetNearestRaidMember", 0}, {"TargetUnit", 0}, {"TogglePetAutocast", 0},
    {"UseAction", 0}, {"UseContainerItem", 0}, {"UseItemByName", 0},
};

/// Widget methods measured the same way (each method of fresh and FrameXML
/// objects of every widget type): 1/nil on 2.4.3, true/false here. Only
/// EditBox:HasFocus answers a boolean there. TargetFrame_OnUpdate compares
/// TargetofTargetFrame:IsShown() with UnitExists("targettarget"), so the two
/// must agree in kind.
constexpr const char* kFlagMethodsTbc[] = {
    "GetFading", "IsClampedToScreen", "IsKeyboardEnabled", "IsMouseEnabled", "IsMovable",
    "IsProtected", "IsResizable", "IsShown", "IsToplevel", "IsUserPlaced", "IsVisible",
};

/// Widget methods that answer nil on 2.4.3 where this client answers a
/// value (pair 11: GameTooltip:SetUnit("player") is nil, so is SetHyperlink). UnitFrame_UpdateTooltip keeps itself as the owner's UpdateTooltip
/// only `if GameTooltip:SetUnit(...)`, so on 2.4.3 a unit frame's tooltip is
/// built once; answering true re-ran it every 0.2 s and wiped what the
/// caller added after it - the raid panel's drag hint vanished a frame after
/// it appeared.
constexpr const char* kNilMethodsTbc[] = {"SetUnit", "SetHyperlink"};

constexpr const char* kInsecureRoots = "wowee_insecure_roots";
constexpr const char* kInsecureChunks = "wowee_insecure_chunks";

enum ShapeBits : int { kFlags = 1, kProtect = 2, kNil = 4 };

/// Who made the running code insecure, innermost first: the AddOn whose file
/// defined a function on the stack, or a /script chunk. Empty when every Lua
/// frame is the interface's own (FrameXML, Blizzard_*, this client's chunks).
struct Taint { bool found = false; bool macro = false; std::string addon; };

/// Whether code from `source` (a chunk name, lua_Debug::source) is insecure,
/// and for which AddOn.
Taint insecureSource(lua_State* L, const char* source) {
    Taint t;
    if (!source) return t;
    if (source[0] == '@') {
        lua_getfield(L, LUA_REGISTRYINDEX, kInsecureRoots);
        if (lua_istable(L, -1)) {
            lua_pushnil(L);
            while (lua_next(L, -2) != 0) {
                size_t len = 0;
                const char* root = lua_tolstring(L, -2, &len);
                if (root && std::strncmp(source, root, len) == 0) {
                    t.found = true;
                    t.addon = lua_tostring(L, -1);
                    lua_pop(L, 3);
                    return t;
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
        return t;
    }
    lua_getfield(L, LUA_REGISTRYINDEX, kInsecureChunks);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, source);
        lua_rawget(L, -2);
        const bool macro = lua_toboolean(L, -1) != 0;
        lua_pop(L, 2);
        if (macro) { t.found = true; t.macro = true; }
    } else {
        lua_pop(L, 1);
    }
    return t;
}

Taint insecureCaller(lua_State* L) {
    lua_Debug ar;
    for (int level = 1; lua_getstack(L, level, &ar); ++level) {
        if (!lua_getinfo(L, "S", &ar) || !ar.source) continue;
        if (std::strcmp(ar.what, "C") == 0) continue;
        if (const Taint t = insecureSource(L, ar.source); t.found) return t;
    }
    return {};
}

/// Fire an interface event through the engine's registry hook
/// (LuaEngine::initialize sets "wowee_fire_event").
void fireEvent(lua_State* L, const char* event, const std::vector<std::string>& args) {
    lua_getfield(L, LUA_REGISTRYINDEX, "wowee_fire_event");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    lua_pushstring(L, event);
    for (const auto& a : args) lua_pushstring(L, a.c_str());
    lua_call(L, static_cast<int>(args.size()) + 1, 0);
}

/// The global replaced by a closure over the original. Upvalues: 1 the
/// original function, 2 ShapeBits, 3 most values (-1 any), 4 values a refused
/// protected call answers.
int shapedCall(lua_State* L) {
    const int bits = static_cast<int>(lua_tointeger(L, lua_upvalueindex(2)));
    const int most = static_cast<int>(lua_tointeger(L, lua_upvalueindex(3)));
    if (bits & kProtect) {
        if (const Taint t = insecureCaller(L); t.found) {
            // The name the caller used, as 2.4.3 reports it: "TargetUnit()"
            // for a direct call, "UNKNOWN()" through pcall.
            lua_Debug self;
            std::string name = "UNKNOWN";
            if (lua_getstack(L, 0, &self) && lua_getinfo(L, "n", &self) && self.name)
                name = self.name;
            name += "()";
            if (t.macro) fireEvent(L, "MACRO_ACTION_FORBIDDEN", {name});
            else fireEvent(L, "ADDON_ACTION_FORBIDDEN", {t.addon, name});
            const int refused = static_cast<int>(lua_tointeger(L, lua_upvalueindex(4)));
            lua_settop(L, 0);
            for (int i = 0; i < refused; ++i) lua_pushnil(L);
            return refused;
        }
    }
    const int nargs = lua_gettop(L);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, nargs, LUA_MULTRET);
    int n = lua_gettop(L);
    if (bits & kNil) {
        lua_settop(L, 0);
        lua_pushnil(L);
        return 1;
    }
    if (most >= 0 && n > most) { lua_settop(L, most); n = most; }
    if (bits & kFlags) {
        for (int i = 1; i <= n; ++i) {
            if (!lua_isboolean(L, i)) continue;
            if (lua_toboolean(L, i)) lua_pushnumber(L, 1); else lua_pushnil(L);
            lua_replace(L, i);
        }
    }
    return n;
}

/// issecure() -> 1 when no insecure code is on the stack, else nil. From a
/// /script line and from an AddOn 2.4.3 answers nil.
int lua_issecure(lua_State* L) {
    if (insecureCaller(L).found) lua_pushnil(L); else lua_pushnumber(L, 1);
    return 1;
}

/// issecurevariable([table,] name) -> 1 when the value is secure, else nil.
/// 2.4.3 answers by who wrote the variable; this client does not record
/// writers, so a Lua function stands for the code that made it: insecure
/// when an AddOn file or a /script chunk defined it (see insecureCaller).
/// Any other value, and an unset one, answers 1 as before. AceHook-2.1
/// refuses to hook a method this calls secure, and Quartz hooks its own
/// modules' methods.
int lua_issecurevariable(lua_State* L) {
    if (lua_istable(L, 1)) {
        lua_pushvalue(L, 2);
        lua_gettable(L, 1);
    } else {
        lua_pushvalue(L, 1);
        lua_gettable(L, LUA_GLOBALSINDEX);
    }
    bool secure = true;
    if (lua_type(L, -1) == LUA_TFUNCTION && !lua_iscfunction(L, -1)) {
        lua_Debug ar;
        lua_getinfo(L, ">S", &ar);
        secure = !insecureSource(L, ar.source).found;
    } else {
        lua_pop(L, 1);
    }
    if (secure) lua_pushnumber(L, 1); else lua_pushnil(L);
    return 1;
}

struct Shape { int bits = 0; int most = -1; int refused = 0; };

void shapeGlobal(lua_State* L, const char* name, const Shape& shape) {
    lua_getglobal(L, name);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    lua_pushinteger(L, shape.bits);
    lua_pushinteger(L, shape.most);
    lua_pushinteger(L, shape.refused);
    lua_pushcclosure(L, shapedCall, 4);
    lua_setglobal(L, name);
}

} // namespace

std::span<const char* const> apiAbsentFrom(std::string_view expansionId) {
    if (expansionId == "tbc") return kAbsentFromTbc;
    return {};
}

void removeApiAbsentFrom(lua_State* L, std::string_view expansionId) {
    for (const char* name : apiAbsentFrom(expansionId)) {
        lua_pushnil(L);
        lua_setglobal(L, name);
    }
}

void applyApiConventions(lua_State* L, std::string_view expansionId) {
    if (expansionId != "tbc") return;
    // One closure per name, so a function in two lists is wrapped once.
    std::vector<std::pair<std::string, Shape>> shapes;
    auto at = [&](const char* name) -> Shape& {
        for (auto& [n, sh] : shapes) if (n == name) return sh;
        return shapes.emplace_back(name, Shape{}).second;
    };
    for (const char* name : kFlagAnswersTbc) at(name).bits |= kFlags;
    for (const auto& r : kResultCountsTbc) at(r.name).most = r.count;
    for (const auto& p : kProtectedTbc) {
        Shape& sh = at(p.name);
        sh.bits |= kProtect;
        sh.refused = p.refusedResults;
    }
    // UnitClassBase answers what UnitClass answers on 2.4.3 ("Rogue",
    // "ROGUE"); here it answered the token alone.
    lua_getglobal(L, "UnitClass");
    lua_setglobal(L, "UnitClassBase");
    at("UnitClassBase").most = 2;
    for (const auto& [name, sh] : shapes) shapeGlobal(L, name.c_str(), sh);
    // Widget methods live in the one method table every widget indexes.
    lua_getglobal(L, "__WoweeFrameMT");
    if (lua_istable(L, -1)) {
        for (const char* name : kFlagMethodsTbc) {
            lua_pushstring(L, name);
            lua_rawget(L, -2);  // not through the no-op fallback
            if (!lua_isfunction(L, -1)) {
                lua_pop(L, 1);
                continue;
            }
            lua_pushinteger(L, kFlags);
            lua_pushinteger(L, -1);
            lua_pushinteger(L, 0);
            lua_pushcclosure(L, shapedCall, 4);
            lua_setfield(L, -2, name);
        }
        for (const char* name : kNilMethodsTbc) {
            lua_pushstring(L, name);
            lua_rawget(L, -2);
            if (!lua_isfunction(L, -1)) {
                lua_pop(L, 1);
                continue;
            }
            lua_pushinteger(L, kNil);
            lua_pushinteger(L, -1);
            lua_pushinteger(L, 0);
            lua_pushcclosure(L, shapedCall, 4);
            lua_setfield(L, -2, name);
        }
    }
    lua_pop(L, 1);
    lua_pushcfunction(L, lua_issecure);
    lua_setglobal(L, "issecure");
    lua_pushcfunction(L, lua_issecurevariable);
    lua_setglobal(L, "issecurevariable");
}

int apiResultCount(std::string_view expansionId, std::string_view name) {
    if (expansionId != "tbc") return -1;
    for (const auto& r : kResultCountsTbc)
        if (name == r.name) return r.count;
    return -1;
}

void markInsecureRoot(lua_State* L, const std::string& directory, const std::string& addon) {
    lua_getfield(L, LUA_REGISTRYINDEX, kInsecureRoots);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, kInsecureRoots);
    }
    const std::string root = "@" + directory + (directory.empty() || directory.back() == '/' ? "" : "/");
    lua_pushstring(L, root.c_str());
    lua_pushstring(L, addon.c_str());
    lua_rawset(L, -3);
    lua_pop(L, 1);
}

void markInsecureChunk(lua_State* L, const char* chunk) {
    lua_getfield(L, LUA_REGISTRYINDEX, kInsecureChunks);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, kInsecureChunks);
    }
    lua_pushstring(L, chunk);
    lua_pushboolean(L, 1);
    lua_rawset(L, -3);
    lua_pop(L, 1);
}

} // namespace wowee::addons
