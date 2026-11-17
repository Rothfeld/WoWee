#pragma once

#include "game/entity.hpp"
#include "game/update_field_table.hpp"
#include <cassert>
#include <cstdint>
#include <iterator>
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
/// pick (hover), the name tooltip, the cursor and the click.
struct GameObjectTypeRule {
    uint32_t type;
    const char* name;
    bool use;                   ///< Cursor shows and a click sends a use.
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
    {0,  "DOOR",               true,  ObjectNameTooltip::Default, "Interact",  GameObjectRuleSource::Server, "Use"},
    {1,  "BUTTON",             true,  ObjectNameTooltip::Default, "Interact",  GameObjectRuleSource::Server, "Use"},
    {2,  "QUESTGIVER",         true,  ObjectNameTooltip::Default, "Interact",  GameObjectRuleSource::Server, "Use (gossip); quest marker cursor from its status"},
    {3,  "CHEST",              true,  ObjectNameTooltip::Default, "Interact",  GameObjectRuleSource::Server, "Use; gear natively (Warpwood Pod 179526)"},
    {4,  "BINDER",             false, ObjectNameTooltip::None,    nullptr,     GameObjectRuleSource::Server, "Absent"},
    {5,  "GENERIC",            false, ObjectNameTooltip::None,    nullptr,     GameObjectRuleSource::Server, "Refused; label from data0/data1"},
    {6,  "TRAP",               true,  ObjectNameTooltip::Default, "Interact",  GameObjectRuleSource::NeedsCapture, "Use casts the trap spell"},
    {7,  "CHAIR",              true,  ObjectNameTooltip::None,    "Interact",  GameObjectRuleSource::Server, "Use (sit); no name is WoWee's earlier rule"},
    {8,  "SPELL_FOCUS",        false, ObjectNameTooltip::None,    nullptr,     GameObjectRuleSource::Native, "Forge 1685: no cursor natively"},
    {9,  "TEXT",               true,  ObjectNameTooltip::Default, "Inspect",   GameObjectRuleSource::Server, "Page"},
    {10, "GOOBER",             true,  ObjectNameTooltip::Default, "Interact",  GameObjectRuleSource::Server, "Use"},
    {11, "TRANSPORT",          false, ObjectNameTooltip::None,    nullptr,     GameObjectRuleSource::Server, "Absent; Vator 80023 shows nothing natively"},
    {12, "AREADAMAGE",         false, ObjectNameTooltip::None,    nullptr,     GameObjectRuleSource::Server, "Absent; interaction distance 0"},
    {13, "CAMERA",             true,  ObjectNameTooltip::Default, "Interact",  GameObjectRuleSource::Server, "Use (cinematic/event)"},
    {14, "MAP_OBJECT",         false, ObjectNameTooltip::None,    nullptr,     GameObjectRuleSource::Server, "Absent"},
    {15, "MO_TRANSPORT",       false, ObjectNameTooltip::None,    nullptr,     GameObjectRuleSource::Server, "Absent"},
    {16, "DUEL_ARBITER",       false, ObjectNameTooltip::None,    nullptr,     GameObjectRuleSource::Native, "Duel Flag 21680: no cursor natively"},
    {17, "FISHINGNODE",        true,  ObjectNameTooltip::Default, "Fishing",   GameObjectRuleSource::Server, "Use"},
    {18, "SUMMONING_RITUAL",   true,  ObjectNameTooltip::Default, "Interact",  GameObjectRuleSource::Server, "Use"},
    {19, "MAILBOX",            true,  ObjectNameTooltip::Default, "Mail",      GameObjectRuleSource::Server, "Mail"},
    {20, "AUCTIONHOUSE",       false, ObjectNameTooltip::None,    nullptr,     GameObjectRuleSource::Server, "Absent"},
    {21, "GUARDPOST",          false, ObjectNameTooltip::None,    nullptr,     GameObjectRuleSource::Server, "Absent"},
    {22, "SPELLCASTER",        true,  ObjectNameTooltip::Default, "Interact",  GameObjectRuleSource::Server, "Use"},
    {23, "MEETINGSTONE",       true,  ObjectNameTooltip::Default, "Interact",  GameObjectRuleSource::Server, "Use"},
    {24, "FLAGSTAND",          true,  ObjectNameTooltip::Default, "Interact",  GameObjectRuleSource::Server, "Use"},
    {25, "FISHINGHOLE",        false, ObjectNameTooltip::Default, nullptr,     GameObjectRuleSource::Native, "Oily Blackmouth School 180582: no cursor natively; fished via the spell"},
    {26, "FLAGDROP",           true,  ObjectNameTooltip::Default, "Interact",  GameObjectRuleSource::Server, "Use"},
    {27, "MINI_GAME",          true,  ObjectNameTooltip::Default, "Interact",  GameObjectRuleSource::Native, "Teleport-Stone to Hyjal 211050: gear natively; server ignores the use"},
    {28, "LOTTERY_KIOSK",      false, ObjectNameTooltip::None,    nullptr,     GameObjectRuleSource::Server, "Absent"},
    {29, "CAPTURE_POINT",      false, ObjectNameTooltip::None,    nullptr,     GameObjectRuleSource::Server, "Absent (proximity)"},
    {30, "AURA_GENERATOR",     false, ObjectNameTooltip::None,    nullptr,     GameObjectRuleSource::Server, "Absent"},
    {31, "DUNGEON_DIFFICULTY", false, ObjectNameTooltip::None,    nullptr,     GameObjectRuleSource::Server, "Absent"},
    {32, "BARBER_CHAIR",       false, ObjectNameTooltip::None,    nullptr,     GameObjectRuleSource::Server, "Absent"},
    {33, "DESTRUCTIBLE",       false, ObjectNameTooltip::None,    nullptr,     GameObjectRuleSource::Server, "Absent"},
    {34, "GUILD_BANK",         true,  ObjectNameTooltip::Default, "Buy",       GameObjectRuleSource::Native, "Guild Vault 187299: Buy natively"},
};

/// The row for a type. Types past the table are not usable and unnamed.
inline const GameObjectTypeRule& gameObjectTypeRule(uint32_t type) {
    static constexpr GameObjectTypeRule kUnknown{
        0xFFFFFFFFu, "UNKNOWN", false, ObjectNameTooltip::None, nullptr,
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

} // namespace wowee::game
