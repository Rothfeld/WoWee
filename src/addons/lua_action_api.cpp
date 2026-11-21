#include "game/spell_prerequisites.hpp"
#include "game/shapeshift_forms.hpp"
// lua_action_api.cpp - Action bar, cursor/pickup, keyboard input, key bindings, and pet actions Lua API bindings.
// Extracted from lua_engine.cpp as part of §5.1 (Tame LuaEngine).
#include "game/item_text.hpp"
#include "addons/macro_target.hpp"
#include "game/game_utils.hpp"
#include "ui/chat/macro_evaluator.hpp"
#include "addons/lua_api_helpers.hpp"
#include "addons/lua_api_registrations.hpp"
// For fireEvent: paging has to reach the frames as well as the addons.
#include "addons/lua_engine.hpp"
#include "game/inventory_slots.hpp"
#include "ui/framexml_takeover.hpp"
#include "ui/keybinding_manager.hpp"
#include "ui/settings_panel.hpp"
#include "ui/gamepad_controls.hpp"

#include "core/config_paths.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/dbc_loader.hpp"
#include "core/logger.hpp"
#include "core/application.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/ui_sound_manager.hpp"
#include <algorithm>
#include <optional>
#include <cctype>
#include <filesystem>
#include <set>
#include "game/current_action.hpp"
#include "game/pet_action.hpp"
#include <SDL3/SDL_keyboard.h>

namespace wowee::addons {

// What the interface is holding.
//
// This is the original interface's cursor, and it is not the only one: this
// client's inventory screen keeps a held item of its own. They do not need
// merging, because whichever interface is drawing the bags is the one the
// player can pick anything up from, and that is the cursor in use - with the
// bags owned, as they are by default, this is it.
//
// What that does mean is that CursorHasItem answers for this cursor alone. An
// addon asking while the player is dragging in this client's own bags is told
// no, which is true of the cursor it can see and not of the player's hand.
// MERCHANT is a fifth kind and it is not an item: what the cursor holds is a
// vendor's list index, not something the player owns. FrameXML's merchant
// window puts one there on every left-click - PickupMerchantItem - and buying
// is what happens when it is dropped into a bag, so without it a left-click at
// a vendor did nothing at all and only right-click bought.

namespace {
std::string lowerCopy(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string trimCopy(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r");
    if (first == std::string::npos) return {};
    return s.substr(first, s.find_last_not_of(" \t\r") - first + 1);
}

/// A known spell by "Name" (the highest rank) or "Name(Rank N)" (that rank
/// only), case-insensitive; 0 when not known.
uint32_t knownSpellNamed(game::GameHandler& gh, const std::string& text) {
    std::string name = text;
    int rank = -1;
    if (const auto open = text.find('('); open != std::string::npos) {
        name = trimCopy(text.substr(0, open));
        const std::string inner = lowerCopy(text.substr(open + 1, text.find(')', open) - open - 1));
        if (inner.rfind("rank ", 0) != 0) return 0;
        rank = std::atoi(inner.c_str() + 5);
    }
    name = lowerCopy(name);
    uint32_t best = 0;
    int bestRank = -1;
    for (const uint32_t id : gh.getKnownSpells()) {
        if (lowerCopy(gh.getSpellName(id)) != name) continue;
        const std::string r = lowerCopy(gh.getSpellRank(id));
        const int n = r.rfind("rank ", 0) == 0 ? std::atoi(r.c_str() + 5) : 0;
        if (rank >= 0 ? n == rank : n > bestRank) { best = id; bestRank = n; }
    }
    return best;
}

/// A carried or equipped item by name, case-insensitive; 0 when none.
uint32_t carriedItemNamed(game::GameHandler& gh, const std::string& text) {
    const std::string name = lowerCopy(text);
    const auto matches = [&](const game::ItemSlot& slot) {
        if (slot.empty()) return false;
        const auto* info = gh.getItemInfo(slot.item.itemId);
        return info && info->valid && lowerCopy(info->name) == name;
    };
    const auto& inv = gh.getInventory();
    for (int e = 0; e < game::Inventory::NUM_EQUIP_SLOTS; ++e)
        if (matches(inv.getEquipSlot(static_cast<game::EquipSlot>(e)))) return inv.getEquipSlot(static_cast<game::EquipSlot>(e)).item.itemId;
    for (int s = 0; s < inv.getBackpackSize(); ++s)
        if (matches(inv.getBackpackSlot(s))) return inv.getBackpackSlot(s).item.itemId;
    for (int b = 0; b < game::Inventory::NUM_BAG_SLOTS; ++b)
        for (int s = 0; s < inv.getBagSize(b); ++s)
            if (matches(inv.getBagSlot(b, s))) return inv.getBagSlot(b, s).item.itemId;
    return 0;
}

/// One /cast, /use or #showtooltip argument after its conditions.
MacroTarget targetOf(game::GameHandler& gh, std::string arg, bool use) {
    arg = trimCopy(arg);
    if (!arg.empty() && arg.front() == '!') arg.erase(arg.begin());
    MacroTarget out;
    out.kind = MacroTarget::Kind::Unresolved;
    if (arg.empty()) return out;
    // "/use 13": an equipment slot, 1-based.
    if (use && std::all_of(arg.begin(), arg.end(), [](unsigned char c) { return std::isdigit(c); })) {
        const int slot = std::atoi(arg.c_str());
        if (slot >= 1 && slot <= game::Inventory::NUM_EQUIP_SLOTS) {
            const auto& equipped = gh.getInventory().getEquipSlot(static_cast<game::EquipSlot>(slot - 1));
            if (!equipped.empty()) { out.kind = MacroTarget::Kind::Item; out.id = equipped.item.itemId; }
        }
        return out;
    }
    // /use names an item before a spell ("/use Linen Bandage" is the
    // bandage, not the First Aid recipe of that name); /cast the reverse.
    const uint32_t item = carriedItemNamed(gh, arg);
    if (use && item) { out.kind = MacroTarget::Kind::Item; out.id = item; return out; }
    if (const uint32_t spell = knownSpellNamed(gh, arg)) {
        out.kind = MacroTarget::Kind::Spell; out.id = spell;
    } else if (item) {
        out.kind = MacroTarget::Kind::Item; out.id = item;
    }
    return out;
}

/// The argument with its [conditions] evaluated; nullopt when none holds.
std::optional<std::string> conditioned(game::GameHandler& gh, const std::string& raw) {
    const std::string arg = trimCopy(raw);
    if (arg.empty() || arg.front() != '[') {
        // "A; B" without conditions is A.
        return trimCopy(arg.substr(0, arg.find(';')));
    }
    uint64_t target = static_cast<uint64_t>(-1);
    std::string chosen = trimCopy(ui::evaluateMacroConditionals(arg, gh, target));
    if (chosen.empty()) return std::nullopt;
    return chosen;
}
}  // namespace

MacroTarget resolveMacroTarget(game::GameHandler& gh, uint32_t macroId) {
    MacroTarget result;
    const std::string& body = gh.getMacroText(macroId);
    size_t pos = 0;
    while (pos <= body.size()) {
        const size_t end = body.find('\n', pos);
        const std::string line = trimCopy(body.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
        pos = end == std::string::npos ? body.size() + 1 : end + 1;
        const std::string lower = lowerCopy(line);
        const auto argAfter = [&](size_t n) { return line.size() > n ? line.substr(n) : std::string(); };
        if (lower.rfind("#showtooltip", 0) == 0 || lower.rfind("#show", 0) == 0) {
            const bool tooltip = lower.rfind("#showtooltip", 0) == 0;
            result.showTooltip = result.showTooltip || tooltip;
            const std::string arg = trimCopy(argAfter(tooltip ? 12 : 5));
            if (arg.empty()) continue;
            if (const auto chosen = conditioned(gh, arg)) {
                MacroTarget t = targetOf(gh, *chosen, true);
                t.showTooltip = result.showTooltip;
                return t;
            }
            continue;
        }
        bool use = false;
        std::string arg;
        if (lower.rfind("/cast ", 0) == 0) arg = argAfter(6);
        else if (lower.rfind("/use ", 0) == 0) { arg = argAfter(5); use = true; }
        else if (lower.rfind("/castsequence ", 0) == 0) {
            // The sequence's first entry, after any reset= clause.
            arg = trimCopy(argAfter(14));
            const auto chosen = conditioned(gh, arg);
            if (!chosen) continue;
            std::string seq = *chosen;
            if (lowerCopy(seq).rfind("reset=", 0) == 0) {
                const auto space = seq.find(' ');
                seq = space == std::string::npos ? std::string() : seq.substr(space + 1);
            }
            MacroTarget t = targetOf(gh, seq.substr(0, seq.find(',')), false);
            t.showTooltip = result.showTooltip;
            return t;
        } else continue;
        const auto chosen = conditioned(gh, arg);
        if (!chosen) continue;
        MacroTarget t = targetOf(gh, *chosen, use);
        t.showTooltip = result.showTooltip;
        return t;
    }
    return result;
}

std::string macroTexture(game::GameHandler& gh, uint32_t macroId) {
    static const std::string kQuestionMark = "Interface\\Icons\\INV_Misc_QuestionMark";
    const std::string& icon = gh.getMacroIcon(macroId);
    if (!icon.empty() && lowerCopy(icon) != lowerCopy(kQuestionMark)) return icon;
    const MacroTarget target = resolveMacroTarget(gh, macroId);
    if (target.kind == MacroTarget::Kind::Spell) {
        const std::string spellIcon = game::actionSpellIcon(gh, target.id);
        if (!spellIcon.empty()) return spellIcon;
    } else if (target.kind == MacroTarget::Kind::Item) {
        gh.ensureItemInfo(target.id);
        if (const auto* info = gh.getItemInfo(target.id); info && info->displayInfoId != 0) {
            const std::string itemIcon = gh.getItemIconPath(info->displayInfoId);
            if (!itemIcon.empty()) return itemIcon;
        }
    }
    return kQuestionMark;
}


/// GetMacroIconInfo(index) as a path, empty past the end.
/// The macro the first argument names: its index, or its name as 2.4.3 also
/// takes it (GetMacroInfo("Decursive") answers that macro there, and
/// Decursive edits its macro with EditMacro(name, ...)). 0 when none.
static uint32_t macroIdArg(lua_State* L, game::GameHandler* gh) {
    if (!gh) return 0;
    if (lua_type(L, 1) == LUA_TSTRING && !lua_isnumber(L, 1)) {
        const std::string name = lua_tostring(L, 1);
        for (uint32_t id : gh->getMacroIds())
            if (gh->getMacroName(id) == name) return id;
        return 0;
    }
    return gh->macroIdForIndex(static_cast<uint32_t>(luaL_optnumber(L, 1, 0)));
}

static std::string macroIconByIndex(lua_State* L, int index) {
    auto* services = getLuaServices(L);
    if (!services || !services->listIconTextures) return {};
    const auto& icons = services->listIconTextures();
    if (index < 1 || index > static_cast<int>(icons.size())) return {};
    return icons[static_cast<size_t>(index) - 1];
}

/// A bar slot as its macro's spell or item, for the questions the button
/// asks; nullopt when the macro names neither.
static std::optional<game::ActionBarSlot> macroStandIn(game::GameHandler& gh, const MacroTarget& target) {
    game::ActionBarSlot slot;
    if (target.kind == MacroTarget::Kind::Spell) {
        slot.type = game::ActionBarSlot::SPELL;
        slot.id = target.id;
        slot.cooldownRemaining = gh.getSpellCooldown(target.id);
        slot.cooldownTotal = gh.getSpellCooldownTotal(target.id);
        return slot;
    }
    if (target.kind == MacroTarget::Kind::Item) {
        slot.type = game::ActionBarSlot::ITEM;
        slot.id = target.id;
        return slot;
    }
    return std::nullopt;
}

enum class CursorType { NONE, SPELL, ITEM, MACRO, MERCHANT, MONEY, GUILDBANK, PET_ACTION, STABLE_PET };
static CursorType s_cursorType = CursorType::NONE;
static uint32_t   s_cursorId   = 0;    // spellId, itemId, or action slot
static uint32_t   s_cursorPetPacked = 0; // packed pet action when CursorType::PET_ACTION
static int        s_cursorSlot = 0;    // source slot for placement
/// Which container the cursor's item came out of, and one of the four values
/// game::slots documents: 0 the backpack, 1-4 a worn bag, -1 the paperdoll,
/// kCursorNoSource the action bar. See cursorSourceIsInventory.
static int        s_cursorBag  = -1;
static uint64_t   s_cursorMoney = 0;   // copper, when the cursor carries money
/// How much of a guild bank stack the cursor took, or zero for all of it. The
/// wire carries the split on the *withdrawal*, not on the pickup, so the amount
/// has to wait on the cursor until the drop says where it is going.
static uint32_t   s_cursorSplit = 0;
static void setCursorType(lua_State* L, CursorType type);
static void clearCursorItem(lua_State* L);
static void returnCursor(lua_State* L);
/// Where the cursor's item came from, in wire bag/slot space. Declared here
/// because the guild bank pickup above needs it and it is defined further down
/// with the other cursor helpers.
static bool cursorWireSlot(uint8_t& bag, uint8_t& slot);
/// Whether a drop on an inventory slot is a move at all, and puts the cursor
/// down when it is not. See kCursorNoSource: an item off the action bar has
/// nowhere to be moved from, so dropping it in a bag or on the paperdoll does
/// nothing but end the drag, which is what the real client does.
static bool droppedItemFromNowhere(lua_State* L);
/// Sell what the cursor is holding. Defined below, beside the buy it mirrors.
static void soldHeldItem(lua_State* L);

uint64_t cursorMoney() {
    return s_cursorType == CursorType::MONEY ? s_cursorMoney : 0;
}

void setCursorMoney(lua_State* L, uint64_t copper) {
    // Through setCursorType rather than by assignment: it is what fires
    // CURSOR_UPDATE and what shows the action bars' empty slots. Setting the
    // type by hand would put money on the cursor with nothing told about it.
    s_cursorMoney = copper;
    if (copper == 0) {
        if (s_cursorType == CursorType::MONEY) setCursorType(L, CursorType::NONE);
        return;
    }
    s_cursorId = 0;
    s_cursorSlot = 0;
    s_cursorBag = -1;
    setCursorType(L, CursorType::MONEY);
}

int cursorStablePetSlot() {
    return s_cursorType == CursorType::STABLE_PET ? s_cursorSlot : -1;
}

void setCursorStablePet(lua_State* L, int slot, uint32_t creatureEntry) {
    if (slot < 0) {
        if (s_cursorType == CursorType::STABLE_PET) clearCursorItem(L);
        return;
    }
    if (s_cursorType != CursorType::NONE && s_cursorType != CursorType::STABLE_PET) {
        returnCursor(L);
    }
    s_cursorMoney = 0;
    s_cursorSplit = 0;
    s_cursorId = creatureEntry;
    s_cursorSlot = slot;
    s_cursorBag = -1;
    if (s_cursorType == CursorType::STABLE_PET) {
        if (auto* gh = getGameHandler(L)) gh->fireAddonEvent("CURSOR_UPDATE", {});
    } else {
        setCursorType(L, CursorType::STABLE_PET);
    }
}

uint32_t cursorItemId() {
    return s_cursorType == CursorType::ITEM ? s_cursorId : 0;
}

int cursorEquipSlot() {
    // s_cursorBag is -1 only for a pickup off the paperdoll, where s_cursorSlot
    // is the one-based slot the item came out of.
    return (s_cursorType == CursorType::ITEM && s_cursorBag == -1) ? s_cursorSlot : 0;
}

bool cursorItemCameFromInventory() {
    // The same test cursorWireSlot makes before it will name a source, because
    // it is the same question: an item the wire can point at is an item that
    // can be destroyed.
    return s_cursorType == CursorType::ITEM &&
           game::slots::cursorSourceIsInventory(s_cursorBag);
}

bool cursorHoldsAction() {
    if (s_cursorId == 0) return false;
    return s_cursorType == CursorType::SPELL || s_cursorType == CursorType::MACRO ||
           (s_cursorType == CursorType::ITEM && !game::slots::cursorSourceIsInventory(s_cursorBag));
}

/// PickupPetAction(slot) - pick up, place, or swap a pet bar slot (1..10).
///
/// Mirrors PickupAction: empty cursor lifts the slot; carrying a pet action
/// swaps source and destination via CMSG_PET_SET_ACTION (24-byte 2-slot form);
/// carrying a pet spell from SpellBookFrame places it into an empty or spell
/// slot via CMSG_PET_SET_ACTION (16-byte 1-slot form).
static int lua_PickupPetAction(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh || !gh->hasPet()) return 0;
    const int slot = static_cast<int>(luaL_optnumber(L, 1, 0));
    if (slot < 1 || slot > game::GameHandler::PET_ACTION_BAR_SLOTS) return 0;
    const uint32_t existing = gh->getPetActionSlot(slot - 1);

    if (s_cursorType == CursorType::PET_ACTION) {
        const int srcSlot = s_cursorSlot;
        const uint32_t heldPacked = s_cursorPetPacked;
        clearCursorItem(L);
        if (srcSlot >= 1 && srcSlot <= game::GameHandler::PET_ACTION_BAR_SLOTS && srcSlot != slot) {
            gh->petSetAction(static_cast<uint32_t>(slot - 1), heldPacked,
                             true, static_cast<uint32_t>(srcSlot - 1), existing);
        }
        return 0;
    }

    if (s_cursorType == CursorType::SPELL && s_cursorId != 0) {
        const uint32_t spellId = s_cursorId;
        const auto& petSpells = gh->getPetSpells();
        const bool isPetSpell = std::find(petSpells.begin(), petSpells.end(), spellId) != petSpells.end();
        if (!isPetSpell) return 0;
        // Commands and stances cannot be overwritten by a single-slot drop.
        if (existing != 0 && !game::pet::isPetSpellAction(existing)) return 0;
        const uint32_t packed = game::pet::packPetAction(
            gh->isPetSpellAutocast(spellId) ? game::pet::ActionType::Enabled
                                            : game::pet::ActionType::Disabled,
            spellId);
        clearCursorItem(L);
        gh->petSetAction(static_cast<uint32_t>(slot - 1), packed);
        return 0;
    }

    if (s_cursorType != CursorType::NONE || existing == 0) return 0;
    s_cursorId = game::pet::petActionId(existing);
    s_cursorPetPacked = existing;
    s_cursorSlot = slot;
    s_cursorBag = -1;
    setCursorType(L, CursorType::PET_ACTION);
    return 0;
}

/// Change what the cursor is holding, and say so.
///
/// CURSOR_UPDATE is how the interface learns the cursor picked something up or
/// put it down - action buttons redraw their highlight on it, and the
/// equipment and container frames test the cursor from it. Thirteen places set
/// this and none of them said anything, so the cursor changed silently.
///
/// Every one of those thirteen goes through here. Converting some and not
/// others would give a cursor that announces half its changes, which is harder
/// to reason about than one that announces none.
///
/// Only on an actual change: several of these run on every click of an empty
/// slot, setting NONE over NONE, and the interface would redraw for each.
/// Defined below, beside the rest of the cursor state it clears. Declared here
/// because the sites that put the cursor down come first in the file, and each
/// of them used to clear only part of what a pickup sets.
static void clearCursorItem(lua_State* L);

/// Whether holding this shows the action bars' empty slots. Money is the one
/// thing on the cursor that does not: 2.4.3 fires no ACTIONBAR_SHOWGRID for
/// PickupPlayerMoney and none of the grid's buttons would take it.
static bool showsActionGrid(CursorType type) {
    return type != CursorType::NONE && type != CursorType::MONEY &&
           type != CursorType::PET_ACTION && type != CursorType::STABLE_PET;
}

static void setCursorType(lua_State* L, CursorType type) {
    if (s_cursorType == type) return;
    // Putting anything down forgets the guild bank split. It rides on the
    // cursor between the pickup and the drop, and a leftover would be applied
    // to the next withdrawal - a stack the player never asked to divide.
    if (type != CursorType::GUILDBANK) s_cursorSplit = 0;
    if (type != CursorType::PET_ACTION) s_cursorPetPacked = 0;
    const bool wasGrid = showsActionGrid(s_cursorType);
    const bool nowGrid = showsActionGrid(type);
    const bool wasPetGrid = s_cursorType == CursorType::PET_ACTION;
    const bool nowPetGrid = type == CursorType::PET_ACTION;
    s_cursorType = type;
    lua_getfield(L, LUA_REGISTRYINDEX, "wowee_lua_engine");
    auto* engine = static_cast<LuaEngine*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (!engine) return;
    // Picking something up shows the empty slots of the action bars, so there
    // is somewhere visible to drop it; putting it down hides them again. Every
    // action button registers for both, and both are fired on the change
    // rather than on every set: this function is called with the same type
    // repeatedly and the grid would flicker.
    //
    // In 2.4.3's order: a pickup says CURSOR_UPDATE and then
    // ACTIONBAR_SHOWGRID, a put-down ACTIONBAR_HIDEGRID and then CURSOR_UPDATE.
    if (wasGrid && !nowGrid) engine->fireEvent("ACTIONBAR_HIDEGRID", {});
    if (wasPetGrid && !nowPetGrid) engine->fireEvent("PET_BAR_HIDEGRID", {});
    engine->fireEvent("CURSOR_UPDATE", {});
    if (nowGrid && !wasGrid) engine->fireEvent("ACTIONBAR_SHOWGRID", {});
    if (nowPetGrid && !wasPetGrid) engine->fireEvent("PET_BAR_SHOWGRID", {});
}

// Bonus bar five while the player possesses a unit: action slots 121-130 are
// the possessed unit's ten actions from SMSG_PET_SPELLS (build 8606 under
// Mind Control: HasAction true for 121-130, GetBonusBarOffset() 5). A spell
// there is cast by the possessed unit.
static bool possessSlot(game::GameHandler* gh, int slot1, uint32_t& packed) {
    if (!gh || slot1 < 121 || slot1 > 120 + game::GameHandler::PET_ACTION_BAR_SLOTS) return false;
    if (gh->getPossessedGuid() == 0) return false;
    packed = gh->getPetActionSlot(slot1 - 121);
    return true;
}
static uint32_t possessSpell(uint32_t packed) {
    const auto type = game::pet::petActionType(packed);
    if (type == game::pet::ActionType::Command || type == game::pet::ActionType::Reaction) return 0;
    return game::pet::petActionId(packed);
}
// The icon of a possessed unit's command or stance: build 8606 controlling a
// Human Footman draws Ability_GhoulFrenzy, Ability_Tracking,
// Spell_Nature_TimeStop (attack, follow, stay) and Ability_Racial_BloodRage,
// Ability_Defend, Ability_Seal (aggressive, defensive, passive) - the
// PET_*_TEXTURE globals.
static const char* possessTokenTexture(uint32_t packed) {
    const uint32_t action = game::pet::petActionId(packed);
    switch (game::pet::petActionType(packed)) {
        case game::pet::ActionType::Command:
            switch (action) {
                case game::pet::kStay: return "PET_WAIT_TEXTURE";
                case game::pet::kFollow: return "PET_FOLLOW_TEXTURE";
                case game::pet::kAttack: return "PET_ATTACK_TEXTURE";
                case game::pet::kAbandon: return "PET_DISMISS_TEXTURE";
                default: return nullptr;
            }
        case game::pet::ActionType::Reaction:
            switch (action) {
                case game::pet::kPassive: return "PET_PASSIVE_TEXTURE";
                case game::pet::kDefensive: return "PET_DEFENSIVE_TEXTURE";
                case game::pet::kAggressive: return "PET_AGGRESSIVE_TEXTURE";
                default: return nullptr;
            }
        default: return nullptr;
    }
}

static int lua_HasAction(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnFalse(L); }
    if (uint32_t packed; possessSlot(gh, static_cast<int>(luaL_checknumber(L, 1)), packed)) {
        lua_pushboolean(L, packed != 0);
        return 1;
    }
    int slot = static_cast<int>(luaL_checknumber(L, 1)) - 1; // WoW uses 1-indexed slots
    lua_pushboolean(L, game::shownAction(*gh, slot) != nullptr);
    return 1;
}

// GetActionTexture(slot) → texturePath or nil
static int lua_GetActionTexture(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }
    if (uint32_t packed; possessSlot(gh, static_cast<int>(luaL_checknumber(L, 1)), packed)) {
        if (const char* token = possessTokenTexture(packed)) {
            lua_getglobal(L, token);
            return 1;
        }
        const uint32_t spell = possessSpell(packed);
        const std::string icon = spell ? gh->getSpellIconPath(spell) : std::string{};
        if (icon.empty()) lua_pushnil(L); else lua_pushstring(L, icon.c_str());
        return 1;
    }
    int slot = static_cast<int>(luaL_checknumber(L, 1)) - 1;
    const auto* shown = game::shownAction(*gh, slot);
    if (!shown) {
        lua_pushnil(L);
        return 1;
    }
    const auto& action = *shown;
    if (action.type == game::ActionBarSlot::MACRO) {
        lua_pushstring(L, macroTexture(*gh, action.id).c_str());
        return 1;
    }
    if (action.type == game::ActionBarSlot::SPELL) {
        // The active icon while the spell's form is on: Stealth shows
        // Spell_Nature_Invisibilty while stealthed.
        std::string icon = game::actionSpellIcon(*gh, action.id);
        if (!icon.empty()) {
            lua_pushstring(L, icon.c_str());
            return 1;
        }
    } else if (action.type == game::ActionBarSlot::ITEM && action.id != 0) {
        gh->ensureItemInfo(action.id);
        const auto* info = gh->getItemInfo(action.id);
        if (info && info->displayInfoId != 0) {
            std::string icon = gh->getItemIconPath(info->displayInfoId);
            if (!icon.empty()) {
                lua_pushstring(L, icon.c_str());
                return 1;
            }
        }
    }
    lua_pushnil(L);
    return 1;
}

// IsCurrentAction(slot) → boolean
static int lua_IsCurrentAction(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int slot = static_cast<int>(luaL_optnumber(L, 1, 0)) - 1;
    bool current = false;
    const auto* shown = gh ? game::shownAction(*gh, slot) : nullptr;
    std::optional<game::ActionBarSlot> standIn;
    if (shown && shown->type == game::ActionBarSlot::MACRO) {
        standIn = macroStandIn(*gh, resolveMacroTarget(*gh, shown->id));
        shown = standIn ? &*standIn : nullptr;
    }
    if (shown && shown->type == game::ActionBarSlot::SPELL) {
        current = game::isCurrentSpellAction(shown->id, gh->getActiveActionSpellId(),
                                             gh->hasAutoAttackIntent()) ||
                  game::spellFormActive(*gh, shown->id) ||
                  (gh->isMounted() && shown->id == gh->getMountedByCastSpellId());
    }
    lua_pushboolean(L, current);
    return 1;
}

// IsUsableAction(slot) → usable, notEnoughMana
//
// Not the cooldown: a spell waiting on one is still usable, and the sweep over
// the icon shows the wait. See game::spellUsability for what is.
static int lua_IsUsableAction(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushboolean(L, 0); lua_pushboolean(L, 0); return 2; }
    if (uint32_t packed; possessSlot(gh, static_cast<int>(luaL_checknumber(L, 1)), packed)) {
        lua_pushboolean(L, packed != 0);
        lua_pushboolean(L, 0);
        return 2;
    }
    int slot = static_cast<int>(luaL_checknumber(L, 1)) - 1;
    const auto* shown = game::shownAction(*gh, slot);
    if (!shown) {
        lua_pushboolean(L, 0);
        lua_pushboolean(L, 0);
        return 2;
    }
    if (shown->type == game::ActionBarSlot::MACRO) {
        // Native 2.4.3: a macro is as usable as its spell or item; one
        // naming neither ("/say hi") is usable, one naming something the
        // player lacks ("/use 13" on an empty slot) is not.
        const MacroTarget target = resolveMacroTarget(*gh, shown->id);
        if (const auto standIn = macroStandIn(*gh, target)) {
            const auto usability = game::actionUsability(*gh, *standIn);
            lua_pushboolean(L, usability.usable ? 1 : 0);
            lua_pushboolean(L, usability.noPower ? 1 : 0);
            return 2;
        }
        lua_pushboolean(L, target.kind == MacroTarget::Kind::None ? 1 : 0);
        lua_pushboolean(L, 0);
        return 2;
    }
    const auto usability = game::actionUsability(*gh, *shown);
    lua_pushboolean(L, usability.usable ? 1 : 0);
    lua_pushboolean(L, usability.noPower ? 1 : 0);
    return 2;
}

// IsActionInRange(slot) → 1 if in range, 0 if out, nil if no range check applicable
static int lua_IsActionInRange(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }
    int slot = static_cast<int>(luaL_checknumber(L, 1)) - 1;
    const auto* shown = game::shownAction(*gh, slot);
    if (!shown) {
        lua_pushnil(L);
        return 1;
    }
    const auto& action = *shown;
    uint32_t spellId = 0;
    if (action.type == game::ActionBarSlot::SPELL) {
        spellId = action.id;
    } else if (const MacroTarget target = action.type == game::ActionBarSlot::MACRO
                   ? resolveMacroTarget(*gh, action.id) : MacroTarget{};
               target.kind == MacroTarget::Kind::Spell) {
        spellId = target.id;
    } else {
        // Items: no range check for now
        lua_pushnil(L);
        return 1;
    }
    return pushSpellRangeAnswer(L, gh, spellId, gh->getTargetGuid());
}

// GetActionInfo(slot) → actionType, id, subType
static int lua_GetActionInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return 0; }
    // Nothing for a possessed unit's action, its spells included: build 8606
    // controlling a Human Footman answers nil for 121-130 while
    // GetActionTexture names each icon.
    if (uint32_t packed; possessSlot(gh, static_cast<int>(luaL_checknumber(L, 1)), packed)) return 0;
    int slot = static_cast<int>(luaL_checknumber(L, 1)) - 1;
    const auto* shown = game::shownAction(*gh, slot);
    if (!shown) {
        return 0;
    }
    const auto& action = *shown;
    switch (action.type) {
        case game::ActionBarSlot::SPELL: {
            // Native 2.4.3: "spell", the spell's spellbook slot, "spell"
            // (Sinister Strike answered 39); 3.3.5 gives the spell id and,
            // fourth, the id again (vehiclemenubar links it).
            if (game::isActiveExpansion("tbc")) {
                uint32_t bookSlot = 0, offset = 0;
                for (const auto& tab : gh->getSpellBookTabs()) {
                    const auto it = std::find(tab.spellIds.begin(), tab.spellIds.end(), action.id);
                    if (it != tab.spellIds.end()) {
                        bookSlot = offset + static_cast<uint32_t>(it - tab.spellIds.begin()) + 1;
                        break;
                    }
                    offset += static_cast<uint32_t>(tab.spellIds.size());
                }
                lua_pushstring(L, "spell");
                lua_pushnumber(L, bookSlot);
                lua_pushstring(L, "spell");
                return 3;
            }
            lua_pushstring(L, "spell");
            lua_pushnumber(L, action.id);
            lua_pushstring(L, "spell");
            lua_pushnumber(L, action.id);
            return 4;
        }
        case game::ActionBarSlot::ITEM:
            // Native 2.4.3: "item", id (Rough Arrow: item|2512).
            lua_pushstring(L, "item");
            lua_pushnumber(L, action.id);
            if (game::isActiveExpansion("tbc")) return 2;
            lua_pushstring(L, "item");
            return 3;
        case game::ActionBarSlot::MACRO:
            // The macro's index, its place in the sorted list now: native
            // ZZa (renamed from APa) answered macro|35.
            lua_pushstring(L, "macro");
            lua_pushnumber(L, gh->macroIndexForId(action.id));
            if (game::isActiveExpansion("tbc")) return 2;
            lua_pushstring(L, "macro");
            return 3;
        default:
            return 0;
    }
}

// GetActionCount(slot) → count (item stack count or 0)
static int lua_GetActionCount(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnZero(L); }
    int slot = static_cast<int>(luaL_checknumber(L, 1)) - 1;
    const auto* shown = game::shownAction(*gh, slot);
    if (!shown) {
        lua_pushnumber(L, 0);
        return 1;
    }
    std::optional<game::ActionBarSlot> standIn;
    if (shown->type == game::ActionBarSlot::MACRO)
        standIn = macroStandIn(*gh, resolveMacroTarget(*gh, shown->id));
    const auto& action = standIn ? *standIn : *shown;
    if (action.type == game::ActionBarSlot::ITEM && action.id != 0) {
        lua_pushnumber(L, game::actionItemCount(*gh, action.id));
    } else if (action.type == game::ActionBarSlot::SPELL) {
        // A spell counts its reagent: native 2.4.3 shows Vanish with the
        // Flash Powder carried.
        lua_pushnumber(L, game::spellReagentCount(*gh, action.id));
    } else {
        lua_pushnumber(L, 0);
    }
    return 1;
}

// GetActionCooldown(slot) → start, duration, enable
static int lua_GetActionCooldown(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 1); return 3; }
    int slot = static_cast<int>(luaL_checknumber(L, 1)) - 1;
    const auto* shown = game::shownAction(*gh, slot);
    if (!shown) {
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 1);
        return 3;
    }
    std::optional<game::ActionBarSlot> standIn;
    if (shown->type == game::ActionBarSlot::MACRO) {
        // Native 2.4.3: a macro's spell or item cooldown; "/say hi" answers
        // enable 0, a macro naming something unresolved enable 1.
        const MacroTarget target = resolveMacroTarget(*gh, shown->id);
        standIn = macroStandIn(*gh, target);
        if (!standIn) {
            lua_pushnumber(L, 0);
            lua_pushnumber(L, 0);
            lua_pushnumber(L, target.kind == MacroTarget::Kind::None ? 0 : 1);
            return 3;
        }
    }
    const auto& action = standIn ? *standIn : *shown;
    // Cast, and its cooldown waits for the event: enable 0, no sweep.
    if (action.type == game::ActionBarSlot::SPELL && pushPendingCooldown(L, *gh, action.id)) return 3;
    if (action.type == game::ActionBarSlot::ITEM) {
        // An item with no use spell has no cooldown to show: native 2.4.3
        // answers enable 0 for Mining Pick, Worn Dagger and Rough Arrow.
        const auto* info = gh->getItemInfo(action.id);
        if (info && info->valid &&
            std::none_of(info->spells.begin(), info->spells.end(),
                         [](const auto& spell) { return spell.spellId != 0 && spell.spellTrigger == 0; })) {
            lua_pushnumber(L, 0);
            lua_pushnumber(L, 0);
            lua_pushnumber(L, 0);
            return 3;
        }
    }
    float cdRemaining = action.cooldownRemaining;
    float cdTotal = action.cooldownTotal;
    if (action.type == game::ActionBarSlot::SPELL) {
        const float spellRem = gh->getSpellCooldown(action.id);
        if (spellRem > cdRemaining) {
            cdRemaining = spellRem;
            cdTotal = gh->getSpellCooldownTotal(action.id);
        }
    } else if (action.type == game::ActionBarSlot::ITEM && cdRemaining <= 0.0f) {
        double itemStart = 0.0, itemDuration = 0.0;
        if (itemUseCooldown(gh, action.id, itemStart, itemDuration)) {
            lua_pushnumber(L, itemStart);
            lua_pushnumber(L, itemDuration);
            lua_pushnumber(L, 1);
            return 3;
        }
    }
    if (cdRemaining > 0.0f) {
        if (cdTotal < cdRemaining) cdTotal = cdRemaining;
        // WoW returns GetTime()-based start time; approximate
        double now = 0;
        lua_getglobal(L, "GetTime");
        if (lua_isfunction(L, -1)) {
            lua_call(L, 0, 1);
            now = lua_tonumber(L, -1);
            lua_pop(L, 1);
        } else {
            lua_pop(L, 1);
        }
        double start = now - (cdTotal - cdRemaining);
        lua_pushnumber(L, start);
        lua_pushnumber(L, cdTotal);
        lua_pushnumber(L, 1);
    } else if (action.type == game::ActionBarSlot::SPELL && gh->isGCDActive() &&
               game::onGlobalCooldown(*gh, action.id)) {
        // No individual cooldown but GCD is active - show GCD sweep
        float gcdRem = gh->getGCDRemaining();
        float gcdTotal = gh->getGCDTotal();
        double now = 0;
        lua_getglobal(L, "GetTime");
        if (lua_isfunction(L, -1)) { lua_call(L, 0, 1); now = lua_tonumber(L, -1); lua_pop(L, 1); }
        else lua_pop(L, 1);
        double elapsed = gcdTotal - gcdRem;
        lua_pushnumber(L, now - elapsed);
        lua_pushnumber(L, gcdTotal);
        lua_pushnumber(L, 1);
    } else {
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 1);
    }
    return 3;
}

// UseAction(slot, unit, button) - activate action bar slot (1-indexed)
//
// The second argument is a unit, and it was ignored. securetemplates is the
// only caller and it passes what the button's own attributes resolved to:
// nil for an ordinary click, and "player" when the self-cast modifier is
// held - which is the whole of how self-casting works. Dropping it meant
// every action went at the current target, so self-cast did nothing, and a
// unit-frame button carrying its own unit acted on whoever was targeted
// instead.
static int lua_PlaceAction(lua_State* L);
static int lua_UseAction(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    // Clicking a button while carrying something puts it there, as dropping
    // it would. SecureActionButton_OnClick answers every click with UseAction,
    // so this is the only place a click with a full cursor reaches. Original
    // 2.4.3: a spell dropped from the book onto an occupied button leaves the
    // old one on the cursor, and a click on an empty button places it and
    // empties the cursor; the button's own action is not used.
    if (s_cursorId != 0 && (s_cursorType == CursorType::SPELL ||
                            s_cursorType == CursorType::ITEM ||
                            s_cursorType == CursorType::MACRO)) {
        lua_settop(L, 1);
        return lua_PlaceAction(L);
    }
    if (uint32_t packed; possessSlot(gh, static_cast<int>(luaL_checknumber(L, 1)), packed)) {
        if (packed == 0) return 0;
        if (const uint32_t spell = possessSpell(packed)) {
            gh->getSpellName(spell);
            const auto it = gh->spellNameCacheRef().find(spell);
            if (it != gh->spellNameCacheRef().end() && (it->second.targetFlags & 0x40u)) {
                // A chess piece's Move: build 8606 puts a circle under the
                // cursor and the next world click casts it there.
                gh->beginPetGroundTarget(spell);
                return 0;
            }
        }
        uint64_t target = gh->hasTarget() ? gh->getTargetGuid() : 0;
        if (const char* unit = lua_tostring(L, 2); unit && *unit) {
            std::string uid(unit);
            toLowerInPlace(uid);
            if (const uint64_t named = resolveUnitGuid(gh, uid)) target = named;
        }
        gh->sendPetAction(packed, target);
        return 0;
    }
    int slot = static_cast<int>(luaL_checknumber(L, 1)) - 1;
    const auto* shown = game::shownAction(*gh, slot);
    if (!shown) return 0;
    const auto& action = *shown;
    // The end of the chain a click travels, and the last place it can stop
    // without saying so: an empty slot, a cooldown, or a type nothing acts on
    // all look the same from the button. At debug level, because pressing a
    // button on the action bar is the most ordinary thing a player does and
    // this was a warning on every one of them.
    LOG_DEBUG("UseAction: slot ", slot + 1, " type=", static_cast<int>(action.type),
              " id=", action.id, " ready=", action.isReady() ? 1 : 0);
    if (action.type == game::ActionBarSlot::SPELL && action.isReady()) {
        // A unit that resolves is named to the cast (the self-cast modifier's
        // "player", a unit frame's own unit). Without one the cast is given no
        // unit, and castSpell decides from the current target, the spell and
        // autoSelfCast whether it goes at the target, at the caster, or onto
        // the targeting cursor.
        uint64_t named = 0;
        if (const char* unit = lua_tostring(L, 2); unit && *unit) {
            std::string uid(unit);
            toLowerInPlace(uid);
            named = resolveUnitGuid(gh, uid);
        }
        // A spell that belongs to an item - a Hearthstone is spell 8690 - has
        // to go as CMSG_USE_ITEM rather than a cast, or the server drops it.
        // This client's own action bar did that and was the only place that
        // did; the bar is FrameXML's now and this is the path its buttons take.
        if (const uint32_t itemForSpell = gh->getItemIdForSpell(action.id)) {
            gh->useItemById(itemForSpell, named != 0 ? named : (gh->hasTarget() ? gh->getTargetGuid() : 0));
        } else {
            gh->castSpell(action.id, named);
        }
    } else if (action.type == game::ActionBarSlot::ITEM && action.id != 0) {
        uint64_t named = 0;
        if (const char* unit = lua_tostring(L, 2); unit && *unit) {
            std::string uid(unit);
            toLowerInPlace(uid);
            named = resolveUnitGuid(gh, uid);
        }
        gh->useItemById(action.id, named);
    } else if (action.type == game::ActionBarSlot::MACRO) {
        // A click on a macro button runs the macro, as its key binding does.
        if (auto* svc = getLuaServices(L); svc && svc->runMacroText) {
            const std::string body = gh->getMacroText(action.id);
            if (!body.empty()) svc->runMacroText(body);
        }
    }
    return 0;
}

// --- Cursor / Drag-Drop System ---
// Tracks what the player is "holding" on the cursor (spell, item, action).


/// Put whatever the cursor holds back where it came from: ClearCursor, a
/// right-click anywhere, and a pickup that displaces what was held.
///
/// An item goes back unlocked, and 2.4.3 says so with ITEM_LOCK_CHANGED for the
/// slot it came from, ahead of ACTIONBAR_HIDEGRID and CURSOR_UPDATE. Without it
/// the bag button stayed greyed after a cancelled drag - an item the auction
/// sell slot had refused, for one. An action (a spell, a macro, an item
/// reference off a bar) has nowhere to go back to and is simply dropped.
static void returnCursor(lua_State* L) {
    const bool carriedItem = s_cursorType == CursorType::ITEM &&
                             game::slots::cursorSourceIsInventory(s_cursorBag);
    const auto held = cursorItemSlot();
    if (carriedItem) {
        // The slot's lock is read from this record, so it goes first: a
        // redraw for the event below must already see the item as back.
        cursorItemSlot() = {};
        if (auto* gh = getGameHandler(L)) {
            if (held.equipped) {
                gh->fireAddonEvent("ITEM_LOCK_CHANGED", {std::to_string(held.slot)});
            } else {
                gh->fireAddonEvent("ITEM_LOCK_CHANGED",
                                   {std::to_string(held.bag), std::to_string(held.slot)});
            }
        }
    }
    clearCursorItem(L);
}

static int lua_ClearCursor(lua_State* L) {
    // 2.4.3 announces CURSOR_UPDATE for a ClearCursor with nothing held too.
    if (s_cursorType == CursorType::NONE) {
        if (auto* gh = getGameHandler(L)) gh->fireAddonEvent("CURSOR_UPDATE", {});
        return 0;
    }
    returnCursor(L);
    return 0;
}

bool returnHeldCursor(lua_State* L) {
    if (s_cursorType == CursorType::NONE) return false;
    returnCursor(L);
    return true;
}

static int lua_GetCursorInfo(lua_State* L) {
    switch (s_cursorType) {
        case CursorType::SPELL:
            lua_pushstring(L, "spell");
            lua_pushnumber(L, 0);          // bookSlotIndex
            lua_pushstring(L, "spell");    // bookType
            lua_pushnumber(L, s_cursorId); // spellId
            return 4;
        case CursorType::ITEM:
        // An item out of the guild bank is an item. Where it came from is this
        // file's business - PickupContainerItem reads the cursor type to know a
        // drop is a withdrawal - and saying so here would only give the
        // interface a kind it has no branch for.
        case CursorType::GUILDBANK:
            lua_pushstring(L, "item");
            lua_pushnumber(L, s_cursorId);
            // The item's link third, as 2.4.3 answers: "item", 37,
            // "|cffffffff|Hitem:37:0:0:0:0:0:0:0|h[Worn Axe]|h|r".
            if (auto* gh = getGameHandler(L)) {
                if (const auto* info = gh->getItemInfo(s_cursorId)) {
                    lua_pushstring(L, game::itemChatLink(s_cursorId, info->quality < 8 ? info->quality : 1u, info->name).c_str());
                    return 3;
                }
            }
            return 2;
        case CursorType::MACRO:
            lua_pushstring(L, "macro");
            lua_pushnumber(L, getGameHandler(L) ? getGameHandler(L)->macroIndexForId(s_cursorId) : 0);
            return 2;
        case CursorType::MONEY:
            // containerframe.lua branches on this exact string to decide that
            // a click in a bag is money being put back.
            lua_pushstring(L, "money");
            lua_pushnumber(L, static_cast<lua_Number>(s_cursorMoney));
            return 2;
        case CursorType::MERCHANT:
            // containerframe.lua branches on this exact string to decide
            // whether a click in a bag is a purchase.
            lua_pushstring(L, "merchant");
            lua_pushnumber(L, s_cursorId);   // the vendor's list index
            return 2;
        case CursorType::PET_ACTION:
            lua_pushstring(L, "petaction");
            lua_pushnumber(L, s_cursorSlot);
            return 2;
        default:
            return 0;
    }
}

static int lua_CursorHasItem(lua_State* L) {
    // A guild bank item counts. The bank frame's own OnClick tests this before
    // deciding a click is a drop, so answering no for one would have left the
    // item stuck on the cursor with nowhere that would take it.
    //
    // An item reference lifted off an action bar does not: 2.4.3 answers
    // GetCursorInfo "item" for it and CursorHasItem nil, because the cursor
    // holds an action, not the item.
    lua_pushboolean(L, ((s_cursorType == CursorType::ITEM &&
                         game::slots::cursorSourceIsInventory(s_cursorBag)) ||
                        s_cursorType == CursorType::GUILDBANK) ? 1 : 0);
    return 1;
}

static int lua_CursorHasMacro(lua_State* L) {
    lua_pushboolean(L, s_cursorType == CursorType::MACRO ? 1 : 0);
    return 1;
}

static int lua_CursorHasMoney(lua_State* L) {
    lua_pushboolean(L, s_cursorType == CursorType::MONEY ? 1 : 0);
    return 1;
}

static int lua_CursorHasSpell(lua_State* L) {
    lua_pushboolean(L, s_cursorType == CursorType::SPELL ? 1 : 0);
    return 1;
}

/// What an action bar slot holds, as the cursor would hold it.
static CursorType cursorTypeForAction(game::ActionBarSlot::Type type) {
    switch (type) {
        case game::ActionBarSlot::SPELL: return CursorType::SPELL;
        case game::ActionBarSlot::ITEM:  return CursorType::ITEM;
        case game::ActionBarSlot::MACRO: return CursorType::MACRO;
        default: break;
    }
    assert(false && "an empty action slot has no cursor type");
    return CursorType::NONE;
}

/// Put an action on the cursor: a spell, a macro, or an item reference off a
/// bar. The fields are written before setCursorType announces the change, so
/// CURSOR_UPDATE and ACTIONBAR_SHOWGRID handlers read the new cursor.
///
/// sourceSlot is the action slot it came off, or zero for the spellbook, the
/// macro frame or a companion. An item off a bar is a reference, not the item:
/// kCursorNoSource says it has no inventory slot behind it, so it is neither
/// moved nor destroyed when dropped (see cursorSourceIsInventory).
static void holdAction(lua_State* L, CursorType type, uint32_t id, int sourceSlot) {
    assert(type == CursorType::SPELL || type == CursorType::ITEM || type == CursorType::MACRO);
    assert(s_cursorType == CursorType::NONE);
    s_cursorId = id;
    s_cursorSlot = sourceSlot;
    s_cursorBag = game::slots::kCursorNoSource;
    cursorItemSlot() = {};
    setCursorType(L, type);
}

/// Pick up from the spellbook, the macro frame or the companion list.
///
/// Whatever was held goes back first, as a ClearCursor would put it - an item
/// to its bag with ITEM_LOCK_CHANGED - and picking up the very thing already
/// held puts it down instead: 2.4.3 empties the cursor for PickupSpell of the
/// spell it is carrying.
static void pickUpAction(lua_State* L, CursorType type, uint32_t id) {
    const bool same = s_cursorType == type && s_cursorId == id;
    returnHeldCursor(L);
    if (same) return;
    holdAction(L, type, id, 0);
}

/// PickupAction(slot) - take what is in a slot onto the cursor, and put down
/// what the cursor was already holding. PlaceAction, and a click on an action
/// button while carrying something (UseAction), are the same swap.
///
/// Measured on build 8606:
///  - empty cursor, occupied slot: the action comes off (CURSOR_UPDATE,
///    ACTIONBAR_SHOWGRID, ACTIONBAR_SLOT_CHANGED).
///  - carrying, empty slot: it goes on; an item out of a bag goes back to its
///    bag (ITEM_LOCK_CHANGED), then ACTIONBAR_HIDEGRID, CURSOR_UPDATE,
///    ACTIONBAR_SLOT_CHANGED.
///  - carrying, occupied slot: the same put-down, then the old action comes
///    onto the cursor (ACTIONBAR_SHOWGRID, plus CURSOR_UPDATE for an item),
///    then ACTIONBAR_SLOT_CHANGED. The old action is held exactly as one lifted
///    off the bar: an item reference answers CursorHasItem nil.
///  - carrying the action the slot already holds: the cursor empties
///    (ACTIONBAR_HIDEGRID, CURSOR_UPDATE) and the slot is left alone.
///  - money, or an item that is neither equippable nor usable (Blinding
///    Powder, Okra, the Guild Charter): nothing happens and nothing is said.
static int lua_PickupAction(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    int slot = static_cast<int>(luaL_checknumber(L, 1));
    const auto& bar = gh->getActionBar();
    if (slot < 1 || slot > static_cast<int>(bar.size())) return 0;

    // Read before anything is written: setActionBarSlot rewrites the entry
    // this would otherwise still be pointing at.
    const auto existing = bar[slot - 1];
    const bool hadAction = !existing.isEmpty();
    const CursorType held = s_cursorType;
    const uint32_t heldId = s_cursorId;
    const bool holding = heldId != 0 &&
                         (held == CursorType::SPELL || held == CursorType::ITEM ||
                          held == CursorType::MACRO);
    if (!holding && held != CursorType::NONE) return 0;   // money, a vendor item...

    // Native 2.4.3 takes an item only if it can be equipped or has a use
    // spell: Mining Pick, Rough Arrow and Linen Bandage go on, Blinding
    // Powder, Flash Powder and the Guild Charter (no InventoryType, no
    // on-use spell) do not.
    if (holding && held == CursorType::ITEM) {
        const auto* info = gh->getItemInfo(heldId);
        if (!info || !info->valid) {
            LOG_WARNING("PlaceAction: item ", heldId, " has no query response; not placed");
            return 0;
        }
        const bool useSpell = std::any_of(info->spells.begin(), info->spells.end(), [](const auto& spell) {
            constexpr uint32_t kTriggerOnUse = 0;
            return spell.spellId != 0 && spell.spellTrigger == kTriggerOnUse;
        });
        if (info->inventoryType == 0 && !useSpell) return 0;
    }
    if (!holding && !hadAction) return 0;

    const auto heldAsAction = held == CursorType::SPELL ? game::ActionBarSlot::SPELL
                            : held == CursorType::ITEM  ? game::ActionBarSlot::ITEM
                                                        : game::ActionBarSlot::MACRO;
    if (holding && hadAction && existing.type == heldAsAction && existing.id == heldId) {
        returnHeldCursor(L);
        return 0;
    }

    if (holding) returnHeldCursor(L);
    if (hadAction) holdAction(L, cursorTypeForAction(existing.type), existing.id, slot);
    // Last, as 2.4.3 announces it: setActionBarSlot raises
    // ACTIONBAR_SLOT_CHANGED, and its handlers see the cursor already settled.
    gh->setActionBarSlot(slot - 1, holding ? heldAsAction : game::ActionBarSlot::EMPTY,
                         holding ? heldId : 0);
    return 0;
}

// PlaceAction(slot) - places cursor content into an action bar slot
//
// A swap, as the real one is: whatever the slot held goes onto the cursor, so
// a bar can be rearranged by dropping one action on another and then placing
// the one that came off. PickupAction while holding something is exactly that
// swap, so this is it. With nothing held it does nothing.
static int lua_PlaceAction(lua_State* L) {
    if (s_cursorType == CursorType::NONE) return 0;
    return lua_PickupAction(L);
}

// PickupSpell(bookSlot, bookType) - picks up a spell from the spellbook
static int lua_PickupSpell(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    // Through the shared resolver, so the book is read as well as the slot:
    // dragging a pet spell out of the book picked up whichever of the player's
    // spells sat at that index.
    const uint32_t picked = spellIdForCall(L, gh);
    if (picked == 0) return 0;
    pickUpAction(L, CursorType::SPELL, picked);
    return 0;
}

// PlaceGlyphInSocket(socket) - put the glyph on the cursor into that socket.
//
// Registered here rather than with the other glyph calls because the glyph is
// on the cursor and the cursor lives in this file. There is no socketing opcode
// in 3.3.5: the glyph item is used with the socket written into the glyphIndex
// field CMSG_USE_ITEM already carries, and the server applies the item's spell
// to it. The comment this replaces said socketing "needs a packet this client
// does not send" - the packet was there, one field short of saying where.
static int lua_PlaceGlyphInSocket(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int socket = static_cast<int>(luaL_optnumber(L, 1, 0));
    if (!gh || socket < 1 || socket > game::GameHandler::MAX_GLYPH_SLOTS) return 0;
    uint8_t bag = 0, slot = 0;
    if (!cursorWireSlot(bag, slot)) return 0;   // nothing held: nothing to place
    gh->placeGlyphFromBag(bag, slot, static_cast<uint32_t>(socket - 1));
    setCursorType(L, CursorType::NONE);
    return 0;
}

// PickupGuildBankItem(tab, slot) - both halves of a guild bank drag.
//
// The frame calls this from OnDragStart and from OnReceiveDrag, so one function
// picks up and puts down, the way PickupContainerItem does for bags. It was a
// no-op, so nothing could be moved into or out of the guild bank at all -
// while the packet builder for the move sat verified against the server, and
// AutoStoreGuildBankItem beside it has been using half of it for right-clicks.
//
// The tab and slot are FrameXML's, counted from one; the wire counts from zero,
// which is the shift AutoStoreGuildBankItem already applies.
static int lua_PickupGuildBankItem(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int tab = static_cast<int>(luaL_optnumber(L, 1, 0));
    const int slot = static_cast<int>(luaL_optnumber(L, 2, 0));
    if (!gh || tab < 1 || slot < 1) return 0;

    if (droppedItemFromNowhere(L)) return 0;

    // Carrying a bag item, so this is the deposit.
    uint8_t srcBag = 0, srcSlot = 0;
    if (cursorWireSlot(srcBag, srcSlot)) {
        gh->guildBankDepositItem(static_cast<uint8_t>(tab - 1),
                                 static_cast<uint8_t>(slot - 1),
                                 srcBag, srcSlot, s_cursorSplit);
        setCursorType(L, CursorType::NONE);
        return 0;
    }
    // Carrying a bank item already: move/swap/split within the guild bank
    // (same tab or cross-tab).
    if (s_cursorType == CursorType::GUILDBANK) {
        if (s_cursorBag != tab || s_cursorSlot != slot) {
            gh->guildBankSwapItems(static_cast<uint8_t>(s_cursorBag - 1),
                                   static_cast<uint8_t>(s_cursorSlot - 1),
                                   static_cast<uint8_t>(tab - 1),
                                   static_cast<uint8_t>(slot - 1),
                                   s_cursorId, s_cursorSplit);
        }
        setCursorType(L, CursorType::NONE);
        return 0;
    }

    // Otherwise pick the slot up, if there is anything in it.
    const auto& bank = gh->getGuildBankData();
    const auto& items = (tab <= static_cast<int>(bank.tabs.size()))
                        ? bank.tabs[static_cast<size_t>(tab - 1)].items
                        : bank.tabItems;
    for (const auto& it : items) {
        if (it.slotId != slot - 1 || it.itemEntry == 0) continue;
        setCursorType(L, CursorType::GUILDBANK);
        s_cursorId = it.itemEntry;
        s_cursorBag = tab;
        s_cursorSlot = slot;
        s_cursorSplit = 0;   // the whole stack, unless SplitGuildBankItem says otherwise
        return 0;
    }
    return 0;
}

// SplitGuildBankItem(tab, slot, amount) - take part of a stack.
//
// The stack split dialog calls this instead of PickupGuildBankItem when the
// player asks for fewer than all of them. It is the same pickup with a number
// attached: the wire carries the split on the *withdrawal* rather than on the
// pickup, so the amount rides on the cursor until the drop says where it goes.
static int lua_SplitGuildBankItem(lua_State* L) {
    const int amount = static_cast<int>(luaL_optnumber(L, 3, 0));
    lua_settop(L, 2);
    lua_PickupGuildBankItem(L);
    if (s_cursorType == CursorType::GUILDBANK && amount > 0)
        s_cursorSplit = static_cast<uint32_t>(amount);
    return 0;
}

// PickupCompanion(mode, index) - drag a mount or a critter to the bar.
//
// Registered here rather than beside the other companion calls because the
// cursor lives in this file, and because that is what this is: a pickup.
//
// A companion is summoned by a spell, and this client's action bar holds
// spells, so the cursor carries the summon rather than a companion of its own
// kind. What lands on the bar is the same icon casting the same thing; the
// difference is invisible to everyone but the tooltip, which names the spell.
// Modelling a fourth action type to say "companion" would change nothing a
// player can see.
static int lua_PickupCompanion(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const char* mode = luaL_optstring(L, 1, "");
    const int index = static_cast<int>(luaL_optnumber(L, 2, 0));
    if (index < 1) return 0;
    const auto& list = gh->getCompanions(std::string(mode) == "MOUNT");
    if (index > static_cast<int>(list.size())) return 0;
    const uint32_t spellId = list[static_cast<size_t>(index) - 1].spellId;
    if (spellId == 0) return 0;
    pickUpAction(L, CursorType::SPELL, spellId);
    return 0;
}

// PickupSpellBookItem(bookSlot, bookType) - alias for PickupSpell
static int lua_PickupSpellBookItem(lua_State* L) {
    return lua_PickupSpell(L);
}

// PickupContainerItem(bag, slot) - picks up an item from a bag

/// Where the item on the cursor lives, in the numbering the server uses.
///
/// The server addresses one flat space: equipment is slots 0 to 22, the
/// backpack follows at 23, and a bag's contents are addressed by that bag's own
/// equipment slot together with an index inside it. FrameXML counts bags 0 to 4
/// with 1-based slots instead, so the two have to be translated - the same
/// translation this client's own bag window does before sending a swap.
static bool cursorWireSlot(uint8_t& bag, uint8_t& slot) {
    if (s_cursorType != CursorType::ITEM) return false;
    // Nowhere to move it from. This used to fall into the equipped branch
    // below, because everything negative was the paperdoll - see
    // kCursorNoSource for what that cost.
    if (!game::slots::cursorSourceIsInventory(s_cursorBag)) return false;

    // Minus one is two different places. The paperdoll picks an item up with
    // it, and the interface numbers the bank's own slots BANK_CONTAINER, which
    // is also minus one - so an item lifted out of the bank read as a worn one,
    // and the move that followed named an equipment slot: bank slot five went
    // out as equipment slot four. The flag the cursor already carries is the
    // only thing that tells the two apart, and it was not being read.
    if (cursorItemSlot().equipped) {
        bag = 0xFF;
        slot = static_cast<uint8_t>(s_cursorSlot - 1);
        return true;
    }
    // Everything else through the shared pair, which knows the backpack, the
    // keyring, the bank, its bags and the worn bags. Written out here it knew
    // three of them.
    bag = containerWireBag(s_cursorBag);
    slot = containerWireSlot(s_cursorBag, s_cursorSlot);
    return true;
}

/// Put the cursor down. Everything is cleared before the events go out, so a
/// CURSOR_UPDATE handler asking GetCursorInfo hears the empty cursor.
static void clearCursorItem(lua_State* L) {
    s_cursorMoney = 0;
    s_cursorSplit = 0;
    s_cursorId = 0;
    s_cursorSlot = 0;
    s_cursorBag = -1;
    cursorItemSlot() = {};
    setCursorType(L, CursorType::NONE);
}

/// The picture on the pointer: the icon of whatever the cursor holds, or empty.
///
/// Worked out from the cursor itself every time it is drawn, never stored
/// beside it. A stored copy was set by each pickup separately and cleared by
/// each put-down separately, and every place that changed one without the other
/// left a pointer showing something other than what was held - a picture with
/// nothing held behind it, or something held and nothing shown (money).
static std::string cursorPicture(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh || s_cursorType == CursorType::NONE) return {};
    const auto itemIcon = [gh](uint32_t itemId) -> std::string {
        if (const auto* info = gh->getItemInfo(itemId); info && info->displayInfoId)
            return gh->getItemIconPath(info->displayInfoId);
        gh->ensureItemInfo(itemId);
        return {};
    };
    switch (s_cursorType) {
        case CursorType::SPELL:
            return gh->getSpellIconPath(s_cursorId);
        case CursorType::ITEM:
        case CursorType::GUILDBANK:
            return itemIcon(s_cursorId);
        case CursorType::MACRO: {
            const std::string& icon = gh->getMacroIcon(s_cursorId);
            return icon.empty() ? std::string("Interface\\Icons\\INV_Misc_QuestionMark") : icon;
        }
        case CursorType::MONEY:
            return coinIconForAmount(static_cast<double>(s_cursorMoney));
        case CursorType::MERCHANT: {
            const auto& items = gh->getVendorItems().items;
            const int index = static_cast<int>(s_cursorId);
            if (index < 1 || index > static_cast<int>(items.size())) return {};
            return gh->getItemIconPath(items[static_cast<size_t>(index) - 1].displayInfoId);
        }
        case CursorType::PET_ACTION: {
            if (const char* globalName = possessTokenTexture(s_cursorPetPacked)) {
                lua_getglobal(L, globalName);
                std::string path = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
                lua_pop(L, 1);
                return path;
            }
            const uint32_t spellId = game::pet::petActionId(s_cursorPetPacked);
            return spellId ? gh->getSpellIconPath(spellId) : std::string{};
        }
        case CursorType::STABLE_PET: {
            const uint32_t family = gh->getCreatureFamily(s_cursorId);
            const auto* layouts = pipeline::getActiveDBCLayout();
            const auto* famLayout = layouts ? layouts->getLayout("CreatureFamily") : nullptr;
            auto* assets = gh->services().assetManager;
            if (family != 0 && famLayout && assets) {
                const uint32_t iconCol = famLayout->field("IconFile");
                const auto families = assets->loadDBC("CreatureFamily.dbc");
                const int32_t row = (iconCol != 0xFFFFFFFFu && families && families->isLoaded())
                    ? families->findRecordById(family) : -1;
                if (row >= 0) {
                    const std::string path = families->getString(static_cast<uint32_t>(row), iconCol);
                    if (!path.empty()) return path;
                }
            }
            return "Interface\\Icons\\Ability_Hunter_BeastTaming";
        }
        case CursorType::NONE:
            break;
    }
    return {};
}

static bool droppedItemFromNowhere(lua_State* L) {
    if (s_cursorType != CursorType::ITEM) return false;
    if (game::slots::cursorSourceIsInventory(s_cursorBag)) return false;
    clearCursorItem(L);
    return true;
}

void pickupMerchantItem(lua_State* L, int index) {
    // Index zero is not a pickup at all: it means "put what the cursor is
    // holding into the merchant's sell slot", which is how 3.3.5 sells from a
    // bag. ContainerFrameItemButton_OnClick picks the item up and calls this
    // with no argument, and luaL_optnumber turns that into the zero below.
    //
    // This returned here, so a right-click at a vendor lifted the item onto
    // the cursor, asked to sell it, was answered with nothing, and left it
    // stuck to the pointer. ShowContainerSellCursor beside this is the other
    // half of the same path and is a no-op for a reason - it only changes the
    // cursor art - which is what made this one look deliberate too.
    if (index < 1) {
        soldHeldItem(L);
        return;
    }
    setCursorType(L, CursorType::MERCHANT);
    s_cursorId = static_cast<uint32_t>(index);
    s_cursorSlot = 0;
    s_cursorBag = -1;
}

/// Sell whatever the cursor is holding, and put it down either way.
///
/// Bags and the backpack only. An item on the paperdoll can be dragged to a
/// merchant in the real client, but the two sell verbs here are the ones that
/// keep the buyback list, and neither knows an equipment slot - inventing a
/// third path for a case a right-click in a bag cannot reach would be guessing
/// at the bookkeeping rather than restoring behaviour.
static void soldHeldItem(lua_State* L) {
    // A merchant item on the cursor was never taken out of anything, so there
    // is nothing to sell and nothing to put back.
    if (s_cursorType == CursorType::MERCHANT) { clearCursorItem(L); return; }
    if (s_cursorType != CursorType::ITEM) return;
    auto* gh = getGameHandler(L);
    if (!gh) return;
    if (!game::slots::cursorSourceIsInventory(s_cursorBag)) return;

    if (s_cursorBag == 0) {
        gh->sellItemBySlot(s_cursorSlot - 1);
    } else if (s_cursorBag > 0) {
        gh->sellItemInBag(s_cursorBag - 1, s_cursorSlot - 1);
    } else {
        return;   // worn, and held rather than dropped on the floor
    }
    clearCursorItem(L);
}

bool boughtHeldMerchantItem(lua_State* L) {
    if (s_cursorType != CursorType::MERCHANT) return false;
    auto* gh = getGameHandler(L);
    const int index = static_cast<int>(s_cursorId);
    if (gh && index >= 1) {
        const auto& items = gh->getVendorItems().items;
        if (index <= static_cast<int>(items.size())) {
            const auto& vi = items[index - 1];
            gh->buyItem(gh->getVendorGuid(), vi.itemId, vi.slot, 1);
        }
    }
    clearCursorItem(L);
    return true;
}


/// Put the item in a bag slot on the cursor, with everything that goes with it:
/// the icon, the greyed slot, and the record of where it came from.
///
/// Split out of lua_PickupContainerItem so PickupItem can reach it. Writing the
/// cursor twice is how one of them would come to set the icon and the other not.
///
/// `announceLock` is false where the item was locked already and stays so -
/// the auction sell slot handing its item back to the cursor - because 2.4.3
/// says nothing about a lock that did not change.
static void playHeldOrSlotItemSound(game::GameHandler* gh, uint32_t itemId, bool pickup) {
    if (!gh || itemId == 0) return;
    auto* ac = gh->services().audioCoordinator;
    auto* sfx = ac ? ac->getUiSoundManager() : nullptr;
    if (!sfx) return;
    uint32_t displayInfoId = 0;
    if (const auto* info = gh->getItemInfo(itemId); info && info->displayInfoId != 0) {
        displayInfoId = info->displayInfoId;
    } else {
        gh->ensureItemInfo(itemId);
    }
    if (displayInfoId == 0 || !sfx->playItemDisplaySound(displayInfoId, pickup)) {
        sfx->playPickupBag();
    }
}

static void pickupFromContainerSlot(lua_State* L, game::GameHandler* gh,
                                    int bag, int slot, bool announceLock = true) {
    const auto& inv = gh->getInventory();
    // Every container this numbering reaches, not the backpack and the four
    // worn bags alone: the bank's own slots are container -1 and its bags are
    // 5 through 11, and bankframe.lua picks an item up with
    // PickupContainerItem(BANK_CONTAINER, self:GetID()). Missing them, the bank
    // answered "nothing there to pick up" for every square that had something
    // in it.
    const game::ItemSlot* itemSlot = containerItemSlot(inv, bag, slot);
    if (itemSlot && !itemSlot->empty()) {
        setCursorType(L, CursorType::ITEM);
        s_cursorId = itemSlot->item.itemId;
        s_cursorBag = bag;
        s_cursorSlot = slot;
        cursorItemSlot() = {bag, slot, false};
        playHeldOrSlotItemSound(gh, itemSlot->item.itemId, /*pickup=*/true);
        // The slot draws greyed while its item is on the cursor, which is how a
        // bag shows that something has been picked up out of it.
        if (announceLock) {
            gh->fireAddonEvent("ITEM_LOCK_CHANGED",
                               {std::to_string(bag), std::to_string(slot)});
        }
        LOG_WARNING("FrameXML pickup: bag ", bag, " slot ", slot,
                    " item ", itemSlot->item.itemId);
    } else {
        LOG_WARNING("FrameXML pickup: bag ", bag, " slot ", slot,
                    " - nothing there to pick up");
    }
}

void pickupSplitFromContainer(lua_State* L, game::GameHandler* gh,
                              int bag, int slot, int count) {
    pickupFromContainerSlot(L, gh, bag, slot);
    // Only if the pickup took: an empty slot leaves the cursor alone, and an
    // amount left on it would ride along with whatever is picked up next.
    if (s_cursorType == CursorType::ITEM && count > 0) {
        s_cursorSplit = static_cast<uint32_t>(count);
    }
}


/// PickupItem(id | "name" | link) - put an item on the cursor by naming it
/// rather than by pointing at a slot.
///
/// A macro's own way to pick something up, and the 'item' branch of
/// securehandlers.lua's cursor dispatcher - every other branch there was bound
/// and this one raised.
///
/// The first matching slot wins. WoW picks the first too, and a stack split
/// across two bags is one item as far as this is concerned.
static int lua_ReturnNothing2(lua_State* L) { (void)L; return 0; }

static int lua_PickupItem(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;

    uint32_t wantId = 0;
    std::string wantName;
    if (lua_isnumber(L, 1)) {
        wantId = static_cast<uint32_t>(lua_tonumber(L, 1));
    } else if (const char* text = lua_tostring(L, 1)) {
        // A link carries the id in it - "|Hitem:6948:0:..." - and is what a
        // shift-click puts in a macro, so it is the common case rather than
        // the exotic one.
        const std::string str(text);
        const size_t at = str.find("Hitem:");
        if (at != std::string::npos) {
            wantId = static_cast<uint32_t>(std::strtoul(str.c_str() + at + 6, nullptr, 10));
        } else {
            wantName = str;
        }
    }
    if (wantId == 0 && wantName.empty()) return 0;

    auto matches = [&](const game::ItemSlot& s) {
        if (s.empty()) return false;
        if (wantId != 0) return s.item.itemId == wantId;
        return s.item.name == wantName;
    };

    const auto& inv = gh->getInventory();
    for (int i = 0; i < inv.getBackpackSize(); ++i) {
        if (matches(inv.getBackpackSlot(i))) {
            pickupFromContainerSlot(L, gh, 0, i + 1);
            return 0;
        }
    }
    for (int b = 0; b < game::Inventory::NUM_BAG_SLOTS; ++b) {
        for (int sl = 0; sl < inv.getBagSize(b); ++sl) {
            if (matches(inv.getBagSlot(b, sl))) {
                pickupFromContainerSlot(L, gh, b + 1, sl + 1);
                return 0;
            }
        }
    }
    return 0;
}

static int lua_PickupContainerItem(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    int bag = static_cast<int>(luaL_checknumber(L, 1));
    int slot = static_cast<int>(luaL_checknumber(L, 2));

    // Same as the paperdoll: a click while something is waiting for an item to
    // be applied to is that choice, not a pickup.
    if (completedItemTarget(L, containerSlotGuid(gh, bag, slot))) return 0;

    // With the repair cursor up, a click on an item is the repair. The real
    // client consumes the click in C for the same reason, which is why
    // containerframe.lua has no branch of its own for it.
    if (repairedHeldItem(gh, containerSlotGuid(gh, bag, slot))) return 0;

    // A bag click while the cursor holds a vendor's item is the purchase.
    // containerframe.lua routes it here after checking GetCursorInfo.
    if (boughtHeldMerchantItem(L)) return 0;

    // Carrying something out of the guild bank, so this is the withdrawal.
    // The destination is this bag slot rather than the server's choice, which
    // is what dropping on a particular square means.
    if (s_cursorType == CursorType::GUILDBANK) {
        const uint8_t dstBag = containerWireBag(bag);
        const uint8_t dstSlot = containerWireSlot(bag, slot);
        gh->guildBankWithdrawItem(static_cast<uint8_t>(s_cursorBag - 1),
                                  static_cast<uint8_t>(s_cursorSlot - 1),
                                  dstBag, dstSlot, s_cursorSplit);
        setCursorType(L, CursorType::NONE);
        return 0;
    }

    // Carrying something the bags cannot take: the drag ends here and nothing
    // moves. Before cursorWireSlot, which would otherwise answer no and let
    // this fall through to the pickup below - putting the bag's own item on a
    // cursor that is already holding one.
    if (droppedItemFromNowhere(L)) return 0;

    // Already carrying something, so this is the drop rather than the pickup.
    // One function does both halves of a drag in WoW, and without this half a
    // dragged item was picked up and never put down anywhere.
    uint8_t srcBag = 0, srcSlot = 0;
    if (cursorWireSlot(srcBag, srcSlot)) {
        // Through the shared pair rather than written out here. This knew the
        // backpack and treated everything else as a worn bag, so a drop into
        // the bank - container -1 - asked wornBagContainer for container 17 and
        // sent the move to a container that is not one. Nothing in the bank
        // window could be dragged anywhere, including a bag sitting in it.
        const uint8_t dstBag = containerWireBag(bag);
        const uint8_t dstSlot = containerWireSlot(bag, slot);
        // At warning level because it is the outcome of a drag and happens
        // once per drop, and because the log carries nothing below warning -
        // which is why "did the move go out" could not be answered at all.
        LOG_WARNING("FrameXML drop: bag ", s_cursorBag, " slot ", s_cursorSlot,
                    " (wire ", (int)srcBag, "/", (int)srcSlot, ") -> bag ", bag,
                    " slot ", slot, " (wire ", (int)dstBag, "/", (int)dstSlot, ")");
        // Back where it came from: put it down rather than asking the server to
        // swap a slot with itself.
        //
        // The equipped flag counts here too, for the same reason it counts
        // above: a worn item on the cursor carries bag minus one, and so does
        // the bank, so dropping the helm on bank slot one read as putting it
        // back where it came from and quietly did nothing.
        if (s_cursorBag == bag && s_cursorSlot == slot && !cursorItemSlot().equipped) {
            const int sameBag = bag, sameSlot = slot;
            const uint32_t droppedId = s_cursorId;
            clearCursorItem(L);
            playHeldOrSlotItemSound(gh, droppedId, /*pickup=*/false);
            gh->fireAddonEvent("ITEM_LOCK_CHANGED",
                               {std::to_string(sameBag), std::to_string(sameSlot)});
            return 0;
        }

        const int wasBag = s_cursorBag, wasSlot = s_cursorSlot;
        const uint32_t droppedId = s_cursorId;
        // Part of a stack moves as a split rather than a swap: a swap would
        // send the whole thing, which is not what was picked up.
        if (s_cursorSplit > 0) {
            gh->splitItemTo(srcBag, srcSlot, dstBag, dstSlot,
                            static_cast<uint8_t>(s_cursorSplit));
        } else {
            gh->swapContainerItems(srcBag, srcSlot, dstBag, dstSlot);
        }
        clearCursorItem(L);
        playHeldOrSlotItemSound(gh, droppedId, /*pickup=*/false);
        gh->fireAddonEvent("ITEM_LOCK_CHANGED",
                           {std::to_string(wasBag), std::to_string(wasSlot)});
        gh->fireAddonEvent("ITEM_LOCK_CHANGED",
                           {std::to_string(bag), std::to_string(slot)});
        return 0;
    }

    // The item in the auction house's sell slot is locked in its bag, and
    // 2.4.3 does nothing at all when it is clicked there: no pickup, no event.
    if (auctionSellItem() == game::BagSlotRef{bag, slot}) {
        LOG_WARNING("FrameXML pickup: bag ", bag, " slot ", slot,
                    " - locked in the auction sell slot");
        return 0;
    }

    pickupFromContainerSlot(L, gh, bag, slot);
    return 0;
}

/// ClickAuctionSellItemButton() - the Auctions tab's sell slot: its OnClick,
/// OnDragStart and OnReceiveDrag all call it.
///
/// Carrying a bag item offers it: the client refuses soulbound, quest,
/// conjured and lootable items with their own error and changes nothing else,
/// and otherwise takes it - the item stays in its bag, locked, and whatever
/// the slot held comes back onto the cursor. Carrying nothing takes the slot's
/// item back onto the cursor. game/auction_sell_slot.hpp has the rule and the
/// measurements.
///
/// It used to take anything and put the cursor down, which unlocked the bag
/// slot without saying so: the button stayed greyed, a soulbound item sat in
/// the slot until the server refused the post, and an item dropped on an
/// occupied slot pushed the old one out of sight instead of onto the cursor.
static int lua_ClickAuctionSellItemButton(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;

    game::BagSlotRef carried;
    if (s_cursorType == CursorType::ITEM) {
        // Bag items only. A worn item or a split stack has no measured answer
        // here, and neither can be named by a bag slot alone, so the drag ends
        // where it started rather than guessing.
        if (cursorItemSlot().equipped || s_cursorSplit > 0 ||
            !game::slots::cursorSourceIsInventory(s_cursorBag)) {
            LOG_WARNING("ClickAuctionSellItemButton: cursor holds bag ", s_cursorBag,
                        " slot ", s_cursorSlot, " split ", s_cursorSplit,
                        " - not a whole bag item, left on the cursor");
            return 0;
        }
        carried = {s_cursorBag, s_cursorSlot};
    } else if (s_cursorType != CursorType::NONE) {
        LOG_WARNING("ClickAuctionSellItemButton: cursor holds a non-item (type ",
                    static_cast<int>(s_cursorType), "), left as it is");
        return 0;
    }

    const char* refusal = nullptr;
    if (!carried.empty()) {
        const auto* held = containerItemSlot(gh->getInventory(), carried.bag, carried.slot);
        if (!held || held->empty()) {
            LOG_WARNING("ClickAuctionSellItemButton: bag ", carried.bag, " slot ",
                        carried.slot, " holds nothing");
            return 0;
        }
        const auto* info = gh->getItemInfo(held->item.itemId);
        if (!info || !info->valid) {
            // Its template decides whether it may be sold, and the template has
            // not arrived. Taking it would be guessing.
            LOG_WARNING("ClickAuctionSellItemButton: no item data yet for ",
                        held->item.itemId, " - not placed");
            return 0;
        }
        refusal = game::auctionSellRefusal(held->item.soulbound, info->bindType,
                                           info->itemFlags);
    }

    const game::AuctionSellClick click =
        game::auctionSellClick(auctionSellItem(), carried, refusal);
    using Kind = game::AuctionSellClick::Kind;
    switch (click.kind) {
        case Kind::Nothing:
            return 0;
        case Kind::Refuse:
            gh->errorGlobal(click.error);
            return 0;
        case Kind::Place:
            auctionSellItem() = click.slot;
            if (click.toCursor.empty()) {
                // Put down without unlocking: the item stays locked in its bag
                // for as long as it is in the sell slot.
                clearCursorItem(L);
            } else {
                pickupFromContainerSlot(L, gh, click.toCursor.bag, click.toCursor.slot,
                                        /*announceLock=*/false);
            }
            gh->fireAddonEvent("NEW_AUCTION_UPDATE", {});
            return 0;
        case Kind::TakeBack:
            auctionSellItem() = click.slot;
            pickupFromContainerSlot(L, gh, click.toCursor.bag, click.toCursor.slot,
                                    /*announceLock=*/false);
            gh->fireAddonEvent("NEW_AUCTION_UPDATE", {});
            return 0;
    }
    return 0;
}

// PickupInventoryItem(slot) - picks up an equipped item
// ClickSendMailItemButton(index, isRightClick) - attach what is held to the
// letter, or take an attachment back.
//
// Lives here rather than with the other mail functions because the cursor
// does, and this is entirely about the cursor: carrying an item means attach
// it, carrying nothing means the slot clicked gives its item back to the bags.
// The attachment index is optional - the drop handler calls this with no
// arguments at all, meaning "attach to wherever there is room", which is what
// attachItemFrom* already does.
static int lua_ClickSendMailItemButton(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;

    if (s_cursorType == CursorType::ITEM) {
        // The cursor counts the backpack as bag zero and the worn bags from
        // one, with slots from one; the attach functions count both from zero.
        const bool attached = (s_cursorBag == 0)
            ? gh->attachItemFromBackpack(s_cursorSlot - 1)
            : (s_cursorBag > 0
                   ? gh->attachItemFromBag(s_cursorBag - 1, s_cursorSlot - 1)
                   : false);
        if (attached) clearCursorItem(L);
        return 0;
    }

    // Nothing held: the click takes an attachment off the letter. Without an
    // index there is nothing to take off, since the caller meant a drop.
    const int index = static_cast<int>(luaL_optnumber(L, 1, 0));
    if (index >= 1) gh->detachMailAttachment(index - 1);
    return 0;
}

// PickupBagFromSlot(inventorySlot) - pick up or put down a bank bag.
//
// The bank's bag buttons hand over an inventory slot rather than an index:
// BankButtonIDToInvSlotID puts the seven bank bags at sixty-eight upward, and
// the wire counts from zero, so the slot sent is one less. That is the same
// arithmetic PickupInventoryItem does for worn equipment, and the cursor
// carries it the same way - a negative bag means "not in a container", which
// cursorWireSlot turns back into container 0xFF and the slot less one.
//
// Deferred for a long time on the belief that this client had no cursor to
// drop things from. It has had one all along, with both halves of a drag in
// one function, and the wire numbers here are the ones its own bank window
// already sends.
/// The nth worn bag, as Lua numbers inventory slots - the four beside the
/// backpack. BagSlotButton_OnDrag passes exactly these.
static constexpr int kFirstWornBagInventorySlot =
    game::slots::toInventorySlot(game::slots::wornBagContainer(0));

static int lua_PickupBagFromSlot(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const int slot = static_cast<int>(luaL_optnumber(L, 1, 0));

    const int bankIndex = slot - game::slots::firstBankBagInventorySlot();
    const int wornIndex = slot - kFirstWornBagInventorySlot;
    const bool isBank = bankIndex >= 0 && bankIndex < game::slots::bankBagCount();
    const bool isWorn = wornIndex >= 0 && wornIndex < game::Inventory::NUM_BAG_SLOTS;
    // Only the bank half was handled, so dragging one of the four worn bags did
    // nothing at all - it returned here before doing anything. Latent until the
    // bag bar was handed over, because this client's own bar never called it.
    if (!isBank && !isWorn) return 0;

    if (droppedItemFromNowhere(L)) return 0;

    // Carrying something: this is the drop, into the bag slot.
    uint8_t srcBag = 0, srcSlot = 0;
    if (cursorWireSlot(srcBag, srcSlot)) {
        const int srcWorn = s_cursorSlot - kFirstWornBagInventorySlot;
        const bool fromWorn = s_cursorBag == -1 &&
                              srcWorn >= 0 && srcWorn < game::Inventory::NUM_BAG_SLOTS;
        if (fromWorn && isWorn) {
            // Both are worn bags, which has its own verb: it swaps the bags'
            // contents locally as well as sending the move, so the bags on
            // screen do not have to wait for the server to agree.
            gh->swapBagSlots(srcWorn, wornIndex);
        } else {
            gh->swapContainerItems(srcBag, srcSlot, 0xFF,
                                   static_cast<uint8_t>(game::slots::toWireSlot(slot)));
        }
        clearCursorItem(L);
        return 0;
    }

    const auto& held = isBank
        ? gh->getInventory().getBankBagItem(bankIndex)
        : gh->getInventory().getEquipSlot(static_cast<game::EquipSlot>(
              static_cast<int>(game::EquipSlot::BAG1) + wornIndex));
    if (held.empty()) {
        // Dragging a bag that is plainly in the slot has been reported doing
        // nothing, and every step of the arithmetic checks out on paper: the
        // button's id, the worn index, the equipment slot. So say what the
        // slot actually held when the drag arrived, rather than returning in
        // silence as this has always done for an empty slot.
        LOG_WARNING("PickupBagFromSlot: slot ", slot, " (worn index ", wornIndex,
                    ", bank index ", bankIndex, ") reads as empty - nothing to pick up");
        return 0;
    }
    setCursorType(L, CursorType::ITEM);
    s_cursorId = held.item.itemId;
    s_cursorSlot = slot;
    s_cursorBag = -1;
    // The shared cursor as well as this file's own. Everything that receives a
    // drop reads cursorItemSlot(), and a worn bag is an equipped item like any
    // other - PickupInventoryItem below sets exactly this. Without it the bag
    // lifted onto the pointer and then every drop target reported nothing
    // held: PutItemInBag returned false, so BagSlotButton_OnClick fell through
    // to ToggleBag and the bag opened instead of moving.
    cursorItemSlot() = {-1, slot, true};
    // Say the slot is locked, as the equipped pickup below does.
    // IsInventoryItemLocked already answers true for it - cursorEquipSlot
    // reads the same s_cursorSlot this just set - but nothing asks again
    // without this, so the square greyed only on the next unrelated redraw.
    // One argument, because the paperdoll's handler tests `not arg2`.
    gh->fireAddonEvent("ITEM_LOCK_CHANGED", {std::to_string(slot)});
    return 0;
}

static int lua_PickupInventoryItem(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    int slot = static_cast<int>(luaL_checknumber(L, 1));
    if (slot < 1 || slot > 23) return 0;

    // Before the pickup, because the paperdoll has no SpellCanTargetItem branch
    // of its own - a left-click on a worn weapon always arrives here, and while
    // a stone or an oil is waiting for a target that click is the target.
    if (slot <= 19 && completedItemTarget(L, gh->getEquipSlotGuid(slot - 1))) return 0;

    // Worn gear is what usually needs repairing, and the paperdoll has no
    // branch of its own for it either.
    if (slot <= 19 && repairedHeldItem(gh, gh->getEquipSlotGuid(slot - 1))) return 0;

    if (droppedItemFromNowhere(L)) return 0;

    // Carrying something: equip it here, which is the other half of the drag.
    uint8_t srcBag = 0, srcSlot = 0;
    if (cursorWireSlot(srcBag, srcSlot)) {
        const auto held = cursorItemSlot();
        const uint32_t droppedId = s_cursorId;
        // An item this would bind stays on the cursor while EQUIP_BIND asks,
        // as native 2.4.3 keeps it; EquipPendingItem finishes the drop.
        if (gh->equipFromWireToSlot(srcBag, srcSlot, static_cast<uint8_t>(slot - 1))) return 0;
        clearCursorItem(L);
        playHeldOrSlotItemSound(gh, droppedId, /*pickup=*/false);
        // The slot it was picked up from is no longer held: without this the
        // bag button stayed greyed after the drop, and for good when the
        // server refused the equip (can't dual wield).
        if (held.equipped) gh->fireAddonEvent("ITEM_LOCK_CHANGED", {std::to_string(held.slot)});
        else gh->fireAddonEvent("ITEM_LOCK_CHANGED",
                                {std::to_string(held.bag), std::to_string(held.slot)});
        gh->fireAddonEvent("ITEM_LOCK_CHANGED", {std::to_string(slot)});
        return 0;
    }

    const auto& inv = gh->getInventory();
    const auto& eq = inv.getEquipSlot(static_cast<game::EquipSlot>(slot - 1));
    if (!eq.empty()) {
        setCursorType(L, CursorType::ITEM);
        s_cursorId = eq.item.itemId;
        s_cursorSlot = slot;
        s_cursorBag = -1;
        playHeldOrSlotItemSound(gh, eq.item.itemId, /*pickup=*/true);
        // What a container pickup does: remember where it came from and say
        // the slot is locked. Without them dragging off the character sheet
        // left the slot looking untouched.
        cursorItemSlot() = {-1, slot, true};
        // One argument, not two: the paperdoll's handler is
        // `if ( not arg2 and arg1 == self:GetID() )`, so an equipment lock is
        // the slot alone. A second argument makes that test fail and the square
        // never greys.
        gh->fireAddonEvent("ITEM_LOCK_CHANGED", {std::to_string(slot)});
        LOG_WARNING("FrameXML pickup: equipment slot ", slot,
                    " item ", eq.item.itemId);
    } else {
        LOG_WARNING("FrameXML pickup: equipment slot ", slot,
                    " - nothing equipped there");
    }
    return 0;
}

// DeleteCursorItem() - destroys the item on the cursor.
//
// It only put the cursor down. Nothing was ever destroyed: the item vanished
// from the cursor, the server was never told, and the next bag update put it
// straight back. StaticPopup "DELETE_ITEM" calls this on Accept, so the whole
// drag-an-item-out-to-destroy-it gesture ended in the item reappearing.
//
// cursorWireSlot already converts the held position into the bag and slot the
// server expects, which is the same translation the drop half of a drag uses.
static int lua_DeleteCursorItem(lua_State* L) {
    auto* gh = getGameHandler(L);
    uint8_t bag = 0, slot = 0;
    if (gh && cursorWireSlot(bag, slot)) {
        // The whole stack: WoW asks about a partial stack before it gets here,
        // and the popup that calls this is the confirmation for all of it.
        gh->destroyItem(bag, slot, 0);
    }
    clearCursorItem(L);
    return 0;
}

// EquipCursorItem(slot) - the held item into that equipment slot, as a drop
// on the paperdoll does. It was missing: FrameXML does not call it, but
// macros and addons do, and native 2.4.3 answers it with EQUIP_BIND for an
// item it would bind.
static int lua_EquipCursorItem(lua_State* L) {
    if (s_cursorType != CursorType::ITEM || s_cursorId == 0) return 0;
    const int slot = static_cast<int>(luaL_checknumber(L, 1));
    if (slot < 1 || slot > 19) return 0;
    return lua_PickupInventoryItem(L);
}

// AutoEquipCursorItem() - equip item from cursor
static int lua_AutoEquipCursorItem(lua_State* L) {
    auto* gh = getGameHandler(L);
    // From the bag slot it was lifted from, through the bind check: native
    // 2.4.3 answers AUTOEQUIP_BIND_CONFIRM for a ring it would bind and keeps
    // the ring on the cursor while it asks (EquipPendingItem clears it).
    uint8_t bag = 0, slot = 0;
    if (gh && cursorWireSlot(bag, slot)) {
        constexpr uint8_t kBackpackBag = 0xFF;
        const int backpackIndex = slot - game::Inventory::NUM_EQUIP_SLOTS;
        const int bagIndex = bag - game::Inventory::FIRST_BAG_EQUIP_SLOT;
        if (bag == kBackpackBag && backpackIndex >= 0) {
            const bool asks = gh->equipWouldBindFromBackpack(backpackIndex);
            gh->autoEquipItemBySlot(backpackIndex);
            if (asks) return 0;
        } else if (bagIndex >= 0 && bagIndex < game::Inventory::NUM_BAG_SLOTS) {
            const bool asks = gh->equipWouldBindFromBag(bagIndex, slot);
            gh->autoEquipItemInBag(bagIndex, slot);
            if (asks) return 0;
        } else {
            gh->useItemById(s_cursorId);
        }
    } else if (gh && s_cursorType == CursorType::ITEM && s_cursorId != 0) {
        gh->useItemById(s_cursorId);
    }
    clearCursorItem(L);
    return 0;
}

// --- Frame System ---
// Minimal WoW-compatible frame objects with RegisterEvent/SetScript/GetScript.
// Frames are Lua tables with a metatable that provides methods.

// Frame method: frame:RegisterEvent("EVENT")

// The modifier state comes from the keyboard rather than from ImGui, which
// learns it from events this client does not always route there once FrameXML
// owns the mouse. lua_system_api registers the same three names against
// SDL_GetModState; these agree with it now instead of racing it, and the
// duplicate Is*KeyDown bindings that used to live here are gone with them.
static bool shiftHeld() { return (SDL_GetModState() & SDL_KMOD_SHIFT) != 0; }
static bool ctrlHeld()  { return (SDL_GetModState() & SDL_KMOD_CTRL)  != 0; }
static bool altHeld()   { return (SDL_GetModState() & SDL_KMOD_ALT)   != 0; }


/// What bindings.xml declares for a modified-click action, or empty.
///
/// One list, read once. The emitter keeps every <ModifiedClick action= default=>
/// in __WoweeModifiedClick, which is the same file the key bindings panel is
/// built from - so this cannot drift from what the interface believes.
///
/// It had drifted. A table written out by hand here answered ALT for FOCUSCAST,
/// which bindings.xml declares as NONE, so alt-clicking a spell tried to cast
/// it at the focus when nothing had asked for that; and both flyout actions are
/// declared ALT and fell through to a shift default.
static std::string modifiedClickBinding(lua_State* L, const std::string& action) {
    lua_getglobal(L, "__WoweeModifiedClick");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return {}; }
    lua_getfield(L, -1, action.c_str());
    const char* value = lua_tostring(L, -1);
    std::string out = value ? value : "";
    lua_pop(L, 2);
    return out;
}

/// Whether the modifiers a binding names are all held.
///
/// A binding is a dash-separated list ending in an optional button -
/// "SHIFT-BUTTON1", "CTRL", "NONE". The button half is the caller's business:
/// it already knows which button was pressed, and every caller here is inside
/// a handler for one.
static bool modifiersHeldFor(const std::string& binding) {
    if (binding.empty() || binding == "NONE") return false;
    bool wantShift = false, wantCtrl = false, wantAlt = false;
    std::string wantButton;
    size_t at = 0;
    while (at <= binding.size()) {
        const size_t dash = binding.find('-', at);
        const std::string part = binding.substr(
            at, dash == std::string::npos ? std::string::npos : dash - at);
        if (part == "SHIFT") wantShift = true;
        else if (part == "CTRL") wantCtrl = true;
        else if (part == "ALT") wantAlt = true;
        // A binding names a button as often as it names a modifier, and the
        // two named actions on the same modifier are told apart by nothing
        // else: CHATLINK is SHIFT-BUTTON1 and SOCKETITEM is SHIFT-BUTTON2.
        // Reading only the modifier made both true on any shift-click, so
        // shift-clicking an item in a bag put the link in chat and opened the
        // socketing window behind it - or, when the link could not be
        // inserted, only the socketing window.
        else if (part == "BUTTON1") wantButton = "LeftButton";
        else if (part == "BUTTON2") wantButton = "RightButton";
        if (dash == std::string::npos) break;
        at = dash + 1;
    }
    if (!wantShift && !wantCtrl && !wantAlt) return false;
    if (!wantButton.empty()) {
        // Only while a click is being dispatched. Asked at any other moment -
        // an OnUpdate deciding what a tooltip should say - the modifier alone
        // is the whole of the question, and the button has not been pressed.
        const std::string& clicking = currentClickButton();
        if (!clicking.empty() && clicking != wantButton) return false;
    }
    return (!wantShift || shiftHeld()) && (!wantCtrl || ctrlHeld()) &&
           (!wantAlt || altHeld());
}

// IsModifiedClick(action) → boolean
static int lua_IsModifiedClick(lua_State* L) {
    const char* action = luaL_optstring(L, 1, "");
    std::string act(action);
    for (char& c : act) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (act.empty()) {
        // No action named means "is this click modified at all", which is how
        // the paperdoll and the bags branch: IsModifiedClick() picks between
        // OnModifiedClick and the plain OnClick. Answering with shift alone
        // sent every ctrl-click down the plain path, so ctrl-clicking a worn
        // item picked it up instead of putting it in the dressing room.
        lua_pushboolean(L, (shiftHeld() || ctrlHeld() || altHeld()) ? 1 : 0);
        return 1;
    }
    const std::string binding = modifiedClickBinding(L, act);
    const bool answer = binding.empty() ? shiftHeld() : modifiersHeldFor(binding);
    // The gate the comparison tooltip goes through, said once each way.
    //
    // Shift-hovering an item that differs from the worn one produced no
    // refusal from SetHyperlinkCompareItem at all, which means FrameXML never
    // got as far as calling it - and this is what it asks first. Whether the
    // answer is yes or no, and which binding decided it, is the difference
    // between a modifier this client never sees and a comparison that is
    // refused further along.
    if (act == "COMPAREITEMS") {
        static bool saidYes = false, saidNo = false;
        bool& said = answer ? saidYes : saidNo;
        if (!said) {
            said = true;
            LOG_WARNING("IsModifiedClick(COMPAREITEMS) = ", answer ? "true" : "false",
                        ", binding '", binding.empty() ? "(none - falls back to shift)" : binding,
                        "', shift held ", shiftHeld() ? "yes" : "no");
        }
    }
    lua_pushboolean(L, answer ? 1 : 0);
    return 1;
}

// GetModifiedClick(action) → the binding it is on ("SHIFT", "CTRL-BUTTON1", "NONE")
static int lua_GetModifiedClick(lua_State* L) {
    const char* action = luaL_optstring(L, 1, "");
    std::string act(action);
    for (char& c : act) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    std::string binding = modifiedClickBinding(L, act);
    if (binding.empty()) {
        std::string stored = "modifiedclick_" + act;
        for (char& c : stored) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const char* def = (act == "SELFCAST") ? "ALT" : (act == "FOCUSCAST" ? "NONE" : "SHIFT");
        binding = storedCVarValue(stored, def);
    }
    lua_pushstring(L, binding.empty() ? "SHIFT" : binding.c_str());
    return 1;
}

// SetModifiedClick(action, binding) - rebinding one from the options panel.
//
// It wrote nowhere, so the panel accepted a change and the next question about
// that action answered the old value. The table the emitter fills is the same
// one read above, so writing to it is the whole of it.
static int lua_SetModifiedClick(lua_State* L) {
    const char* action = luaL_optstring(L, 1, "");
    const char* binding = luaL_optstring(L, 2, "NONE");
    if (!action || !*action) return 0;
    std::string act(action);
    for (char& c : act) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    lua_getglobal(L, "__WoweeModifiedClick");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setglobal(L, "__WoweeModifiedClick");
    }
    lua_pushstring(L, binding);
    lua_setfield(L, -2, act.c_str());
    lua_pop(L, 1);
    // And the store, so it comes back next time. Bindings.xml rewrites the
    // table with its defaults at every load, and SaveBindings writes keys,
    // not clicks: nothing else kept the self-cast, focus-cast and auto-loot
    // keys past the session they were set in.
    std::string stored = "modifiedclick_" + act;
    for (char& c : stored) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    setStoredCVar(stored, binding);
    return 0;
}

bool isModifiedClickActive(lua_State* L, const std::string& action,
                           bool shift, bool ctrl, bool alt) {
    std::string act = action;
    for (char& c : act) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    std::string binding = L ? modifiedClickBinding(L, act) : std::string{};
    if (binding.empty()) {
        std::string stored = "modifiedclick_" + act;
        for (char& c : stored) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const char* def = (act == "SELFCAST") ? "ALT" : (act == "FOCUSCAST" ? "NONE" : "SHIFT");
        binding = storedCVarValue(stored, def);
    }
    if (binding.empty() || binding == "NONE") return false;
    bool wantShift = false, wantCtrl = false, wantAlt = false;
    size_t at = 0;
    while (at <= binding.size()) {
        const size_t dash = binding.find('-', at);
        const std::string part = binding.substr(
            at, dash == std::string::npos ? std::string::npos : dash - at);
        if (part == "SHIFT") wantShift = true;
        else if (part == "CTRL") wantCtrl = true;
        else if (part == "ALT") wantAlt = true;
        if (dash == std::string::npos) break;
        at = dash + 1;
    }
    if (!wantShift && !wantCtrl && !wantAlt) return false;
    return (!wantShift || shift) && (!wantCtrl || ctrl) && (!wantAlt || alt);
}

// --- Trading ---
//
// Placing an item is the only part that needs the cursor, which is why these
// live here. The peer's side is read-only: a trade slot of theirs can be looked
// at and not touched, so clicking one does nothing rather than pretending.

static int pushTradeSlot(lua_State* L, game::GameHandler* gh,
                         const game::GameHandler::TradeSlot& slot, bool targetSide) {
    if (!slot.occupied || slot.itemId == 0) { return luaReturnNil(L); }
    const auto* info = gh->getItemInfo(slot.itemId);
    lua_pushstring(L, info ? info->name.c_str() : "");
    lua_pushstring(L, gh->getItemIconPath(
        info && info->displayInfoId ? info->displayInfoId : slot.displayId).c_str());
    lua_pushnumber(L, slot.stackCount);
    if (targetSide) lua_pushnumber(L, info ? static_cast<int>(info->quality) : 0);
    lua_pushboolean(L, 1);       // isUsable
    lua_pushnil(L);              // enchantment: not carried by the trade packet
    return targetSide ? 6 : 5;
}

static int lua_GetTradePlayerItemInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int i = static_cast<int>(luaL_optnumber(L, 1, 0));
    if (!gh || i < 1 || i > game::GameHandler::TRADE_SLOT_COUNT) { return luaReturnNil(L); }
    return pushTradeSlot(L, gh, gh->getMyTradeSlots()[i - 1], false);
}

static int lua_GetTradeTargetItemInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int i = static_cast<int>(luaL_optnumber(L, 1, 0));
    if (!gh || i < 1 || i > game::GameHandler::TRADE_SLOT_COUNT) { return luaReturnNil(L); }
    return pushTradeSlot(L, gh, gh->getPeerTradeSlots()[i - 1], true);
}

static int pushTradeLink(lua_State* L, game::GameHandler* gh,
                         const game::GameHandler::TradeSlot& slot) {
    const auto* info = (gh && slot.occupied) ? gh->getItemInfo(slot.itemId) : nullptr;
    if (!info || info->name.empty()) { return luaReturnNil(L); }
    const std::string link = game::itemChatLink(slot.itemId, static_cast<uint32_t>(info->quality), info->name);
    lua_pushstring(L, link.c_str());
    return 1;
}

static int lua_GetTradePlayerItemLink(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int i = static_cast<int>(luaL_optnumber(L, 1, 0));
    if (!gh || i < 1 || i > game::GameHandler::TRADE_SLOT_COUNT) { return luaReturnNil(L); }
    return pushTradeLink(L, gh, gh->getMyTradeSlots()[i - 1]);
}

static int lua_GetTradeTargetItemLink(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int i = static_cast<int>(luaL_optnumber(L, 1, 0));
    if (!gh || i < 1 || i > game::GameHandler::TRADE_SLOT_COUNT) { return luaReturnNil(L); }
    return pushTradeLink(L, gh, gh->getPeerTradeSlots()[i - 1]);
}

// ClickTradeButton(slot) - put what is held into the slot, or take back what is
// already there when nothing is held
static int lua_ClickTradeButton(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int i = static_cast<int>(luaL_optnumber(L, 1, 0));
    if (!gh || i < 1 || i > game::GameHandler::TRADE_SLOT_COUNT) return 0;
    uint8_t srcBag = 0, srcSlot = 0;
    // Through the same translation as every other send. This passed FrameXML's
    // own numbering straight to the server - bags counted 0 to 4 and slots from
    // one - where the wire wants the flat space GetItemByPos reads: container
    // 255 with an absolute slot, or a worn bag's equipment slot with a 0-based
    // index inside it. The server finds no item at the slot it was given and
    // answers TRADE_STATUS_TRADE_CANCELED, so offering an item ended the trade.
    if (s_cursorType == CursorType::ITEM && s_cursorBag >= 0 &&
        cursorWireSlot(srcBag, srcSlot)) {
        gh->setTradeItem(static_cast<uint8_t>(i - 1), srcBag, srcSlot);
        clearCursorItem(L);
    } else {
        gh->clearTradeItem(static_cast<uint8_t>(i - 1));
    }
    return 0;
}

// The other side's slots belong to the other player.
static int lua_ClickTargetTradeButton(lua_State* L) { (void)L; return 0; }

static int lua_SetTradeMoney(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (gh) gh->setTradeGold(static_cast<uint64_t>(luaL_optnumber(L, 1, 0)));
    return 0;
}

static int lua_CloseTrade(lua_State* L) {
    if (auto* gh = getGameHandler(L)) gh->cancelTrade();
    return 0;
}

static int lua_BeginTrade(lua_State* L) {
    if (auto* gh = getGameHandler(L)) gh->acceptTradeRequest();
    return 0;
}

// --- Keybinding API ---
//
// Which commands exist, and in what order, comes from bindings.xml: the emitter
// turns it into __WoweeBindings (the list the UI walks, header rows included)
// and __WoweeBindingScripts (what each one runs). What each is *bound to* lives
// here instead, because it outlives any one Lua state and has to reach a file.

namespace {

// Constructing defaults needs no Lua; restoring saved user settings is separate.
std::string bindingsFilePath() { return core::getConfigRoot() + "/bindings.cfg"; }

int bindingCount(lua_State* L);
std::string bindingAt(lua_State* L, int index);

/// The expansion's WTF\\DefaultBindings.wtf, kept to commands this FrameXML's
/// Bindings.xml declares, hidden and debug ones included.
/// False only while Bindings.xml has not loaded, so the caller asks again.
bool applyDataDefaultBindings(lua_State* L) {
    // No Application in the unit tests: the built-in defaults stand there.
    if (!core::Application::hasInstance()) return true;
    auto* assets = core::Application::getInstance().getAssetManager();
    if (!assets) return false;  // not up yet; asked again on the next call
    const auto data = assets->readFileOptional("WTF\\DefaultBindings.wtf");
    if (data.empty()) {
        LOG_WARNING("Key bindings: this expansion's data has no WTF\\DefaultBindings.wtf; built-in defaults kept");
        return true;
    }
    std::set<std::string> declared;
    for (int i = 1, n = bindingCount(L); i <= n; ++i) declared.insert(bindingAt(L, i));
    // Hidden and debug commands are bound as well (TURNORACTION, TOGGLESTATS);
    // only the listing leaves them out.
    lua_getglobal(L, "__WoweeBindingHidden");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            if (lua_type(L, -2) == LUA_TSTRING) declared.insert(lua_tostring(L, -2));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    if (declared.empty()) return false;
    std::vector<std::pair<std::string, std::string>> rows;
    std::string text(data.begin(), data.end());
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        start = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.starts_with("bind ")) continue;
        const size_t space = line.find(' ', 5);
        if (space == std::string::npos) continue;
        std::string key = line.substr(5, space - 5);
        std::string command = line.substr(space + 1);
        if (!declared.contains(command)) continue;
        rows.emplace_back(std::move(key), std::move(command));
    }
    LOG_INFO("Key bindings: ", rows.size(), " defaults from DefaultBindings.wtf");
    wowee::ui::KeybindingManager::getInstance().setKeyboardDefaults(std::move(rows));
    return true;
}

void seedBindingDefaults(lua_State* L) {
    static bool seeded = false;
    if (seeded) return;
    if (!applyDataDefaultBindings(L)) return;  // Bindings.xml not loaded yet
    seeded = true;
    wowee::ui::KeybindingManager::getInstance().loadBindingsWithLegacy(
        bindingsFilePath(), wowee::ui::SettingsPanel::getSettingsPath());
}

/// The command list the emitter built, or zero if bindings.xml never loaded.
int bindingCount(lua_State* L) {
    lua_getglobal(L, "__WoweeBindings");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return 0; }
    const int n = static_cast<int>(lua_objlen(L, -1));
    lua_pop(L, 1);
    return n;
}

/// The command at a one-based position, empty when out of range.
std::string bindingAt(lua_State* L, int index) {
    lua_getglobal(L, "__WoweeBindings");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return ""; }
    lua_rawgeti(L, -1, index);
    const char* s = lua_tostring(L, -1);
    std::string out = s ? s : "";
    lua_pop(L, 2);
    return out;
}

}  // namespace

// GetBindingKey(command) → key1, key2 (or nil)
static int pushBindingKeys(lua_State* L, const std::string& command, bool controller) {
    const auto& keys = wowee::ui::KeybindingManager::getInstance().bindings(controller
        ? wowee::ui::KeybindingManager::Device::Controller : wowee::ui::KeybindingManager::Device::KeyboardMouse);
    auto it = keys.find(command);
    if (it == keys.end()) { lua_pushnil(L); lua_pushnil(L); return 2; }
    for (const std::string& key : it->second) {
        if (key.empty()) lua_pushnil(L); else lua_pushstring(L, key.c_str());
    }
    return 2;
}

static int lua_GetBindingKey(lua_State* L) {
    seedBindingDefaults(L);
    return pushBindingKeys(L, luaL_checkstring(L, 1), false);
}

static int lua_GetControllerBindingKey(lua_State* L) {
    seedBindingDefaults(L);
    return pushBindingKeys(L, luaL_checkstring(L, 1), true);
}

// GetBindingAction(key) → command, or "" for a key that holds nothing
//
// Empty rather than nil, which is what the real client answers and what the
// interface is written against. Blizzard's key binding panel reads it as
//
//     local oldAction = GetBindingAction(keyPressed, KeyBindingFrame.mode);
//     if ( oldAction ~= "" and oldAction ~= KeyBindingFrame.selected ) then
//         local key1, key2 = GetBindingKey(oldAction, ...)
//
// and nil is not equal to "", so a nil sent the panel into that branch with
// nil in hand: "bad argument #1 to 'GetBindingKey' (string expected, got
// nil)", thrown out of OnKeyDown before the new binding was ever set. Every
// attempt to bind a key that was not already bound to something failed that
// way - a controller button being the case where that is always true.
static int lua_GetBindingAction(lua_State* L) {
    seedBindingDefaults(L);
    const std::string key = luaL_checkstring(L, 1);
    lua_pushstring(L, wowee::ui::KeybindingManager::getInstance().commandForKey(key).c_str());
    return 1;
}

// GetNumBindings() → how many rows the list has, headers included
static int lua_GetNumBindings(lua_State* L) {
    lua_pushinteger(L, bindingCount(L));
    return 1;
}

// GetBinding(index) → command, key1, key2. A header row is a command like
// "HEADER_MOVEMENT" with no keys, which is how the UI tells the two apart.
static int lua_GetBinding(lua_State* L) {
    seedBindingDefaults(L);
    const int index = static_cast<int>(luaL_checkinteger(L, 1));
    const std::string command = bindingAt(L, index);
    if (command.empty()) { lua_pushnil(L); return 1; }
    lua_pushstring(L, command.c_str());
    pushBindingKeys(L, command, false);
    return 3;
}

static int lua_GetControllerBinding(lua_State* L) {
    seedBindingDefaults(L);
    const std::string command = bindingAt(L, static_cast<int>(luaL_checkinteger(L, 1)));
    if (command.empty()) { lua_pushnil(L); return 1; }
    lua_pushstring(L, command.c_str());
    pushBindingKeys(L, command, true);
    return 3;
}

// Editor-only slot edits retain holes; ordinary SetBinding keeps its addon API.
static int lua_SetBindingSlot(lua_State* L) {
    seedBindingDefaults(L);
    const std::string command = luaL_checkstring(L, 1);
    const int slot = static_cast<int>(luaL_checkinteger(L, 2)) - 1;
    const char* key = lua_isnoneornil(L, 3) ? "" : luaL_checkstring(L, 3);
    const bool controller = lua_toboolean(L, 4);
    auto& manager = wowee::ui::KeybindingManager::getInstance();
    auto& keys = manager.bindings(controller ? wowee::ui::KeybindingManager::Device::Controller
                                           : wowee::ui::KeybindingManager::Device::KeyboardMouse);
    if (slot < 0 || slot > 1 || command.empty() || command.starts_with("HEADER_") ||
        std::any_of(command.begin(), command.end(), [](unsigned char c) { return c < 32 || c == 127; }) ||
        (*key && (manager.isControllerKey(key) != controller))) {
        lua_pushboolean(L, 0);
        return 1;
    }
    // Validate and resolve conflicts before touching this command's old slots.
    const auto existing = keys.find(command);
    const auto old = existing == keys.end() ? std::array<std::string, 2>{} : existing->second;
    if (*key && !manager.setBinding(key, command)) { lua_pushboolean(L, 0); return 1; }
    const std::string assigned = *key ? manager.resolveBindingKey(key) : "";
    auto desired = old;
    if (desired[1 - slot] == assigned) desired[1 - slot].clear();
    desired[slot] = assigned;
    keys[command] = std::move(desired);
    if (auto* gh = getGameHandler(L)) gh->fireAddonEvent("UPDATE_BINDINGS", {});
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_ResetKeyboardBindings(lua_State* L) {
    seedBindingDefaults(L);
    wowee::ui::KeybindingManager::getInstance().resetToDefaults(false);
    if (auto* gh = getGameHandler(L)) gh->fireAddonEvent("UPDATE_BINDINGS", {});
    return 0;
}

static int lua_ResetControllerBindings(lua_State* L) {
    seedBindingDefaults(L);
    wowee::ui::KeybindingManager::getInstance().resetToDefaults(true);
    if (auto* gh = getGameHandler(L)) gh->fireAddonEvent("UPDATE_BINDINGS", {});
    return 0;
}

// SetBinding(key, command) → true. A nil command clears whatever holds the key,
// which is how the UI unbinds.
static int lua_SetBinding(lua_State* L) {
    seedBindingDefaults(L);
    const std::string key = luaL_checkstring(L, 1);
    const char* command = lua_isnoneornil(L, 2) ? nullptr : luaL_checkstring(L, 2);

    if (!wowee::ui::KeybindingManager::getInstance().setBinding(key, command ? command : "")) {
        lua_pushboolean(L, 0);
        return 1;
    }
    // Six frames wait on this, the action buttons among them: it is what
    // redraws the little key printed in the corner. Without it a rebind takes
    // effect while every button goes on showing the old key.
    if (auto* gh = getGameHandler(L)) gh->fireAddonEvent("UPDATE_BINDINGS", {});
    lua_pushboolean(L, 1);
    return 1;
}

// SaveBindings(which) → writes what is bound where, for the next run
static int lua_SaveBindings(lua_State* L) {
    seedBindingDefaults(L);
    const int which = static_cast<int>(luaL_optinteger(L, 1, 1));
    if (which == 1 || which == 2) {
        setStoredCVar("wowee_bindingset", which == 2 ? "2" : "1");
    }
    lua_pushboolean(L, wowee::ui::KeybindingManager::getInstance().saveBindings(bindingsFilePath()));
    return 1;
}

// LoadBindings(which) → the defaults for DEFAULT_BINDINGS (0), otherwise what
// a previous run saved over them. The key binding panel's Default button
// passes 0 and its Cancel passes the current set.
static int lua_LoadBindings(lua_State* L) {
    seedBindingDefaults(L);
    auto& manager = wowee::ui::KeybindingManager::getInstance();
    const auto previous = manager;
    const bool defaultsOnly = luaL_optinteger(L, 1, 1) == 0;
    const bool loaded = defaultsOnly || manager.loadBindingsWithLegacy(bindingsFilePath(), wowee::ui::SettingsPanel::getSettingsPath());
    if (defaultsOnly) manager.resetToDefaults();
    if (!loaded) manager = previous;
    if (loaded) if (auto* gh = getGameHandler(L)) gh->fireAddonEvent("UPDATE_BINDINGS", {});
    lua_pushboolean(L, loaded);
    return 1;
}

// RunBinding(command, keystate) → runs what bindings.xml says the command does
static int lua_RunBinding(lua_State* L) {
    const std::string command = luaL_checkstring(L, 1);
    const char* keystate = lua_isnoneornil(L, 2) ? "down" : luaL_checkstring(L, 2);

    lua_getglobal(L, "__WoweeBindingScripts");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return 0; }
    lua_getfield(L, -1, command.c_str());
    if (!lua_isfunction(L, -1)) { lua_pop(L, 2); return 0; }
    lua_pushstring(L, keystate);
    if (lua_pcall(L, 1, 0, 0) != 0) {
        LOG_WARNING("Binding ", command, " failed: ",
                    lua_tostring(L, -1) ? lua_tostring(L, -1) : "?");
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return 0;
}
static std::string overrideOwnerId(lua_State* L, int idx) {
    if (const void* ptr = lua_topointer(L, idx)) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%p", ptr);
        return buf;
    }
    if (const char* s = lua_tostring(L, idx)) return s;
    return {};
}

static int applyOverrideBinding(lua_State* L, const std::string& command) {
    seedBindingDefaults(L);
    const std::string owner = overrideOwnerId(L, 1);
    const bool isPriority = lua_toboolean(L, 2) != 0;
    const char* key = luaL_optstring(L, 3, nullptr);
    if (owner.empty() || !key || !*key) return 0;
    if (wowee::ui::KeybindingManager::getInstance().setOverrideBinding(owner, isPriority, key, command)) {
        if (auto* gh = getGameHandler(L)) gh->fireAddonEvent("UPDATE_BINDINGS", {});
    }
    return 0;
}

static int lua_SetOverrideBinding(lua_State* L) {
    const char* cmd = lua_isnoneornil(L, 4) ? "" : luaL_checkstring(L, 4);
    return applyOverrideBinding(L, cmd);
}
static int lua_SetOverrideBindingClick(lua_State* L) {
    const char* buttonName = luaL_optstring(L, 4, nullptr);
    if (!buttonName || !*buttonName) return applyOverrideBinding(L, "");
    const char* mouseClick = luaL_optstring(L, 5, "LeftButton");
    std::string cmd = "CLICK ";
    cmd += buttonName;
    cmd += ":";
    cmd += (mouseClick && *mouseClick) ? mouseClick : "LeftButton";
    return applyOverrideBinding(L, cmd);
}
static int lua_SetOverrideBindingSpell(lua_State* L) {
    const char* spell = luaL_optstring(L, 4, nullptr);
    return applyOverrideBinding(L, (spell && *spell) ? std::string("SPELL ") + spell : "");
}
static int lua_SetOverrideBindingMacro(lua_State* L) {
    const char* macro = luaL_optstring(L, 4, nullptr);
    return applyOverrideBinding(L, (macro && *macro) ? std::string("MACRO ") + macro : "");
}
static int lua_SetOverrideBindingItem(lua_State* L) {
    const char* item = luaL_optstring(L, 4, nullptr);
    return applyOverrideBinding(L, (item && *item) ? std::string("ITEM ") + item : "");
}
static int lua_ClearOverrideBindings(lua_State* L) {
    seedBindingDefaults(L);
    const std::string owner = overrideOwnerId(L, 1);
    if (owner.empty()) return 0;
    wowee::ui::KeybindingManager::getInstance().clearOverrideBindings(owner);
    if (auto* gh = getGameHandler(L)) gh->fireAddonEvent("UPDATE_BINDINGS", {});
    return 0;
}

// Frame methods: SetPoint, SetSize, SetWidth, SetHeight, GetWidth, GetHeight, GetCenter, SetAlpha, GetAlpha

void registerActionLuaAPI(lua_State* L) {
    seedBindingDefaults(L);
    static const struct { const char* name; lua_CFunction func; } api[] = {
                {"HasAction",           lua_HasAction},
                // A macro's name, which only a macro has; ActionButton_Update
                // shows it under the icon and expects nothing for a spell.
                // GetActionText(slot) → the name written under the button.
                //
                // Only a macro has one, which is what the action button draws
                // in its Name font string - so with nil answered for every slot
                // a bar full of macros was a row of nameless icons, all of them
                // the same question mark unless the player had picked an icon.
                // The names were there: GetMacroInfo has been reading them off
                // the same handler all along.
                {"GetActionText", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int slot = static_cast<int>(luaL_optnumber(L, 1, 0)) - 1;
            if (!gh || slot < 0) { lua_pushnil(L); return 1; }
            const auto& bar = gh->getActionBar();
            if (slot >= static_cast<int>(bar.size())) { lua_pushnil(L); return 1; }
            const auto& action = bar[static_cast<size_t>(slot)];
            if (action.type != game::ActionBarSlot::MACRO) { lua_pushnil(L); return 1; }
            const std::string name = gh->getMacroName(action.id);
            if (name.empty()) { lua_pushnil(L); return 1; }
            lua_pushstring(L, name.c_str());
            return 1;
        }},
                // Whether the action has a range to be in or out of. Nothing
                // here tracks that yet, and false is what "no range check"
                // looks like - the button simply never dims for distance.
                // Whether the action's spell has a range: SpellRange.dbc max
                // above 0. Native 2.4.3: Sinister Strike, Distract, Slice
                // and Dice, Linen Bandage (First Aid 15 yd) yes; Attack,
                // Stealth, Sprint, Hearthstone, potions (self only) no; a
                // macro answers for its spell or item.
                {"ActionHasRange",      [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const auto* shown = gh ? game::shownAction(*gh, static_cast<int>(luaL_optnumber(L, 1, 0)) - 1) : nullptr;
            std::optional<game::ActionBarSlot> standIn;
            if (shown && shown->type == game::ActionBarSlot::MACRO) {
                standIn = macroStandIn(*gh, resolveMacroTarget(*gh, shown->id));
                shown = standIn ? &*standIn : nullptr;
            }
            uint32_t spellId = 0;
            if (shown && shown->type == game::ActionBarSlot::SPELL) {
                spellId = shown->id;
            } else if (shown && shown->type == game::ActionBarSlot::ITEM) {
                if (const auto* info = gh->getItemInfo(shown->id); info && info->valid)
                    for (const auto& spell : info->spells)
                        if (spell.spellId != 0 && spell.spellTrigger == 0) { spellId = spell.spellId; break; }
            }
            const auto* entry = spellId ? gh->getSpellNameEntry(spellId) : nullptr;
            lua_pushboolean(L, entry && entry->maxRange > 0.0f ? 1 : 0);
            return 1;
        }},
                {"GetActionTexture",    lua_GetActionTexture},
                {"IsCurrentAction",     lua_IsCurrentAction},
                {"IsUsableAction",      lua_IsUsableAction},
                {"IsActionInRange",     lua_IsActionInRange},
                {"GetActionInfo",       lua_GetActionInfo},
                {"GetActionCount",      lua_GetActionCount},
                {"GetActionCooldown",   lua_GetActionCooldown},
                {"UseAction",           lua_UseAction},
                {"PickupAction",        lua_PickupAction},
                {"PlaceAction",         lua_PlaceAction},
                {"PickupSpell",         lua_PickupSpell},
                {"PickupCompanion",     lua_PickupCompanion},
                {"PickupGuildBankItem", lua_PickupGuildBankItem},
                {"PlaceGlyphInSocket",  lua_PlaceGlyphInSocket},
                {"SplitGuildBankItem",  lua_SplitGuildBankItem},
                {"PickupSpellBookItem", lua_PickupSpellBookItem},
                {"PickupContainerItem", lua_PickupContainerItem},
                {"PickupItem",          lua_PickupItem},
                // The totem bar's summon flyout calls these two from its
                // OnClick and OnLeave, and nothing in this interface defines
                // them - not multicastactionbarframe.lua beside the XML that
                // calls them, not anywhere. An upstream gap rather than one of
                // this client's, but the raise lands here all the same, and it
                // lands on a shaman clicking their own totem bar.
                //
                // Answered with nothing, which makes the button inert. There
                // is no honest alternative: the missing function is what would
                // have decided which totem the flyout chose, and inventing one
                // would summon whatever this guessed.
                {"MultiCastSummonSpellButtonFlyoutButton_OnClick", lua_ReturnNothing2},
                {"MultiCastSummonSpellButtonFlyoutButton_OnLeave", lua_ReturnNothing2},
                {"PickupInventoryItem", lua_PickupInventoryItem},
                {"PickupBagFromSlot",   lua_PickupBagFromSlot},
                {"ClickSendMailItemButton", lua_ClickSendMailItemButton},
                {"ClickAuctionSellItemButton", lua_ClickAuctionSellItemButton},
                {"ClearCursor",         lua_ClearCursor},
                {"GetCursorInfo",       lua_GetCursorInfo},
                {"PickupPetAction",     lua_PickupPetAction},
                {"CursorHasItem",       lua_CursorHasItem},
                {"CursorHasSpell",      lua_CursorHasSpell},
                {"CursorHasMacro",      lua_CursorHasMacro},
                {"CursorHasMoney",      lua_CursorHasMoney},
                {"DeleteCursorItem",    lua_DeleteCursorItem},
                {"AutoEquipCursorItem", lua_AutoEquipCursorItem},
                {"EquipCursorItem",     lua_EquipCursorItem},
                {"IsModifiedClick",     lua_IsModifiedClick},
                {"GetModifiedClick",    lua_GetModifiedClick},
                {"SetModifiedClick",    lua_SetModifiedClick},
                // Macros. The client has stored their text all along and
                // persisted it per character; only the interface's way in was
                // missing, so GetNumMacros answered zero and the panel opened
                // empty over a full set.
                //
                // Every verb here takes an index - the macro's place in its
                // half, sorted by name - and the client stores macros by
                // identity; see GameHandler::macroIdForIndex.
                {"GetNumMacros",        [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            int global = 0, perChar = 0;
            if (gh) {
                for (uint32_t id : gh->getMacroIds()) {
                    if (id & game::GameHandler::kCharacterMacroFlag) ++perChar; else ++global;
                }
            }
            lua_pushnumber(L, global);
            lua_pushnumber(L, perChar);
            return 2;
        }},
                // GetMacroInfo(index) → name, icon, body
                {"GetMacroInfo",        [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const uint32_t id = macroIdArg(L, gh);
            if (!gh || id == 0 || !gh->hasMacro(id)) {
                return luaReturnNil(L);
            }
            std::string name = gh->getMacroName(id);
            if (name.empty()) name = "Macro";
            // The question mark stands for the macro's spell or item icon:
            // native "/cast Sinister Strike" with icon 1 answers
            // Spell_Shadow_RitualOfSacrifice.
            lua_pushstring(L, name.c_str());
            lua_pushstring(L, macroTexture(*gh, id).c_str());
            lua_pushstring(L, gh->getMacroText(id).c_str());
            return 3;
        }},
                // GetMacroSpell(index) -> name, rank of the spell the macro
                // stands for (#showtooltip or its first /cast); native
                // "#showtooltip Kick\n/say x" answers "Kick", "Rank 5".
                {"GetMacroSpell",       [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const uint32_t id = macroIdArg(L, gh);
            if (!gh || id == 0 || !gh->hasMacro(id)) return luaReturnNil(L);
            const MacroTarget target = resolveMacroTarget(*gh, id);
            if (target.kind != MacroTarget::Kind::Spell) return luaReturnNil(L);
            lua_pushstring(L, gh->getSpellName(target.id).c_str());
            lua_pushstring(L, gh->getSpellRank(target.id).c_str());
            return 2;
        }},
                // GetMacroItem(index) -> name, link; native "/use Linen
                // Bandage" answers the bandage and its link.
                {"GetMacroItem",        [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const uint32_t id = macroIdArg(L, gh);
            if (!gh || id == 0 || !gh->hasMacro(id)) return luaReturnNil(L);
            const MacroTarget target = resolveMacroTarget(*gh, id);
            const auto* info = target.kind == MacroTarget::Kind::Item ? gh->getItemInfo(target.id) : nullptr;
            if (!info || !info->valid) return luaReturnNil(L);
            lua_pushstring(L, info->name.c_str());
            lua_pushstring(L, game::buildItemLink(target.id, info->quality, info->name).c_str());
            return 2;
        }},
                // WoweeActionInfo(slot) -> "spell"|"item"|"macro", the spell
                // id, item id or macro index; for the client's own SetAction.
                {"WoweeActionInfo", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const auto* shown = gh ? game::shownAction(*gh, static_cast<int>(luaL_optnumber(L, 1, 0)) - 1) : nullptr;
            if (!shown) return 0;
            switch (shown->type) {
                case game::ActionBarSlot::SPELL: lua_pushstring(L, "spell"); lua_pushnumber(L, shown->id); return 2;
                case game::ActionBarSlot::ITEM:  lua_pushstring(L, "item");  lua_pushnumber(L, shown->id); return 2;
                case game::ActionBarSlot::MACRO: lua_pushstring(L, "macro"); lua_pushnumber(L, gh->macroIndexForId(shown->id)); return 2;
                default: return 0;
            }
        }},
                // WoweeMacroActionTooltip(slot) -> "spell"|"item", id for a
                // macro button with #showtooltip; nothing otherwise.
                // __WoweeFrameMT:SetAction fills the tooltip from it.
                {"WoweeMacroActionTooltip", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const auto* shown = gh ? game::shownAction(*gh, static_cast<int>(luaL_optnumber(L, 1, 0)) - 1) : nullptr;
            if (!shown || shown->type != game::ActionBarSlot::MACRO) return 0;
            const MacroTarget target = resolveMacroTarget(*gh, shown->id);
            if (!target.showTooltip) return 0;
            if (target.kind == MacroTarget::Kind::Spell) lua_pushstring(L, "spell");
            else if (target.kind == MacroTarget::Kind::Item) lua_pushstring(L, "item");
            else return 0;
            lua_pushnumber(L, target.id);
            return 2;
        }},
                // GetMacroIndexByName(name) -> index, 0 for none.
                {"GetMacroIndexByName", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const char* name = luaL_optstring(L, 1, "");
            uint32_t found = 0;
            if (gh && name && *name) {
                for (uint32_t id : gh->getMacroIds()) {
                    if (gh->getMacroName(id) == name) { found = gh->macroIndexForId(id); break; }
                }
            }
            lua_pushnumber(L, found);
            return 1;
        }},
                // CreateMacro(name, icon, body, perCharacter) → id
                {"CreateMacro",         [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (!gh) return luaReturnNil(L);
            const char* name = luaL_optstring(L, 1, "Macro");
            const char* body = luaL_optstring(L, 3, "");
            // 2.4.3: CreateMacro(name, iconIndex, body, local, perCharacter),
            // the icon an index into GetMacroIconInfo (Blizzard_MacroUI passes
            // MacroPopupFrame.selectedIcon); 3.3.5: (name, icon, body,
            // perCharacter).
            const bool tbc = game::isActiveExpansion("tbc");
            const bool perChar = lua_toboolean(L, tbc ? 5 : 4) != 0;
            std::string icon = luaL_optstring(L, 2, "");
            if (lua_isnumber(L, 2)) icon = macroIconByIndex(L, static_cast<int>(lua_tonumber(L, 2)));
            // Refused once the half holds its 18 (36 account on 3.3.5).
            const uint32_t id = gh->createMacro(perChar, name, icon, body);
            if (id == 0) return luaReturnNil(L);
            // The index it sorts to: native CreateMacro("MZ", ...) answered
            // 31, between APl and UPev.
            lua_pushnumber(L, gh->macroIndexForId(id));
            return 1;
        }},
                // EditMacro(index, name, icon, body) → the index it sorts to
                // afterwards: native renaming APa (19) to ZZa answered 35.
                {"EditMacro",           [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const uint32_t id = macroIdArg(L, gh);
            if (!gh || id == 0) return luaReturnNil(L);
            const char* name = luaL_optstring(L, 2, nullptr);
            const char* body = luaL_optstring(L, 4, nullptr);
            // An icon index as Blizzard_MacroUI passes it, or a path.
            std::string iconByIndex;
            if (lua_isnumber(L, 3)) iconByIndex = macroIconByIndex(L, static_cast<int>(lua_tonumber(L, 3)));
            const char* icon = lua_isnumber(L, 3) ? iconByIndex.c_str() : luaL_optstring(L, 3, nullptr);
            // Each part is optional and nil means "leave it": EditMacro is
            // called with only a name when a macro is renamed, and writing the
            // other two as empty would erase the macro being renamed.
            if (body) gh->setMacroText(id, body);
            gh->setMacroMeta(id, name ? name : gh->getMacroName(id),
                                 icon ? icon : gh->getMacroIcon(id));
            lua_pushnumber(L, gh->macroIndexForId(id));
            return 1;
        }},
                // DeleteMacro(index). The macros after it move up an index
                // because indexes are places in the sorted list; buttons
                // hold identities and keep their macros.
                {"DeleteMacro",         [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const uint32_t id = macroIdArg(L, gh);
            if (id != 0) gh->deleteMacro(id);
            return 0;
        }},
                // Saved as each change is made, so this has nothing to do.
                {"SaveMacros",          [](lua_State* L) -> int { (void)L; return 0; }},
                // PickupMacro(id) - onto the cursor, so it can be dropped on
                // an action slot. The slot type already existed and the packet
                // that sets it already went out for spells and items; a macro
                // was simply never something the cursor could hold.
                {"PickupMacro",         [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const uint32_t id = macroIdArg(L, gh);
            if (!gh || id == 0 || !gh->hasMacro(id)) return 0;
            pickUpAction(L, CursorType::MACRO, id);
            return 0;
        }},
                {"GetTradePlayerItemInfo", lua_GetTradePlayerItemInfo},
                {"GetTradeTargetItemInfo", lua_GetTradeTargetItemInfo},
                {"GetTradePlayerItemLink", lua_GetTradePlayerItemLink},
                {"GetTradeTargetItemLink", lua_GetTradeTargetItemLink},
                {"ClickTradeButton",    lua_ClickTradeButton},
                {"ClickTargetTradeButton", lua_ClickTargetTradeButton},
                {"SetTradeMoney",       lua_SetTradeMoney},
                // CancelTradeAccept() - take an acceptance back without
                // closing the trade. The cancel button does this instead of
                // closing while the player has already accepted, so without it
                // the only way out of an accepted trade was to end it.
                {"CancelTradeAccept", [](lua_State* L) -> int {
            if (auto* gh = getGameHandler(L)) gh->unacceptTrade();
            return 0;
        }},
                {"CloseTrade",          lua_CloseTrade},
                {"BeginTrade",          lua_BeginTrade},
                {"GetBindingKey",       lua_GetBindingKey},
                {"WoweeGetControllerBindingKey", lua_GetControllerBindingKey},
                {"WoweeGetControllerBinding", lua_GetControllerBinding},
                {"WoweeSetBindingSlot", lua_SetBindingSlot},
                {"WoweeResetKeyboardBindings", lua_ResetKeyboardBindings},
                {"WoweeResetControllerBindings", lua_ResetControllerBindings},
                {"GetBindingAction",    lua_GetBindingAction},
                {"GetNumBindings",      lua_GetNumBindings},
                {"GetBinding",          lua_GetBinding},
                {"SetBinding",          lua_SetBinding},
                {"GetBindingByKey", [](lua_State* L) -> int {
            seedBindingDefaults(L);
            const auto command = wowee::ui::KeybindingManager::getInstance().commandForKey(luaL_checkstring(L, 1));
            if (command.empty()) lua_pushnil(L);
            else lua_pushstring(L, command.c_str());
            return 1;
        }},
                // GetQuestGreenRange() - how many levels below the player a
                // thing can be before it turns grey.
                //
                // This was left unbound on the grounds that a guessed constant
                // would mis-colour every quest and that nothing live called it.
                // The second half was wrong: TargetFrame_CheckLevel reaches it
                // through GetQuestDifficultyColor for any target the player can
                // attack, so it raised before levelText:Show() and an enemy's
                // level never appeared. A friendly target took the other branch
                // and showed its level, which is why it looked like enemies
                // specifically.
                //
                // TBC answers build 8606's measured table (questGreenRange).
                {"GetQuestGreenRange", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int lvl = gh ? static_cast<int>(gh->getPlayerLevel()) : 1;
            lua_pushnumber(L, questGreenRange(tbcInterface(L), lvl));
            return 1;
        }},
                // Two latent raises on the escape-key path, bound as no-ops
                // because doing nothing is the right behaviour rather than a
                // placeholder for it. SetUIVisibility is reached only from
                // ToggleGameMenu's first branch, which runs when UIParent is
                // hidden - out of the way until someone hides the interface,
                // and then on the only path that brings it back.
                // ConsoleAddMessage is the sink for FrameXML's debug print.
                {"SetUIVisibility",   [](lua_State* L) -> int { (void)L; return 0; }},
                {"ConsoleAddMessage", [](lua_State* L) -> int { (void)L; return 0; }},
                // GetCurrentBindingSet() - which set SaveBindings should write.
                //
                // 1 is account-wide and 2 is per character. This client keeps
                // one set of bindings, so it is always the first. It was
                // missing rather than stubbed, and interfaceoptionspanels.lua
                // calls SaveBindings(GetCurrentBindingSet()) from three
                // separate option handlers - so changing an option would have
                // thrown rather than saved.
                {"GetCurrentBindingSet", [](lua_State* L) -> int {
            const std::string v = storedCVarValue("wowee_bindingset", "1");
            lua_pushnumber(L, v == "2" ? 2 : 1);
            return 1;
        }},
                {"SaveBindings",        lua_SaveBindings},
                {"LoadBindings",        lua_LoadBindings},
                {"RunBinding",          lua_RunBinding},
                {"SetOverrideBindingClick", lua_SetOverrideBindingClick},
                {"SetOverrideBinding",      lua_SetOverrideBinding},
                {"SetOverrideBindingSpell", lua_SetOverrideBindingSpell},
                {"SetOverrideBindingMacro", lua_SetOverrideBindingMacro},
                {"SetOverrideBindingItem",  lua_SetOverrideBindingItem},
                {"ClearOverrideBindings", lua_ClearOverrideBindings},
                // Paging lives here, and the getter with it. Both were
                // defined twice - this pair against __WoweeActionBarPage and a
                // bootstrap pair against a local - so whichever won,
                // ChangeActionBarPage could move one number while
                // GetActionBarPage read the other. Removing only the getter
                // made that worse rather than better, which is what happened
                // an hour ago.
                {"GetActionBarPage", [](lua_State* L) -> int {
            lua_getglobal(L, "__WoweeActionBarPage");
            if (lua_isnil(L, -1)) { lua_pop(L, 1); lua_pushnumber(L, 1); }
            return 1;
        }},
                {"ChangeActionBarPage", [](lua_State* L) -> int {
            int page = static_cast<int>(luaL_checknumber(L, 1));
            if (page < 1) page = 1;
            if (page > 6) page = 6;
            lua_pushnumber(L, page);
            lua_setglobal(L, "__WoweeActionBarPage");
            // The number keys are handled client-side against a page of their
            // own, so it has to be told or they go on casting page one.
            if (auto* svc = getLuaServices(L); svc && svc->setActionBarPage) {
                svc->setActionBarPage(page);
            }
            // Through the engine, which delivers to both registries.
            //
            // This walked __WoweeEvents by hand and stopped there. That table
            // is where an addon's RegisterEvent lands; FrameXML registers
            // through frame:RegisterEvent, which fills __WoweeFrameEvents - so
            // the six frames that listen for this, including the one in
            // actionbutton.lua that redraws every button, were never told.
            //
            // The page number moved and nothing else did, which is exactly
            // what the arrows beside the bar looked like: they clicked, they
            // played their sound, and the icons stayed where they were.
            lua_getfield(L, LUA_REGISTRYINDEX, "wowee_lua_engine");
            auto* engine = static_cast<LuaEngine*>(lua_touserdata(L, -1));
            lua_pop(L, 1);
            if (engine) engine->fireEvent("ACTIONBAR_PAGE_CHANGED", {});
            return 0;
        }},
                // Two returns: whether there is a pet interface at all, and
                // whether it is a hunter's. PetFrame_SetHappiness reads the
                // second to decide whether to draw the happiness icon, and one
                // return left it nil.
                // A hunter's pet is the one with happiness (power 4): native
                // 2.4.3 answers 1, 1 with a hunter's wolf out and nothing
                // without a pet.
                {"HasPetUI", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (!gh || !gh->hasPet()) return 0;
            auto* pet = resolveUnit(L, "pet");
            lua_pushnumber(L, 1);
            if (pet && pet->getMaxPowerByType(4) != 0) lua_pushnumber(L, 1);
            else lua_pushnil(L);
            return 2;
        }},
                // GetPetLoyalty() → the PetLoyalty.dbc name of the pet's
                // loyalty level (UNIT_FIELD_BYTES_1 byte 1, CMaNGOS
                // Pet::GetLoyaltyLevel): "(Loyalty Level 1) Rebellious" on
                // 2.4.3. Nothing without a hunter's pet.
                {"GetPetLoyalty", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            auto* pet = resolveUnit(L, "pet");
            if (!gh || !pet || pet->getMaxPowerByType(4) == 0) return 0;
            const uint16_t bytes1 = game::fieldIndex(game::UF::UNIT_FIELD_BYTES_1);
            if (bytes1 == 0xFFFF) return 0;
            const uint32_t level = (pet->getField(bytes1) >> 8) & 0xFF;
            const auto* layouts = pipeline::getActiveDBCLayout();
            const auto* layout = layouts ? layouts->getLayout("PetLoyalty") : nullptr;
            auto* assets = gh->services().assetManager;
            if (!layout || !assets) {
                LOG_ERROR("GetPetLoyalty: dbc_layouts.json lacks PetLoyalty");
                return 0;
            }
            const auto dbc = assets->loadDBC("PetLoyalty.dbc");
            const int32_t row = dbc && dbc->isLoaded() ? dbc->findRecordById(level) : -1;
            if (row < 0) return 0;
            lua_pushstring(L, dbc->getString(static_cast<uint32_t>(row), layout->field("Name")).c_str());
            return 1;
        }},
                // GetPetTrainingPoints() → totalPoints, spent from UNIT_TRAINING_POINTS.
                // High 16 bits = total, low 16 bits = spent (CMaNGOS Pet::GetDispTP
                // writes -(TP+1) for TP >= 0 and -TP for TP < 0, so (value >> 16) -
                // (value & 0xFFFF) == TP in all cases; 0, 0 without a pet).
                {"GetPetTrainingPoints", [](lua_State* L) -> int {
            auto* pet = resolveUnit(L, "pet");
            const uint16_t field = game::fieldIndex(game::UF::UNIT_TRAINING_POINTS);
            const uint32_t value = pet && field != 0xFFFF ? pet->getField(field) : 0;
            lua_pushnumber(L, value >> 16);
            lua_pushnumber(L, value & 0xFFFF);
            return 2;
        }},
                // 1 while a pet is out (native 2.4.3 with a hunter's wolf),
                // nothing without one.
                {"GetPetActionsUsable", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (!gh || !gh->hasPet()) return 0;
            lua_pushnumber(L, 1);
            return 1;
        }},
                // Happiness, for a hunter's pet only. Nil is the honest answer
                // for everyone else and the one PetFrame_SetHappiness guards
                // for - the fallback answering with an object instead made
                // that guard pass and the branch index nothing.
                // GetMirrorTimerInfo(index) → timer, value, maxvalue, scale,
                // paused, label.
                //
                // "UNKNOWN" for a timer that is not running, which is exactly
                // what mirrortimer.lua tests for before using the rest. Absent,
                // the fallback answered with an object, that test passed, and
                // the next line divided a nil by a thousand.
                {"GetMirrorTimerInfo", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int index = static_cast<int>(luaL_optnumber(L, 1, 1)) - 1;
            // Shared with the packet handler that fires MIRROR_TIMER_START, so
            // that polling and event agree on the name MirrorTimer_Hide will
            // later be asked to match.
            const auto& kNames  = game::GameHandler::kMirrorTimerNames;
            const auto& kLabels = game::GameHandler::kMirrorTimerLabels;
            if (!gh || index < 0 || index > 2 || !gh->getMirrorTimer(index).active) {
                lua_pushstring(L, "UNKNOWN");
                lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0);
                lua_pushnumber(L, 0);
                lua_pushstring(L, "");
                return 6;
            }
            const auto& t = gh->getMirrorTimer(index);
            lua_pushstring(L, kNames[index]);
            lua_pushnumber(L, t.value);
            lua_pushnumber(L, t.maxValue);
            lua_pushnumber(L, t.scale);
            lua_pushnumber(L, t.paused ? 1 : 0);
            lua_pushstring(L, kLabels[index]);
            return 6;
        }},
                // GetPetHappiness() → happiness 1..3, and what it does to damage
                //
                // Both values, and that is not tidiness. The pet frame does
                // format(PET_DAMAGE_PERCENTAGE, damagePercentage) - "Causes %d%%
                // of normal damage" - the moment happiness is anything at all,
                // and %d against nil raises. Answering nil, as this did before,
                // was safe only because the line above it returns early on a nil
                // happiness; filling in the first value alone would have turned
                // a blank indicator into a dead pet frame for every hunter.
                //
                // Happiness is a power like mana, at index 4, running from
                // nothing to its own maximum. The three faces are the thirds of
                // that range, and the damage each is worth - three quarters,
                // normal, a quarter more - is the game's, not a guess.
                {"GetPetHappiness", [](lua_State* L) -> int {
            // No pet, or one without happiness (a warlock's felhunter):
            // native 2.4.3 answers nil, 100, 0.
            auto* pet = resolveUnit(L, "pet");
            const uint32_t maxHappiness = pet ? pet->getMaxPowerByType(4) : 0;
            if (maxHappiness == 0) {
                lua_pushnil(L); lua_pushnumber(L, 100); lua_pushnumber(L, 0);
                return 3;
            }

            const uint32_t happiness = pet->getPowerByType(4);
            const int level = (happiness * 3 >= maxHappiness * 2) ? 3
                            : (happiness * 3 >= maxHappiness)     ? 2
                                                                  : 1;
            // Damage and loyalty rate per face, as native 2.4.3 answers them
            // for a wolf fed from unhappy to happy: 1, 75, -10; 2, 100, 5;
            // 3, 125, 20. The pet frame's tooltip says losing or gaining
            // loyalty by the sign of the third.
            static const int kDamagePercent[4] = {0, 75, 100, 125};
            static const int kLoyaltyRate[4] = {0, -10, 5, 20};
            lua_pushnumber(L, level);
            lua_pushnumber(L, kDamagePercent[level]);
            lua_pushnumber(L, kLoyaltyRate[level]);
            return 3;
        }},
                // Only referenced from commented-out code in 3.3.5, but a
                // temporary pet's timer is nil when there is no timer.
                {"GetPetTimeRemaining", [](lua_State* L) -> int {
            lua_pushnil(L);
            return 1;
        }},
                // GetPetActionSlotUsable(index) → whether the pet can use it
                //
                // Read as "if usable then draw it normally, else grey it out",
                // so an absent answer greyed out every ability the pet has. A
                // slot holding a spell is usable; an empty one has nothing to
                // grey.
                // CancelItemTempEnchantment(hand) - drop a weapon imbue
                //
                // The interface counts the hands from one and the request from
                // zero, which is the only thing between them.
                {"CancelItemTempEnchantment", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int hand = static_cast<int>(luaL_optnumber(L, 1, 0));
            if (gh && (hand == 1 || hand == 2)) {
                gh->cancelTempEnchantment(static_cast<uint8_t>(hand - 1));
            }
            return 0;
        }},
                {"GetPetActionSlotUsable", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int index = static_cast<int>(luaL_optnumber(L, 1, 0));
            if (!gh || !gh->hasPet() || index < 1 || index > game::GameHandler::PET_ACTION_BAR_SLOTS) {
                lua_pushboolean(L, 0);
                return 1;
            }
            const uint32_t packed = gh->getPetActionSlot(index - 1);
            lua_pushboolean(L, packed != 0 ? 1 : 0);
            return 1;
        }},
                // GetPetActionInfo(index) →
                //   name, subtext, texture, isToken, isActive,
                //   autoCastAllowed, autoCastEnabled
                //
                // Six of the pet bar's ten slots are not spells at all: three
                // commands (attack, follow, stay) and three stances. This read
                // every slot as a spell, so the low 24 bits - which for those
                // six are 0, 1 or 2 - were looked up as spell ids. The bar's
                // command buttons were drawn with whatever Spell.dbc has at
                // those numbers, or with "Unknown" and a question mark.
                //
                // isToken says which kind a slot is, and PetActionBar_Update
                // reads name and texture as *global names* when it is set:
                //     petActionIcon:SetTexture(_G[texture])
                // Answering a flat false meant even a correct path would have
                // been used the wrong way round.
                {"GetPetActionInfo", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            int index = static_cast<int>(luaL_checknumber(L, 1));
            if (!gh || index < 1 || index > game::GameHandler::PET_ACTION_BAR_SLOTS) {
                return luaReturnNil(L);
            }
            const uint32_t packed = gh->getPetActionSlot(index - 1);
            if (packed == 0) { return luaReturnNil(L); }

            const uint32_t action = game::pet::petActionId(packed);
            const auto type = game::pet::petActionType(packed);

            const char* tokenName = nullptr;
            const char* tokenTexture = nullptr;
            bool active = false;

            if (type == game::pet::ActionType::Command) {
                switch (action) {
                    case game::pet::kStay:
                        tokenName = "PET_ACTION_WAIT";    tokenTexture = "PET_WAIT_TEXTURE";   break;
                    case game::pet::kFollow:
                        tokenName = "PET_ACTION_FOLLOW";  tokenTexture = "PET_FOLLOW_TEXTURE"; break;
                    case game::pet::kAttack:
                        tokenName = "PET_ACTION_ATTACK";  tokenTexture = "PET_ATTACK_TEXTURE"; break;
                    case game::pet::kAbandon:
                        tokenName = "PET_DISMISS";        tokenTexture = "PET_DISMISS_TEXTURE"; break;
                    default: break;
                }
                // The command the pet is under: native 2.4.3 lights Follow
                // for a felhunter just summoned.
                active = (action == gh->getPetCommand());
            } else if (type == game::pet::ActionType::Reaction) {
                switch (action) {
                    case game::pet::kPassive:
                        tokenName = "PET_MODE_PASSIVE";    tokenTexture = "PET_PASSIVE_TEXTURE";    break;
                    case game::pet::kDefensive:
                        tokenName = "PET_MODE_DEFENSIVE";  tokenTexture = "PET_DEFENSIVE_TEXTURE";  break;
                    case game::pet::kAggressive:
                        tokenName = "PET_MODE_AGGRESSIVE"; tokenTexture = "PET_AGGRESSIVE_TEXTURE"; break;
                    default: break;
                }
                // The stance the pet is in is the one drawn as chosen.
                active = (action == gh->getPetReact());
            }
            // 2.4.3's shape (pair 21, felhunter): name, subtext, texture,
            // isToken, isActive, autoCastAllowed, autoCastEnabled, flags as
            // 1/nil; a token's subtext is nil, a spell's its rank.
            const auto flag = [L](bool on) { if (on) lua_pushnumber(L, 1); else lua_pushnil(L); };

            if (tokenName) {
                lua_pushstring(L, tokenName);       // 1: name - a global's name
                lua_pushnil(L);                     // 2: subtext
                lua_pushstring(L, tokenTexture);    // 3: texture - likewise
                flag(true);                         // 4: isToken
                flag(active);                       // 5: isActive
                // A command or a stance has nothing to cast automatically, and
                // saying otherwise puts the autocast ring on all six of them.
                flag(false);                        // 6: autoCastAllowed
                flag(false);                        // 7: autoCastEnabled
                return 7;
            }

            const uint32_t spellId = action;
            if (spellId == 0) { return luaReturnNil(L); }
            const std::string& name = gh->getSpellName(spellId);
            const std::string iconPath = gh->getSpellIconPath(spellId);
            lua_pushstring(L, name.empty() ? "Unknown" : name.c_str());
            lua_pushstring(L, gh->getSpellRank(spellId).c_str());      // "Rank 1"
            lua_pushstring(L, iconPath.empty() ? "Interface\\Icons\\INV_Misc_QuestionMark"
                                               : iconPath.c_str());
            flag(false);                                                // isToken
            // A pet spell is never "active" - that is the checked ring, which
            // belongs to the stance. Reading the autocast bits as active put
            // it on every spell the pet knows.
            flag(false);                                                // isActive
            const bool canAutocast = (type == game::pet::ActionType::Enabled ||
                                      type == game::pet::ActionType::Disabled);
            flag(canAutocast);                                          // autoCastAllowed
            flag(canAutocast && gh->isPetSpellAutocast(spellId));       // autoCastEnabled
            return 7;
        }},
                {"WoweePetActionSpellId", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int index = static_cast<int>(luaL_optnumber(L, 1, 0));
            if (!gh || index < 1 || index > game::GameHandler::PET_ACTION_BAR_SLOTS) {
                lua_pushnumber(L, 0);
                return 1;
            }
            const uint32_t packed = gh->getPetActionSlot(index - 1);
            const uint32_t spellId = game::pet::isPetSpellAction(packed)
                ? game::pet::petActionId(packed) : 0u;
            lua_pushnumber(L, spellId);
            return 1;
        }},
                // IsPetAttackAction(index) - which slot is the attack command,
                // so the bar can flash it while the pet is on a target.
                {"IsPetAttackAction", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int index = static_cast<int>(luaL_optnumber(L, 1, 0));
            bool isAttack = false;
            if (gh && index >= 1 && index <= game::GameHandler::PET_ACTION_BAR_SLOTS) {
                const uint32_t packed = gh->getPetActionSlot(index - 1);
                isAttack = game::pet::petActionType(packed) == game::pet::ActionType::Command &&
                           game::pet::petActionId(packed) == game::pet::kAttack;
            }
            lua_pushboolean(L, isAttack ? 1 : 0);
            return 1;
        }},
                // GetPetActionCooldown(index) → start, duration, enable
                //
                // A flat zero, so a pet ability on cooldown was drawn ready and
                // clickable. The client has tracked pet spell cooldowns all
                // along - they arrive on the same packet as the player's.
                //
                // Only the remaining time is kept, not the original duration,
                // so the cooldown is reported as starting now and lasting what
                // is left. That draws exactly the remaining arc, which is the
                // part anyone reads, and it is what the item cooldown beside it
                // does for the same reason.
                {"GetPetActionCooldown", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int index = static_cast<int>(luaL_optnumber(L, 1, 0));
            // 0, 0, 0 with no pet at all (native 2.4.3).
            if (!gh || index < 1 || index > game::GameHandler::PET_ACTION_BAR_SLOTS) {
                lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0);
                return 3;
            }
            const uint32_t packed = gh->getPetActionSlot(index - 1);
            // A command or a stance has no cooldown of its own.
            const uint32_t petSpellId = game::pet::isPetSpellAction(packed)
                ? game::pet::petActionId(packed) : 0u;
            const float remaining = petSpellId != 0 ? gh->getPetSpellCooldown(petSpellId) : 0.0f;
            // Ready: enable 1 for a spell (an imp's Firebolt), 0 for a
            // command or a stance, as native 2.4.3 answers.
            if (remaining <= 0.01f) {
                lua_pushnumber(L, 0); lua_pushnumber(L, 0);
                lua_pushnumber(L, game::pet::isPetSpellAction(packed) ? 1 : 0);
                return 3;
            }
            double total = gh->getPetSpellCooldownTotal(petSpellId);
            if (total < remaining) total = remaining;
            lua_pushnumber(L, luaGetTimeNow() - (total - remaining));
            lua_pushnumber(L, total);
            lua_pushnumber(L, 1);
            return 3;
        }},
                {"PetAttack", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (gh && gh->hasPet() && gh->hasTarget())
                gh->sendPetAction(
                    game::pet::packPetAction(game::pet::ActionType::Command, game::pet::kAttack),
                    gh->getTargetGuid());
            return 0;
        }},
                {"PetFollow", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (gh && gh->hasPet())
                gh->sendPetAction(
                    game::pet::packPetAction(game::pet::ActionType::Command, game::pet::kFollow), 0);
            return 0;
        }},
                {"PetWait", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (gh && gh->hasPet())
                gh->sendPetAction(
                    game::pet::packPetAction(game::pet::ActionType::Command, game::pet::kStay), 0);
            return 0;
        }},
                {"PetPassiveMode", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (gh && gh->hasPet())
                gh->sendPetAction(
                    game::pet::packPetAction(game::pet::ActionType::Reaction, game::pet::kPassive), 0);
            return 0;
        }},
                {"CastPetAction", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            int index = static_cast<int>(luaL_checknumber(L, 1));
            if (!gh || !gh->hasPet() || index < 1 || index > game::GameHandler::PET_ACTION_BAR_SLOTS) return 0;
            const uint32_t packed = gh->getPetActionSlot(index - 1);
            if (packed == 0) return 0;
            const auto type = game::pet::petActionType(packed);
            const uint32_t actionId = game::pet::petActionId(packed);
            // Stay (COMMAND_STAY=0) and Passive (REACT_PASSIVE=0) have low 24 bits == 0
            // and must still be sent; only empty spell slots (non-command/reaction with id 0) skip.
            if (game::pet::isPetSpellAction(packed) && actionId == 0) return 0;

            uint64_t target = 0;
            const bool needsTarget = game::pet::isPetSpellAction(packed) ||
                (type == game::pet::ActionType::Command && actionId == game::pet::kAttack);
            if (needsTarget) {
                // CastPetAction(action, unit) - the unit is what a click-cast
                // binding on a unit frame passes, and dropping it sent the pet
                // at whatever was targeted instead of what was clicked.
                if (const char* uid = luaL_optstring(L, 2, nullptr)) {
                    std::string u(uid);
                    toLowerInPlace(u);
                    target = resolveUnitGuid(gh, u);
                }
                if (target == 0)
                    target = gh->hasTarget() ? gh->getTargetGuid() : gh->getPetGuid();
            }
            gh->sendPetAction(packed, target);
            return 0;
        }},
                {"TogglePetAutocast", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            int index = static_cast<int>(luaL_checknumber(L, 1));
            if (!gh || !gh->hasPet() || index < 1 || index > game::GameHandler::PET_ACTION_BAR_SLOTS) return 0;
            const uint32_t packed = gh->getPetActionSlot(index - 1);
            if (!game::pet::isPetSpellAction(packed)) return 0;
            const auto type = game::pet::petActionType(packed);
            if (type != game::pet::ActionType::Enabled && type != game::pet::ActionType::Disabled) return 0;
            const uint32_t spellId = game::pet::petActionId(packed);
            if (spellId != 0) gh->togglePetSpellAutocast(spellId);
            return 0;
        }},
                {"PetStopAttack", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (gh && gh->hasPet()) gh->petStopAttack();
            return 0;
        }},
                {"PetDismiss", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (gh && gh->hasPet())
                gh->sendPetAction(
                    game::pet::packPetAction(game::pet::ActionType::Command, game::pet::kAbandon), 0);
            return 0;
        }},
                // The other end of the ABANDON_PET dialog, which the pet's
                // unit menu opens. It was built, shown, and raised on accept.
                //
                // Not the same as PetDismiss above, which sends the pet away
                // and can be undone by calling it back. Abandon is permanent,
                // which is what the dialog in front of it is for.
                {"PetAbandon", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (gh && gh->hasPet()) gh->abandonPet();
            return 0;
        }},
                // CONFIRM_BUY_STABLE_SLOT's accept, from the stable panel.
                {"BuyStableSlot", [](lua_State* L) -> int {
            if (auto* gh = getGameHandler(L)) gh->buyStableSlot();
            return 0;
        }},
                // VOTE_BOOT_PLAYER's two buttons. The dialog is shown from
                // LFG_BOOT_PROPOSAL_UPDATE, which is fired, so a vote to kick
                // reached the player and neither answer went anywhere.
                //
                // Both buttons matter here: declining is a vote, not a
                // dismissal, and OnCancel sends it.
                {"SetLFGBootVote", [](lua_State* L) -> int {
            if (auto* gh = getGameHandler(L)) gh->lfgSetBootVote(lua_toboolean(L, 1) != 0);
            return 0;
        }},
                // CONFIRM_RESET_INSTANCES' accept, from /resetinstances and
                // the party menu.
                {"ResetInstances", [](lua_State* L) -> int {
            if (auto* gh = getGameHandler(L)) gh->resetInstances();
            return 0;
        }},
                // UNLEARN_SKILL's accept, from the skill panel's unlearn
                // button. The skill line comes from the button's own row.
                // Takes a skill id here, and FrameXML's one caller passes it
                // whatever it was handed as the popup's data - which in the
                // original client is the skills tab's row index, not an id.
                // Nothing shows UNLEARN_SKILL in this build (GetSkillLineInfo
                // answers isAbandonable false, so no row offers to unlearn), so
                // the two never meet; if the popup is ever raised, whichever
                // side passes the index has to resolve it through skillRows()
                // first, or this unlearns the skill whose id happens to equal a
                // row number.
                {"AbandonSkill", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const auto skillId = static_cast<uint32_t>(luaL_optnumber(L, 1, 0));
            if (gh && skillId) gh->unlearnSkill(skillId);
            return 0;
        }},
                {"IsPetAttackActive", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (!gh || gh->getPetCommand() != game::pet::kAttack) {
                lua_pushnil(L);
                return 1;
            }
            // PetActionButton_OnClick passes this:GetID(); only the Attack slot
            // should answer active so clicking Follow/Stay/stances never routes
            // to PetStopAttack().
            if (lua_gettop(L) >= 1 && !lua_isnil(L, 1)) {
                const int index = static_cast<int>(luaL_optnumber(L, 1, 0));
                if (index < 1 || index > game::GameHandler::PET_ACTION_BAR_SLOTS) {
                    lua_pushnil(L);
                    return 1;
                }
                const uint32_t packed = gh->getPetActionSlot(index - 1);
                const bool isAttackSlot =
                    game::pet::petActionType(packed) == game::pet::ActionType::Command &&
                    game::pet::petActionId(packed) == game::pet::kAttack;
                if (!isAttackSlot) {
                    lua_pushnil(L);
                    return 1;
                }
            }
            lua_pushnumber(L, 1);
            return 1;
        }},
                {"PetDefensiveMode", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (gh && gh->hasPet())
                gh->sendPetAction(
                    game::pet::packPetAction(game::pet::ActionType::Reaction, game::pet::kDefensive), 0);
            return 0;
        }},
                // The third stance, which was a no-op in another file entirely
                // - so a hunter could set a pet passive or defensive and not
                // aggressive, and the button for it did nothing.
                {"PetAggressiveMode", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (gh && gh->hasPet())
                gh->sendPetAction(
                    game::pet::packPetAction(game::pet::ActionType::Reaction, game::pet::kAggressive), 0);
            return 0;
        }},
    };
    for (const auto& [name, func] : api) {
        lua_pushcfunction(L, func);
        lua_setglobal(L, name);
    }

    // The cursor, for this client's own windows to read. They keep a separate
    // held item of their own, so a drag out of a handed-over bag into a window
    // that is still this client's - the bank, the mailbox, a trade - had no way
    // to know anything had been picked up.
    //
    // Registered here rather than mirrored into the ui layer because this is
    // where the cursor is. Re-registered on every reload, which rebuilds the
    // Lua state, so the captured pointer is the current one; LuaEngine::shutdown
    // takes the bridge back down again.
    ui::frameXmlSetCursorBridge(
        [](uint8_t& bag, uint8_t& slot) { return cursorWireSlot(bag, slot); },
        [L]() { clearCursorItem(L); });
    ui::frameXmlSetCursorPicture([L]() { return cursorPicture(L); });
}

} // namespace wowee::addons
