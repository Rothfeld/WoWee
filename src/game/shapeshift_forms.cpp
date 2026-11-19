#include "game/shapeshift_forms.hpp"

#include <algorithm>

namespace wowee::game {

bool onStanceBar(uint32_t attrEx2, uint8_t shapeshiftForm, bool withHidden) {
    constexpr uint32_t kNoShapeshiftUi = 0x2;
    constexpr uint32_t kUseShapeshiftBar = 0x10;
    if (attrEx2 & kUseShapeshiftBar) return true;
    return shapeshiftForm != 0 && (withHidden || !(attrEx2 & kNoShapeshiftUi));
}

std::vector<ShapeshiftForm> stanceBarForms(std::vector<StanceBarSpell> known, bool withHidden) {
    std::erase_if(known, [&](const StanceBarSpell& s) { return !onStanceBar(s.attrEx2, s.shapeshiftForm, withHidden); });
    std::sort(known.begin(), known.end(), [](const StanceBarSpell& a, const StanceBarSpell& b) {
        return a.stanceBarOrder != b.stanceBarOrder ? a.stanceBarOrder < b.stanceBarOrder : a.spellId < b.spellId;
    });
    std::vector<ShapeshiftForm> out;
    out.reserve(known.size());
    for (const StanceBarSpell& s : known) out.push_back({.spellId = s.spellId, .formId = s.shapeshiftForm});
    return out;
}

}  // namespace wowee::game
