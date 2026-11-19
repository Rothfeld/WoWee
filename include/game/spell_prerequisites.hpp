#pragma once
#include <cstdint>
namespace wowee::game {
class GameHandler;
struct ActionBarSlot;
/// FormRefused: the player's form is in the spell's StancesNot. Native 2.4.3
/// refuses Flash Heal in Shadowform itself: UNIT_SPELLCAST_FAILED, no
/// UNIT_SPELLCAST_SENT, no error line.
enum class SpellPrerequisite { Ready, Unknown, Passive, NoPower, NoComboPoints, Stealth, Combat, Dead, FormRefused };
SpellPrerequisite spellPrerequisite(GameHandler& owner, uint32_t spellId, uint64_t target);

/// IsUsableAction / IsUsableSpell: whether the spell could be cast now, and
/// when it cannot only for want of power, notEnoughMana. Every other unmet
/// requirement answers (false, false) - native 2.4.3: Ambush out of stealth
/// with 30 of its 60 energy is (nil, nil), Sinister Strike at 30/45 (nil, 1).
/// A cooldown is not a requirement: the sweep shows it.
struct SpellUsability {
    bool usable = false;
    bool noPower = false;
    bool operator==(const SpellUsability&) const = default;
};
SpellUsability spellUsability(GameHandler& owner, uint32_t spellId);

/// An item action's count: carried in bags plus equipped. Native 2.4.3 shows
/// the equipped Worn Dagger on the bar as 1.
uint32_t actionItemCount(GameHandler& owner, uint32_t itemId);

/// GetActionCount of a spell: the carried count of its first reagent, 0 for
/// none (Vanish: Flash Powder).
uint32_t spellReagentCount(GameHandler& owner, uint32_t spellId);

/// IsConsumableAction: a spell with a reagent (Vanish, not Blind); an item
/// its use spends - a negative charge count (potion, food, bandage) - or
/// that ranged attacks spend (InventoryType 24 ammo, 25 thrown). Native
/// 2.4.3 answers 1 for Light Throwing Knife and Rough Arrow, nil for
/// Hearthstone, Mining Pick and Worn Dagger.
bool isConsumableAction(GameHandler& owner, const ActionBarSlot& slot);

/// Whether the global cooldown covers this spell: StartRecoveryCategory 133.
bool onGlobalCooldown(GameHandler& owner, uint32_t spellId);

/// Bar slot `slot` (0-based) as the interface sees it: nullptr when it is out
/// of range, empty, or holds a spell the player does not know. Native 2.4.3
/// with Sinister Strike Rank 1 (1752) stored on button 2 of a character that
/// knows only Rank 10: HasAction nil, GetActionInfo nothing, GetActionTexture
/// nil, IsUsableAction nil - an empty button.
const ActionBarSlot* shownAction(GameHandler& owner, int slot);

/// The same answer for an action bar slot: a spell by spellUsability, an item
/// by being carried (equipped counts) and by its use spell.
SpellUsability actionUsability(GameHandler& owner, const ActionBarSlot& slot);
}
