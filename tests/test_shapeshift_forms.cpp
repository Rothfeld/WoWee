// The stance bar: which known spells are on it and in what order, from their
// Spell.dbc rows (AttributesEx2, the MOD_SHAPESHIFT form, StanceBarOrder).
//
// The orders are native 2.4.3 (build 8606) with every spell learned at once:
// GetShapeshiftFormInfo(1..GetNumShapeshiftForms()) on a private pair
// (_experiments/20261119T095500Z.class_ui_b in the workspace).
#include <catch_amalgamated.hpp>

#include <vector>

#include "game/shapeshift_forms.hpp"

using wowee::game::ShapeshiftForm;
using wowee::game::StanceBarSpell;
using wowee::game::stanceBarForms;

namespace {

std::vector<uint32_t> ids(const std::vector<ShapeshiftForm>& forms) {
    std::vector<uint32_t> out;
    for (const auto& f : forms) out.push_back(f.spellId);
    return out;
}

}  // namespace

TEST_CASE("paladin auras are on the bar by StanceBarOrder", "[shapeshift]") {
    // Known ranks as the server sends them (Devotion Aura Rank 8 is 27149),
    // AttributesEx2 0x10 (USE_SHAPESHIFT_BAR), no form.
    const std::vector<StanceBarSpell> known{
        {32223, 0x10, 0, 7}, {20218, 0x10, 0, 6}, {27153, 0x10, 0, 5}, {27152, 0x10, 0, 4},
        {27151, 0x10, 0, 3}, {19746, 0x10, 0, 2}, {27150, 0x10, 0, 1}, {27149, 0x10, 0, 0},
        {25780, 0x0, 0, 0},  // Righteous Fury: a buff, not on the bar
    };
    const auto forms = stanceBarForms(known);
    CHECK(ids(forms) == std::vector<uint32_t>{27149, 27150, 19746, 27151, 27152, 27153, 20218, 32223});
    for (const auto& f : forms) CHECK(f.formId == 0);
}

TEST_CASE("warrior stances follow StanceBarOrder, not spell id", "[shapeshift]") {
    // Defensive Stance is spell 71, yet second.
    const auto forms = stanceBarForms({{71, 0x0, 18, 1}, {2458, 0x0, 19, 2}, {2457, 0x1, 17, 0}});
    CHECK(ids(forms) == std::vector<uint32_t>{2457, 71, 2458});
    CHECK(forms[0].formId == 17);
}

TEST_CASE("druid forms: Bear, Aquatic, Cat, Travel, Moonkin, Flight", "[shapeshift]") {
    const auto forms = stanceBarForms({{768, 0, 1, 2}, {783, 0, 3, 3}, {1066, 0, 4, 1},
                                       {5487, 0, 5, 0}, {24858, 0, 31, 4}, {33943, 0, 29, 5}});
    CHECK(ids(forms) == std::vector<uint32_t>{5487, 1066, 768, 783, 24858, 33943});
}

TEST_CASE("NO_SHAPESHIFT_UI keeps a form off the bar", "[shapeshift]") {
    // Shadowform (15473) and Ghost Wolf (2645) carry AttributesEx2 0x2; Stealth
    // (1784) does not and is the rogue's only button.
    CHECK(stanceBarForms({{15473, 0x2, 28, 0}}).empty());
    CHECK(stanceBarForms({{2645, 0x2, 16, 0}}).empty());
    CHECK(ids(stanceBarForms({{1784, 0x00200000, 30, 0}})) == std::vector<uint32_t>{1784});
    // GetShapeshiftForm still counts it: 1 in Shadowform.
    CHECK(ids(stanceBarForms({{15473, 0x2, 28, 0}}, true)) == std::vector<uint32_t>{15473});
}

TEST_CASE("spells with neither attribute nor form are not on the bar", "[shapeshift]") {
    CHECK(stanceBarForms({{1459, 0, 0, 0}, {588, 0x00080000, 0, 0}}).empty());
}
