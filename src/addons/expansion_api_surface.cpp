#include "addons/expansion_api_surface.hpp"

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
    "CanMerchantRepair", "CanSummonFriend", "CanViewOfficerNote",
    "CheckInteractDistance", "CursorCanGoInSlot", "CursorHasItem", "CursorHasSpell",
    "GetActionBarToggles", "GetAddOnInfo", "GetBattlefieldStatus",
    "GetChannelDisplayInfo", "GetContainerItemInfo", "GetCraftFilter", "GetFactionInfo",
    "GetGuildRosterShowOffline", "GetInventoryItemBroken", "GetOptOutOfLoot",
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
    "IsOutdoors", "IsQuestCompletable", "IsQuestWatched", "IsResting",
    "IsRightAltKeyDown", "IsRightControlKeyDown", "IsRightShiftKeyDown",
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

enum ShapeBits : int { kFlags = 1 };

/// The global replaced by a closure over the original. Upvalues: 1 the
/// original function, 2 ShapeBits, 3 most values (-1 any).
int shapedCall(lua_State* L) {
    const int bits = static_cast<int>(lua_tointeger(L, lua_upvalueindex(2)));
    const int most = static_cast<int>(lua_tointeger(L, lua_upvalueindex(3)));
    const int nargs = lua_gettop(L);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, nargs, LUA_MULTRET);
    int n = lua_gettop(L);
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

struct Shape { int bits = 0; int most = -1; };

void shapeGlobal(lua_State* L, const char* name, const Shape& shape) {
    lua_getglobal(L, name);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    lua_pushinteger(L, shape.bits);
    lua_pushinteger(L, shape.most);
    lua_pushcclosure(L, shapedCall, 3);
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
            lua_pushcclosure(L, shapedCall, 3);
            lua_setfield(L, -2, name);
        }
    }
    lua_pop(L, 1);
}

} // namespace wowee::addons
