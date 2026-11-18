#pragma once

/**
 * spell_description_format.hpp - the $-token text of Spell.dbc descriptions,
 * as the 2.4.3 client substitutes it.
 *
 *     Improved Gouge 13741: "... by $/1000;S1 sec."        -> "... by 0.5 sec."
 *     Gouge 1776:           "Causes $s1 damage ... for $d"  -> "Causes 10 damage ... for 1 sec"
 *     Eviscerate 26865:     "${$m1+($b1*1)+$AP*0.03}"       -> "251"
 *     Mutilate 1329:        "$5374s1"                       -> spell 5374, effect 1
 *
 * Token forms (TBC Spell.dbc census, tools in
 * _experiments/20261117T181655.755643Z.talent_tooltip_pair2/token_census.py):
 *
 *     $[id]s1 $[id]m1 $[id]M1   effect value, minimum, maximum
 *     $[id]o1                   periodic total over the duration
 *     $[id]t1                   periodic interval, seconds
 *     $[id]d                    duration (SpellDuration.dbc)
 *     $[id]a1                   radius (SpellRadius.dbc), yards
 *     $[id]h $n $u $x1 $i $v    proc chance, charges, stack, chain targets,
 *                               max targets, max target level
 *     $[id]e1 $f1 $b1 $q1 $r1   multiple value, chain amplitude, per combo
 *                               point, misc value, max range
 *     $/N;tok  $*N;tok          a token divided or multiplied by N
 *     ${expr}  ${expr}.N        arithmetic, rounded or with N decimals
 *     $lsingular:plural;        by the number written before it
 *     $gmale:female;            by the player's gender
 *     $z                        home bind location
 *
 * Expressions add $AP $RAP $SP $PL, weapon quantities ($MWS $mwb $MWB $rwb
 * $RWB $HND), $cond/$eq/$gt/$lt/$max/$min, $?s<id>[a][b] and $<name> from
 * SpellDescriptionVariables.dbc.
 *
 * A token this cannot resolve stays in the text as written and is reported
 * through SpellDescriptionEnvironment::unresolved.
 */

#include <cstdint>
#include <functional>
#include <string>
#include <utility>

namespace wowee {
namespace game {

/// The Spell.dbc columns description tokens read, for one spell.
struct SpellDescriptionRecord {
    int32_t basePoints[3]{};
    int32_t dieSides[3]{};
    int32_t baseDice[3]{};
    float dicePerLevel[3]{};
    float realPointsPerLevel[3]{};
    float pointsPerComboPoint[3]{};
    uint32_t amplitudeMs[3]{};
    uint32_t auraType[3]{};
    /// Spell.dbc Effect: what the effect does (2 school damage, 31 weapon
    /// percent damage, ...).
    uint32_t effectType[3]{};
    float multipleValue[3]{};
    uint32_t chainTargets[3]{};
    int32_t miscValue[3]{};
    /// SpellRadius.dbc radius in yards, by EffectRadiusIndex.
    float radius[3]{};
    float damageMultiplier[3]{};
    /// SpellDuration.dbc base duration; -1 is unlimited, 0 none.
    int32_t durationMs = 0;
    uint32_t procChance = 0;
    uint32_t procCharges = 0;
    uint32_t stackAmount = 0;
    uint32_t maxTargets = 0;
    uint32_t maxTargetLevel = 0;
    /// SpellRange.dbc maximum, yards.
    float rangeMax = 0.0f;
    uint32_t baseLevel = 0;
    uint32_t spellLevel = 0;
    uint32_t maxLevel = 0;
    /// SpellDescriptionVariables.dbc row text for $<name>, or null.
    const std::string* declarations = nullptr;
};

struct SpellDescriptionEnvironment {
    /// Spell.dbc row by id, or null when absent.
    std::function<const SpellDescriptionRecord*(uint32_t)> spell;
    /// Player quantity by expression name (AP, RAP, SP, PL, MWS, mwb, MWB,
    /// rwb, RWB, HND). False when the client does not know it.
    std::function<bool(const std::string&, double&)> player;
    /// Whether the player knows a spell, for $?s<id>[then][else].
    std::function<bool(uint32_t)> knowsSpell;
    /// Caster level for EffectRealPointsPerLevel.
    uint32_t casterLevel = 0;
    /// Applications of the aura being described: an aura tooltip states the
    /// stacked amount. Native Sunder Armor 25225 at three stacks reads "Armor
    /// decreased by 1560" for $s1 = 520. Applies to the spell's own $s only.
    uint32_t auraStacks = 1;
    bool female = false;
    std::string homeName;
    /// The player's damage modifiers (SpellModOp 0) for a spell: flat and
    /// percent, from SMSG_SET_FLAT/PCT_SPELL_MODIFIER. Null for none.
    std::function<std::pair<int32_t, int32_t>(uint32_t)> damageModifiers;
    /// Each token left unresolved, with its text.
    std::function<void(const std::string&)> unresolved;
};

/// Substitute every token in a description of spell `spellId`.
std::string formatSpellDescriptionText(uint32_t spellId, const std::string& text,
                                       const SpellDescriptionEnvironment& env);

/// Duration text as $d prints it: "6 sec", "30 min", "2 hrs", "1 day".
std::string formatSpellDescriptionDuration(int32_t durationMs);

/// A number as $/N; and the radius/period tokens print it: integral values
/// without a fraction, others with up to two decimals ("0.5", "1.25").
std::string formatSpellDescriptionNumber(double value);

}  // namespace game
}  // namespace wowee
