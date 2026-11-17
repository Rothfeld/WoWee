#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wowee {
namespace game {

/// One ItemSet.dbc row: the set's name, its pieces in DBC order and the bonus
/// spells with the number of equipped pieces each needs. Assassination Armor
/// (620): five pieces, Haste 37165 at 2, Eviscerate and Envenom Discount
/// 37166 at 4.
struct ItemSetInfo {
    std::string name;
    std::vector<uint32_t> items;
    struct Bonus { uint32_t spellId = 0; uint32_t threshold = 0; };
    std::vector<Bonus> bonuses;
    uint32_t requiredSkill = 0;
    uint32_t requiredSkillRank = 0;
};

} // namespace game
} // namespace wowee
