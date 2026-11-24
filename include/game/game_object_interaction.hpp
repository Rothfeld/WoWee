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
    {8,  "SPELL_FOCUS",        false, true,  ObjectNameTooltip::None,    nullptr,     GameObjectRuleSource::Native, "Campfire 1798, Forge 1685: lit natively, no cursor"},
    {9,  "TEXT",               true,  true,  ObjectNameTooltip::Default, "Inspect",   GameObjectRuleSource::Server, "Page"},
    {10, "GOOBER",             true,  true,  ObjectNameTooltip::Default, "Interact",  GameObjectRuleSource::Server, "Use"},
    {11, "TRANSPORT",          false, false, ObjectNameTooltip::None,    nullptr,     GameObjectRuleSource::Server, "Absent; Vator 80023 neither cursor nor highlight natively"},
    {12, "AREADAMAGE",         false, false, ObjectNameTooltip::None,    nullptr,     GameObjectRuleSource::Server, "Absent; interaction distance 0"},
    {13, "CAMERA",             true,  true,  ObjectNameTooltip::Default, "Interact",  GameObjectRuleSource::Server, "Use (cinematic/event)"},
    {14, "MAP_OBJECT",         false, false, ObjectNameTooltip::None,    nullptr,     GameObjectRuleSource::Server, "Absent"},
    {15, "MO_TRANSPORT",       false, false, ObjectNameTooltip::None,    nullptr,     GameObjectRuleSource::Server, "Absent"},
    {16, "DUEL_ARBITER",       false, true,  ObjectNameTooltip::None,    nullptr,     GameObjectRuleSource::Native, "Duel Flag 21680: lit natively (1.66x), no cursor"},
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
    {29, "CAPTURE_POINT",      false, false, ObjectNameTooltip::None,    nullptr,     GameObjectRuleSource::Server, "Absent (proximity); highlight from data19"},
    {30, "AURA_GENERATOR",     false, true,  ObjectNameTooltip::None,    nullptr,     GameObjectRuleSource::Server, "Absent; Vanguard of the Alliance 183333 lit natively (2.04x)"},
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

/// GAMEOBJECT_TYPE_GENERIC (5) data0 floatingTooltip, data1 highlight
/// (CMaNGOS GameObject.h _generic). floatingTooltip labels the object at the
/// cursor; highlight alone (Guild Master & Tabards, 183865) shows its name in
/// the default bottom-right tooltip; neither is unlabelled scenery. Every
/// other type takes its row's tooltip.
inline ObjectNameTooltip objectNameTooltip(uint32_t type, uint32_t data0, uint32_t data1) {
    if (type != 5) return gameObjectTypeRule(type).tooltip;
    if (data0 != 0) return ObjectNameTooltip::Floating;
    return data1 != 0 ? ObjectNameTooltip::Default : ObjectNameTooltip::None;
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

inline GameObjectAffordance gameObjectAffordance(uint32_t type, uint32_t data0, uint32_t data1,
                                                 bool available) {
    if (!available) return {};
    const auto& rule = gameObjectTypeRule(type);
    return {rule.use, objectNameTooltip(type, data0, data1), rule.cursor};
}

/// The model animation SMSG_GAMEOBJECT_CUSTOM_ANIM's index (0-3) plays:
/// AnimationData Custom0-Custom3, ids 153-156. nullopt outside 0-3.
inline std::optional<uint32_t> gameObjectCustomAnimation(uint32_t index) {
    constexpr uint32_t kCustom0 = 153;
    if (index > 3) return std::nullopt;
    return kCustom0 + index;
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

} // namespace wowee::game
