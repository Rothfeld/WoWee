#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace wowee::game {

/// Spell.dbc AttributesEx3 0x4, SPELL_ATTR_EX3_NO_CASTING_BAR_TEXT in CMaNGOS
/// TBC SpellDefines.h. Among spells with a cast time only Opening 6477,
/// Opening - No Text 22810 and Spawn Magic Sucker Device Bunny 38707 carry it.
inline constexpr uint32_t kSpellAttrEx3NoCastingBarText = 0x4;

/// The words on a cast bar: UnitCastingInfo's third value, which
/// CastingBarFrame.lua puts on the bar.
///
/// objectText is the opening (or closing) text of the game object an
/// OPEN_LOCK cast is aimed at, from its query response; nullopt for any other
/// cast. Original 2.4.3 (pair 8, _experiments/20261119T1330Z.castbar_text):
///
/// | Object (text)                    | Spell                        | Bar           |
/// |----------------------------------|------------------------------|---------------|
/// | Battered Chest 2843 ("")         | Opening 3365                 | Opening       |
/// | Sapphire Signal Fire 185216      | Opening 3365                 | Extinguishing |
/// | Glowcap 182053 ("Collecting")    | Opening - No Text 22810 0x4  | Collecting    |
/// | Torch 97701 ("")                 | Opening 6477, 0x4            | (empty)       |
/// | Rizzle's Guarded Plans 179888    | Opening 6477, 0x4            | Examining     |
///
/// The last two are pair 0, _experiments/20261117T171500Z.pair0_go_ghost_pet.
/// UnitCastingInfo's first value stays the spell name in every case.
inline std::string castBarText(const std::string& spellName, uint32_t attributesEx3,
                               const std::optional<std::string>& objectText) {
    if (objectText && !objectText->empty()) return *objectText;
    if (attributesEx3 & kSpellAttrEx3NoCastingBarText) return {};
    return spellName;
}

} // namespace wowee::game
