#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wowee::game {

class GameHandler;

/// One button of the stance bar: a known spell and the form it takes (0 for a
/// spell that is on the bar without being a form, such as a paladin aura).
///
/// FrameXML's stance bar asks three questions about these and they have to
/// agree with each other: how many there are (GetNumShapeshiftForms), what the
/// i-th one looks like (GetShapeshiftFormInfo) and what casting the i-th one
/// does (CastShapeshiftForm). All three read playerShapeshiftForms.
struct ShapeshiftForm {
    uint32_t spellId;   ///< What casting this button casts.
    uint8_t formId;     ///< The shapeshift form field's value while it is active, 0 for none.
};

/// The Spell.dbc columns that decide a spell's place on the stance bar.
struct StanceBarSpell {
    uint32_t spellId = 0;
    uint32_t attrEx2 = 0;        ///< AttributesEx2
    uint8_t shapeshiftForm = 0;  ///< EffectMiscValue of an APPLY_AURA MOD_SHAPESHIFT (36) effect
    uint32_t stanceBarOrder = 0; ///< StanceBarOrder
};

/// Spell.dbc AttributesEx2 0x10 (USE_SHAPESHIFT_BAR) puts a spell on the bar
/// without a form: the eight paladin auras. A MOD_SHAPESHIFT spell is on it
/// unless AttributesEx2 0x2 (NO_SHAPESHIFT_UI) keeps it off.
/// `withHidden` keeps the NO_SHAPESHIFT_UI forms: GetShapeshiftForm counts
/// them (native 2.4.3 answers 1 in Shadowform, whose bar has no button).
bool onStanceBar(uint32_t attrEx2, uint8_t shapeshiftForm, bool withHidden = false);

/// The stance-bar spells among `known`, in bar order: StanceBarOrder, then
/// form, then spell id. Native 2.4.3, everything learned at once: paladin
/// Devotion, Retribution, Concentration, Shadow, Frost, Fire Resistance,
/// Sanctity, Crusader Aura (orders 0-7); warrior Battle, Defensive, Berserker
/// Stance (0, 1, 2; Defensive is spell 71); druid Dire Bear, Aquatic, Cat,
/// Travel, Tree of Life, Moonkin, Swift Flight Form (0-5; Cat is 768; Tree of
/// Life form 2 and Moonkin form 31 share order 4, Tree first). Unlearning one
/// and learning it again puts it back in its place.
std::vector<ShapeshiftForm> stanceBarForms(std::vector<StanceBarSpell> known, bool withHidden = false);

/// The player's stance bar from the known spells' Spell.dbc rows.
std::vector<ShapeshiftForm> playerShapeshiftForms(GameHandler& owner, bool withHidden = false);

/// GetShapeshiftForm: the 1-based place of the active one in
/// playerShapeshiftForms with the hidden forms, 0 in none. Native 2.4.3
/// answers 1 in Stealth, 1 for a paladin under his own Devotion Aura and 1
/// in Shadowform (GetNumShapeshiftForms 0).
int currentShapeshiftFormIndex(GameHandler& owner);

/// Whether a stance-bar button is lit: its form is the player's form, or its
/// spell's own aura is on the player (a paladin aura).
bool stanceBarFormActive(GameHandler& owner, const ShapeshiftForm& form);

/// Whether the spell's form is the one the player is in: native 2.4.3 marks
/// Stealth current (IsCurrentAction 1, checked button) while stealthed.
bool spellFormActive(GameHandler& owner, uint32_t spellId);

/// A spell's icon as the bar and stance bar show it: its ActiveIconID while
/// its own aura is on the player (Stealth: Spell_Nature_Invisibilty,
/// Shadowmeld: Spell_Nature_WispSplode), else its icon.
std::string actionSpellIcon(GameHandler& owner, uint32_t spellId);

}  // namespace wowee::game
