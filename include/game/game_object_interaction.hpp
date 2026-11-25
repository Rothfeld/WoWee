#pragma once

#include "game/entity.hpp"
#include "game/update_field_table.hpp"
#include <cassert>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>

namespace wowee::game {

// CMaNGOS GO_FLAG_INTERACT_COND is lifted by the per-player ACTIVATE bit.
// Quest chests and questgiver objects use this instead of a client quest list.
inline bool gameObjectInteractionEnabled(uint32_t flags, uint32_t dynamicFlags) {
    constexpr uint32_t inUse = 0x01, conditional = 0x04, noInteract = 0x10;
    if (flags & (inUse | noInteract)) return false;
    return !(flags & conditional) || (dynamicFlags & 0x01u);
}

inline bool gameObjectInteractionEnabled(const GameObject& object) {
    const auto flags = fieldIndex(UF::GAMEOBJECT_FLAGS);
    const auto dynamic = fieldIndex(UF::GAMEOBJECT_DYNAMIC);
    if (flags == 0xFFFF || dynamic == 0xFFFF)
        throw std::runtime_error("update_fields.json lacks game-object interaction fields; deploy current expansion metadata");
    return gameObjectInteractionEnabled(object.getField(flags), object.getField(dynamic));
}

/// The name tooltip a hovered object shows.
enum class ObjectNameTooltip { None, Floating, Default };

/// Where a row of the type table comes from.
///   Server:       CMaNGOS decides it (GameObject::Use handles the type, a
///                 type-specific handler accepts it, or the use opcode is
///                 refused / never handled for it).
///   Native:       an original 2.4.3 hover decides it (pair 0,
///                 _experiments/20261117T195204Z.pair0_go_cursors).
///   NeedsCapture: the server does not settle what the client shows; the
///                 row keeps WoWee's earlier behaviour until an original
///                 2.4.3 hover is captured.
enum class GameObjectRuleSource { Server, Native, NeedsCapture };

/// What one GAMEOBJECT_TYPE offers the mouse. One table, used by the scene
/// pick (hover), hover highlight, the name tooltip, the cursor and the click.
struct GameObjectTypeRule {
    uint32_t type;
    const char* name;
    bool use;                   ///< Cursor shows and a click sends a use.
    bool highlight;             ///< Model brightens under the pointer (5 & 29 use data[]).
    ObjectNameTooltip tooltip;  ///< GENERIC overrides this from its data.
    const char* cursor;         ///< Interface\Cursor\<cursor>.blp when usable.
    GameObjectRuleSource source;
    const char* why;
};

// Server sources (mangos-tbc):
//   Use     = GameObject::Use switch (GameObject.cpp) handles the type.
//   Refused = HandleGameObjectUseOpcode refuses GENERIC outright.
//   Absent  = no case in GameObject::Use and no other handler: the server
//             logs "unhandled GameObject type" and does nothing.
//   Mail/GuildBank = MailHandler / GuildHandler accept only that type
//             (GetGameObjectIfCanInteractWith).
//   Page    = TEXT is read through CMSG_PAGE_TEXT_QUERY from its pageID.
inline constexpr GameObjectTypeRule kGameObjectTypeRules[] = {
    {0,  "DOOR",               true,  true,  ObjectNameTooltip::Default, "Interact",  GameObjectRuleSource::Server, "Use"},
    {1,  "BUTTON",             true,  true,  ObjectNameTooltip::Default, "Interact",  GameObjectRuleSource::Server, "Use"},
    {2,  "QUESTGIVER",         true,  true,  ObjectNameTooltip::Default, "Interact",  GameObjectRuleSource::Server, "Use (gossip); quest marker cursor from its status"},
    {3,  "CHEST",              true,  true,  ObjectNameTooltip::Default, "Interact",  GameObjectRuleSource::Server, "Use; gear natively (Warpwood Pod 179526)"},
    {4,  "BINDER",             false, false, ObjectNameTooltip::None,    nullptr,     GameObjectRuleSource::Server, "Absent"},
    {5,  "GENERIC",            false, false, ObjectNameTooltip::None,    nullptr,     GameObjectRuleSource::Server, "Refused; label from data0/data1, highlight from data1"},
    {6,  "TRAP",               false, false, ObjectNameTooltip::None,    nullptr,     GameObjectRuleSource::Native, "Not interactive/lit: Silithyst 181603/181618, Drakonid Bones 179804"},
    {7,  "CHAIR",              true,  true,  ObjectNameTooltip::Default, "Interact",  GameObjectRuleSource::Native, "Use (sit); gear and name tooltip natively (Bench 133469, Chair 180046)"},
    {8,  "SPELL_FOCUS",        false, true,  ObjectNameTooltip::Default, nullptr,     GameObjectRuleSource::Native, "Wow.exe 8606 vtable 0x8c48d0 (Slot3=TRUE, Slot5=FALSE): Icebellow Anvil 181234, Forge 1685, Campfire 1798 highlight + default tooltip, no cursor"},
    {9,  "TEXT",               true,  true,  ObjectNameTooltip::Default, "Inspect",   GameObjectRuleSource::Server, "Page"},
    {10, "GOOBER",             true,  true,  ObjectNameTooltip::Default, "Interact",  GameObjectRuleSource::Server, "Use"},
    {11, "TRANSPORT",          false, false, ObjectNameTooltip::None,    nullptr,     GameObjectRuleSource::Server, "Absent; Vator 80023 neither cursor nor highlight natively"},
    {12, "AREADAMAGE",         false, false, ObjectNameTooltip::None,    nullptr,     GameObjectRuleSource::Server, "Absent; interaction distance 0"},
    {13, "CAMERA",             true,  true,  ObjectNameTooltip::Default, "Interact",  GameObjectRuleSource::Server, "Use (cinematic/event)"},
    {14, "MAP_OBJECT",         false, false, ObjectNameTooltip::None,    nullptr,     GameObjectRuleSource::Server, "Absent"},
    {15, "MO_TRANSPORT",       false, false, ObjectNameTooltip::None,    nullptr,     GameObjectRuleSource::Server, "Absent"},
    {16, "DUEL_ARBITER",       false, true,  ObjectNameTooltip::Default, nullptr,     GameObjectRuleSource::Native, "Wow.exe 8606 vtable 0x8c48d0 (Slot3=TRUE, Slot5=FALSE): Duel Flag 21680 highlights + default tooltip, no cursor"},
    {17, "FISHINGNODE",        true,  true,  ObjectNameTooltip::Default, "Interact",  GameObjectRuleSource::Server, "Use; TBC 2.4.3 has no Fishing.blp cursor, bobbers use Interact"},
    {18, "SUMMONING_RITUAL",   true,  true,  ObjectNameTooltip::Default, "Interact",  GameObjectRuleSource::Server, "Use"},
    {19, "MAILBOX",            true,  true,  ObjectNameTooltip::Default, "Mail",      GameObjectRuleSource::Server, "Mail"},
    {20, "AUCTIONHOUSE",       false, false, ObjectNameTooltip::None,    nullptr,     GameObjectRuleSource::Server, "Absent; Gadgetzan Auction 175904 dark natively"},
    {21, "GUARDPOST",          false, false, ObjectNameTooltip::None,    nullptr,     GameObjectRuleSource::Server, "Absent"},
    {22, "SPELLCASTER",        true,  true,  ObjectNameTooltip::Default, "Interact",  GameObjectRuleSource::Server, "Use"},
    {23, "MEETINGSTONE",       true,  true,  ObjectNameTooltip::Default, "Interact",  GameObjectRuleSource::Server, "Use"},
    {24, "FLAGSTAND",          true,  true,  ObjectNameTooltip::Default, "Interact",  GameObjectRuleSource::Server, "Use"},
    {25, "FISHINGHOLE",        false, false, ObjectNameTooltip::Default, nullptr,     GameObjectRuleSource::Native, "Oily Blackmouth School 180582: named, neither cursor nor highlight"},
    {26, "FLAGDROP",           true,  true,  ObjectNameTooltip::Default, "Interact",  GameObjectRuleSource::Server, "Use"},
    {27, "MINI_GAME",          true,  true,  ObjectNameTooltip::Default, "Interact",  GameObjectRuleSource::Native, "Teleport-Stone to Hyjal 211050: gear and highlight natively"},
    {28, "LOTTERY_KIOSK",      false, false, ObjectNameTooltip::None,    nullptr,     GameObjectRuleSource::Server, "Absent"},
    {29, "CAPTURE_POINT",      false, false, ObjectNameTooltip::None,    nullptr,     GameObjectRuleSource::Native, "Wow.exe 8606 vtable 0x8c4820 (Slot3=data[19]!=0, Slot5=FALSE): highlight + default tooltip from data19"},
    {30, "AURA_GENERATOR",     false, true,  ObjectNameTooltip::Default, nullptr,     GameObjectRuleSource::Native, "Wow.exe 8606 vtable 0x8c48d0 (Slot3=TRUE, Slot5=FALSE): Vanguard of the Alliance 183333 highlights + default tooltip, no cursor"},
    {31, "DUNGEON_DIFFICULTY", false, false, ObjectNameTooltip::None,    nullptr,     GameObjectRuleSource::Server, "Absent; heroic portal icon 184128 dark natively"},
    {32, "BARBER_CHAIR",       false, false, ObjectNameTooltip::None,    nullptr,     GameObjectRuleSource::Server, "Absent"},
    {33, "DESTRUCTIBLE",       false, false, ObjectNameTooltip::None,    nullptr,     GameObjectRuleSource::Server, "Absent"},
    {34, "GUILD_BANK",         true,  true,  ObjectNameTooltip::Default, "Buy",       GameObjectRuleSource::Native, "Guild Vault 187299: Buy and highlight natively"},
};

/// The row for a type. Types past the table are not usable and unnamed.
inline const GameObjectTypeRule& gameObjectTypeRule(uint32_t type) {
    static constexpr GameObjectTypeRule kUnknown{
        0xFFFFFFFFu, "UNKNOWN", false, false, ObjectNameTooltip::None, nullptr,
        GameObjectRuleSource::Server, "no such type"};
    if (type >= std::size(kGameObjectTypeRules)) return kUnknown;
    const auto& row = kGameObjectTypeRules[type];
    assert(row.type == type);
    return row;
}

/// Name tooltip from Wow.exe build 8606 CGGameObject_C_Type Slot 3 (CanHover)
/// and CGGameObject_C::HasFloatingTooltip (propId 0x13 = GENERIC data[0]).
///   - GENERIC (5): data[0] floatingTooltip -> Floating; data[1] highlight -> Default; else None.
///   - CAPTURE_POINT (29): data[19] highlight -> Default; else None.
///   - All other types: their row's tooltip (SPELL_FOCUS 8, DUEL_ARBITER 16,
///     FISHINGHOLE 25, AURA_GENERATOR 30, and all usable types -> Default).
inline ObjectNameTooltip objectNameTooltip(uint32_t type, std::span<const uint32_t> data) {
    if (type == 5) {
        if (data.size() > 0 && data[0] != 0) return ObjectNameTooltip::Floating;
        return (data.size() > 1 && data[1] != 0) ? ObjectNameTooltip::Default : ObjectNameTooltip::None;
    }
    if (type == 29) {
        return (data.size() > 19 && data[19] != 0) ? ObjectNameTooltip::Default : ObjectNameTooltip::None;
    }
    return gameObjectTypeRule(type).tooltip;
}

inline ObjectNameTooltip objectNameTooltip(uint32_t type, uint32_t data0, uint32_t data1) {
    const uint32_t d[2] = {data0, data1};
    return objectNameTooltip(type, d);
}

/// Whether 2.4.3 lights the object under the pointer. Measured on build 8606
/// (pair 5 & pair 15): usable types plus SPELL_FOCUS (Campfire 1798, Forge
/// 1685), DUEL_ARBITER (Duel Flag 21680) and AURA_GENERATOR (Vanguard of the
/// Alliance 183333) brighten; GENERIC (5) and CAPTURE_POINT (29) consult their
/// template highlight field (CMaNGOS GameObject.h _generic.highlight = data[1],
/// capturePoint.highlight = data[19]); TRAP (6: Silithyst pillar 181618,
/// Silithyst extractor 181603, Drakonid Bones 179804), TRANSPORT (11),
/// AUCTIONHOUSE (20), FISHINGHOLE (25), DUNGEON_DIFFICULTY (31) and unused
/// server types stay dark.
inline bool gameObjectHighlights(uint32_t type, std::span<const uint32_t> data) {
    if (type == 5) return data.size() > 1 && data[1] != 0;
    if (type == 29) return data.size() > 19 && data[19] != 0;
    return gameObjectTypeRule(type).highlight;
}

/// What the mouse gets from one object: whether a click uses it, which name
/// tooltip it shows, and nothing at all when neither (the pick passes
/// through it). `available` is the flag/faction/ghost gate
/// (GameHandler::canInteractWithGameObject); an unavailable object offers
/// nothing.
struct GameObjectAffordance {
    bool use = false;
    ObjectNameTooltip tooltip = ObjectNameTooltip::None;
    const char* cursor = nullptr;
    [[nodiscard]] bool hoverable() const { return use || tooltip != ObjectNameTooltip::None; }
};

inline GameObjectAffordance gameObjectAffordance(uint32_t type, std::span<const uint32_t> data,
                                                 bool available) {
    if (!available) return {};
    const auto& rule = gameObjectTypeRule(type);
    return {rule.use, objectNameTooltip(type, data), rule.cursor};
}

inline GameObjectAffordance gameObjectAffordance(uint32_t type, uint32_t data0, uint32_t data1,
                                                 bool available) {
    const uint32_t d[2] = {data0, data1};
    return gameObjectAffordance(type, d, available);
}

/// The model animation SMSG_GAMEOBJECT_CUSTOM_ANIM's index (0-3) plays:
/// AnimationData Custom0-Custom3, ids 153-156. nullopt outside 0-3.
inline std::optional<uint32_t> gameObjectCustomAnimation(uint32_t index) {
    constexpr uint32_t kCustom0 = 153;
    if (index > 3) return std::nullopt;
    return kCustom0 + index;
}

/// Normalize the SMSG_GAMEOBJECT_CUSTOM_ANIM payload to a Custom0-3 slot index
/// (0-3) before passing to gameObjectCustomAnimation().
///
/// Fishing bobbers (GAMEOBJECT_TYPE_FISHINGNODE = 17, G_FishingBobber.m2) use
/// Custom0 (slot 0 -> AnimationData 153) for their bite/splash animation and
/// water splash particle emitter; CMaNGOS sends 0 while AzerothCore sends
/// animation progress (100), so fishing bobbers always map to Custom0 (0).
inline uint32_t normalizeGameObjectCustomAnimSlot(uint32_t goType, uint32_t packetAnimId) {
    constexpr uint32_t kGameObjectTypeFishingNode = 17u;
    if (goType == kGameObjectTypeFishingNode) return 0u;
    return packetAnimId;
}

/// Client-local GameObject open/close transition when a reader, gossip, or
/// questgiver dialog opens on `newGuid` (0 when no dialog is open).
///
/// Native 2.4.3 opens interactive GameObjects locally (GO_STATE_ACTIVE = 0,
/// playing Open 148 -> Opened 149) while the dialog is shown even though the
/// server keeps GAMEOBJECT_STATE = READY (1) (e.g. The Codex of Blood 182947,
/// type 2 QUESTGIVER, displayId 6892). Page turns or Gossip -> QuestDetails
/// transitions on the same GameObject keep it open without replaying Open/Close.
/// Closing the dialog (or switching to another object) restores the previous
/// GameObject to its server state (playing Close 146 -> Closed 147 when READY).
struct GameObjectDialogTransition {
    uint64_t closeGuid = 0;  ///< Previous GameObject to restore to server state (0 = none)
    uint64_t openGuid = 0;   ///< New GameObject to open locally to state 0 (0 = none)
};

inline GameObjectDialogTransition resolveGameObjectDialogTransition(uint64_t currentGuid,
                                                                    uint64_t newGuid) {
    if (currentGuid == newGuid) return {};
    return {currentGuid, newGuid};
}

/// Whether a GameObject state update (`newState`) should trigger a state
/// animation and sound transition given the last applied/known state.
///
/// Native 2.4.3 ignores redundant state publications where the state byte did
/// not change (e.g. CMaNGOS `Player::SetGameMaster` re-sending
/// `UPDATETYPE_VALUES` with `GAMEOBJECT_STATE = 1` for every visible door on
/// `.gm on`/`.gm off`, or `GAMEOBJECT_BYTES_1` updates where only `artKit` or
/// `animProgress` changed). Initial spawn (`!appliedState`) always applies.
inline constexpr bool shouldApplyGameObjectState(std::optional<uint8_t> appliedState,
                                                 uint8_t newState) {
    return !appliedState.has_value() || *appliedState != newState;
}

/// Action to take when SMSG_GAMEOBJECT_DESPAWN_ANIM (0x215) arrives for a
/// spawned GameObject.
///
/// - M2 GameObjects authoring sequence 157 (AnimationData::Despawn) — e.g.
///   Naxxramas_Frostwyrm_Birth.m2 (Sapphiron Birth, 36.67 s),
///   FrostwurmFellfire_Birth.m2 (36.67 s), g_necropolis.m2 (14.7 s),
///   beerfest_molemachine.m2 (4.67 s), ahn_quiraj_ossiriancrystal.m2 (3.9 s) —
///   play Despawn (157) to completion even when OUT_OF_RANGE arrives in the
///   same server tick (CMaNGOS GameObject::Update GO_JUST_DEACTIVATED).
/// - WMOs and M2s lacking sequence 157 (e.g. PORTCULLISACTIVE.m2 Kirtonos gate
///   175570, WROUGHTIRONDOOR02.m2, g_fishingbobber.m2) do nothing on 0x215:
///   CMaNGOS sends SMSG_GAMEOBJECT_DESPAWN_ANIM on static DB doors/buttons/
///   goobers when GO_JUST_DEACTIVATED fires with animProgress > 0 without
///   despawning them, followed by GAMEOBJECT_STATE = READY (1). Actual object
///   removal and fade-out are driven strictly by SMSG_UPDATE_OBJECT
///   (OUT_OF_RANGE -> despawnGameObjectFromServer).
enum class GameObjectDespawnAnimAction : uint8_t {
    PLAY_DESPAWN_ANIM,
    NO_OP,
};

inline constexpr GameObjectDespawnAnimAction resolveGameObjectDespawnAnim(
    bool isWmo, bool hasDespawnSequence) {
    if (!isWmo && hasDespawnSequence) {
        return GameObjectDespawnAnimAction::PLAY_DESPAWN_ANIM;
    }
    return GameObjectDespawnAnimAction::NO_OP;
}

} // namespace wowee::game
