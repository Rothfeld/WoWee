#pragma once

#include <cstdint>

namespace wowee::game {

// Spell.dbc Attributes / AttributesEx2 bits governing shapeshift form rules.
constexpr uint32_t kSpellAttrNotShapeshift           = 0x00010000u;
constexpr uint32_t kSpellAttrEx2AllowWhileNotShifted = 0x00080000u;

// SpellShapeshiftForm.dbc Flags bits (TBC 2.4.3).
//   0x0001: acts as no form for SPELL_ATTR_NOT_SHAPESHIFT
//           (Tree of Life 2, Warrior stances 17-19, Shadowform 28, Stealth 30, Moonkin 31)
//   0x0002: not toggleable / cannot be cancelled (Warrior Battle/Defensive/Berserker Stance)
//   0x0100: cannot auto-unshift (Spirit of Redemption 32)
constexpr uint32_t kShapeshiftFlagActsAsNoForm  = 0x0001u;
constexpr uint32_t kShapeshiftFlagNotToggleable = 0x0002u;
constexpr uint32_t kShapeshiftFlagNoAutoUnshift = 0x0100u;

struct ShapeshiftCastCheck {
    uint8_t  form = 0;                   ///< Active shapeshift form ID (0 = none)
    uint32_t formFlags = 0;              ///< SpellShapeshiftForm.dbc Flags for `form`
    bool     formAuraCancellable = true; ///< Player holds no blocking debuff / non-cancellable aura for `form`
    bool     autoUnshiftEnabled = true;  ///< CVar "autoUnshift" != "0" (default "1" in TBC 2.4.3)
    uint32_t spellAttr = 0;              ///< Spell.dbc Attributes
    uint32_t spellAttrEx2 = 0;           ///< Spell.dbc AttributesEx2
    uint32_t spellStances = 0;           ///< Spell.dbc Stances mask
    uint32_t spellStancesNot = 0;        ///< Spell.dbc StancesNot mask
};

enum class ShapeshiftCastResult : uint8_t {
    AllowedInForm,        ///< Spell can be cast directly in the current form (no unshift needed)
    AutoUnshiftAndCast,   ///< Blocked in current form, castable unshifted, form is cancellable, autoUnshift=1
    RefusedNotShapeshift, ///< Blocked by StancesNot or NOT_SHAPESHIFT (autoUnshift=0 or non-cancellable form)
    RefusedOnlyShapeshift,///< Requires a specific stance/form the player is not in (unshifting does not help)
};

constexpr ShapeshiftCastResult evaluateShapeshiftCast(const ShapeshiftCastCheck& c) {
    const uint32_t formBit = c.form != 0 ? (1u << (c.form - 1)) : 0u;
    const bool canCastUnshifted =
        (c.spellStances == 0) || ((c.spellAttrEx2 & kSpellAttrEx2AllowWhileNotShifted) != 0);
    const bool formCanAutoUnshift =
        c.autoUnshiftEnabled &&
        c.form != 0 &&
        c.formAuraCancellable &&
        ((c.formFlags & (kShapeshiftFlagNotToggleable | kShapeshiftFlagNoAutoUnshift)) == 0);

    // 1. Explicitly forbidden in this form (StancesNot):
    //    e.g. Priest Holy spells in Shadowform (28), Healing Touch/Rejuvenation in Moonkin (31),
    //    Wrath/Starfire/Cat Form/Healing Touch in Tree of Life (2).
    if ((formBit & c.spellStancesNot) != 0) {
        return (canCastUnshifted && formCanAutoUnshift)
            ? ShapeshiftCastResult::AutoUnshiftAndCast
            : ShapeshiftCastResult::RefusedNotShapeshift;
    }

    // 2. Explicitly allowed in this form (Stances):
    //    e.g. PW:Shield / Mind Blast in Shadowform, Rejuvenation in Tree of Life,
    //    Wrath / Starfire in Moonkin, Overpower in Battle Stance, Ambush in Stealth.
    if ((formBit & c.spellStances) != 0) {
        return ShapeshiftCastResult::AllowedInForm;
    }

    // 3. Real shapeshift (form != 0 and NOT kShapeshiftFlagActsAsNoForm):
    //    Cat (1), Travel (3), Aquatic (4), Bear (5), Dire Bear (8), Ghost Wolf (16),
    //    Epic Flight (27), Flight (29), Spirit of Redemption (32).
    const bool actsAsShifted = (c.form != 0) && ((c.formFlags & kShapeshiftFlagActsAsNoForm) == 0);
    if (actsAsShifted) {
        if ((c.spellAttr & kSpellAttrNotShapeshift) != 0) {
            if (!canCastUnshifted) return ShapeshiftCastResult::RefusedOnlyShapeshift;
            return formCanAutoUnshift
                ? ShapeshiftCastResult::AutoUnshiftAndCast
                : ShapeshiftCastResult::RefusedNotShapeshift;
        }
        if (!canCastUnshifted) return ShapeshiftCastResult::RefusedOnlyShapeshift;
        if (c.spellStances == 0) return ShapeshiftCastResult::AllowedInForm;
        return formCanAutoUnshift
            ? ShapeshiftCastResult::AutoUnshiftAndCast
            : ShapeshiftCastResult::RefusedOnlyShapeshift;
    }

    // 4. Unshifted (form == 0) or form acts as no form (Shadowform, Moonkin, Tree of Life,
    //    Warrior stances, Stealth) and not in StancesNot:
    return canCastUnshifted
        ? ShapeshiftCastResult::AllowedInForm
        : ShapeshiftCastResult::RefusedOnlyShapeshift;
}

} // namespace wowee::game
