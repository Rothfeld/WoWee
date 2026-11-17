#pragma once
#include <cstdint>
namespace wowee::game {
class GameHandler;
struct ActionBarSlot;
enum class SpellPrerequisite { Ready, Unknown, Passive, NoPower, NoComboPoints, Stealth, Combat, Dead };
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

/// The same answer for an action bar slot: a spell by spellUsability, an item
/// by being carried (equipped counts) and by its use spell.
SpellUsability actionUsability(GameHandler& owner, const ActionBarSlot& slot);
}
