#include "addons/expansion_api_surface.hpp"

extern "C" {
#include "lua.h"
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

} // namespace wowee::addons
