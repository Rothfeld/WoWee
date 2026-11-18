#pragma once

#include <cstdint>
#include <string>

namespace wowee {
namespace game { class GameHandler; }
namespace addons {

/// What a macro on the action bar stands for: the spell or item of its
/// "#showtooltip <name>" line, else of its first /cast, /use or
/// /castsequence line whose conditions hold. Native 2.4.3 (pair 2,
/// level 40 rogue):
///
///     "/cast Sinister Strike"                 Spell 26862
///     "#showtooltip\n/cast Eviscerate"        Spell, tooltip shown
///     "/cast [stealth] Ambush; Sinister Strike"  Sinister Strike out of stealth
///     "/use Linen Bandage"                    Item 1251
///     "/castsequence Sinister Strike, Eviscerate"  Sinister Strike
///     "#showtooltip Kick\n/say x"             Spell Kick, tooltip shown
///     "/say hi"                               None
///     "/cast Sinister Strike(Rank 1)", "/use 13" (empty trinket slot)  Unresolved
struct MacroTarget {
    enum class Kind { None, Unresolved, Spell, Item };
    Kind kind = Kind::None;
    uint32_t id = 0;
    /// A #showtooltip line: the button's tooltip is the spell's or item's.
    bool showTooltip = false;
};

MacroTarget resolveMacroTarget(game::GameHandler& gh, uint32_t macroId);

/// The macro's icon: its own, or for the question mark (icon 1) the icon of
/// its spell or item. Native "/cast Sinister Strike" with the question mark
/// shows Spell_Shadow_RitualOfSacrifice; "/say hi" keeps the question mark.
std::string macroTexture(game::GameHandler& gh, uint32_t macroId);

}  // namespace addons
}  // namespace wowee
