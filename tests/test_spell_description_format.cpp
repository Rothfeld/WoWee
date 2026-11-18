// Spell.dbc description substitution, checked against text the native 2.4.3
// client printed on pair 2 (GameTooltip lines recorded by the TooltipDump
// addon: _experiments/20261117T181655.755643Z.talent_tooltip_pair2/r5_native.lua).
// Records hold the TBC Spell.dbc values of the spells named.

#include <catch2/catch_amalgamated.hpp>

#include <map>
#include <string>
#include <vector>

#include "game/spell_description_format.hpp"

using namespace wowee::game;

namespace {

struct Fixture {
    std::map<uint32_t, SpellDescriptionRecord> spells;
    std::map<std::string, double> player;
    std::vector<std::string> unresolved;
    SpellDescriptionEnvironment env;

    Fixture() {
        env.spell = [this](uint32_t id) -> const SpellDescriptionRecord* {
            const auto it = spells.find(id);
            return it == spells.end() ? nullptr : &it->second;
        };
        env.player = [this](const std::string& name, double& out) {
            const auto it = player.find(name);
            if (it == player.end()) return false;
            out = it->second;
            return true;
        };
        env.unresolved = [this](const std::string& token) { unresolved.push_back(token); };
        env.casterLevel = 40;
    }
};

SpellDescriptionRecord effect(int32_t base, int32_t die = 1, int32_t dice = 1) {
    SpellDescriptionRecord r;
    r.basePoints[0] = base;
    r.dieSides[0] = die;
    r.baseDice[0] = dice;
    return r;
}

}  // namespace

TEST_CASE("Improved Gouge divides its effect by 1000", "[spell-description]") {
    Fixture f;
    f.spells[13741] = effect(499);
    CHECK(formatSpellDescriptionText(13741,
        "Increases the effect duration of your Gouge ability by $/1000;S1 sec.", f.env) ==
        "Increases the effect duration of your Gouge ability by 0.5 sec.");
    f.spells[13742] = effect(-45001);
    CHECK(formatSpellDescriptionText(13742, "abilities by $/1000;s1 sec.", f.env) ==
          "abilities by 45 sec.");
}

TEST_CASE("Proc chance, charges, duration and cross-spell durations", "[spell-description]") {
    Fixture f;
    SpellDescriptionRecord kick = effect(0);
    kick.procChance = 50;
    f.spells[13754] = kick;
    SpellDescriptionRecord silence;
    silence.durationMs = 2000;
    f.spells[18425] = silence;
    CHECK(formatSpellDescriptionText(13754,
        "Gives your Kick ability a $h% chance to silence the target for $18425d.", f.env) ==
        "Gives your Kick ability a 50% chance to silence the target for 2 sec.");

    SpellDescriptionRecord hemo;
    hemo.basePoints[1] = 109; hemo.dieSides[1] = 1; hemo.baseDice[1] = 1;
    hemo.basePoints[2] = 12; hemo.dieSides[2] = 1; hemo.baseDice[2] = 1;
    hemo.procCharges = 10;
    hemo.durationMs = 15000;
    f.spells[16511] = hemo;
    CHECK(formatSpellDescriptionText(16511,
        "deals $s2% weapon damage ... by up to $s3.  Lasts $n charges or $d.", f.env) ==
        "deals 110% weapon damage ... by up to 13.  Lasts 10 charges or 15 sec.");
}

TEST_CASE("Level scaling truncates like the server", "[spell-description]") {
    Fixture f;
    // Serrated Blades rank 1, effect 2: -1 base, -2.67 per level, level 40.
    SpellDescriptionRecord r;
    r.basePoints[1] = -1; r.dieSides[1] = 1; r.baseDice[1] = 1;
    r.realPointsPerLevel[1] = -2.67f;
    f.spells[14171] = r;
    CHECK(formatSpellDescriptionText(14171, "ignore $s2 of your target's Armor", f.env) ==
          "ignore 106 of your target's Armor");
}

TEST_CASE("Expressions with combo points and attack power", "[spell-description]") {
    Fixture f;
    // Eviscerate rank 10: 59 base, 121 die, 185 per combo point; AP 200.
    SpellDescriptionRecord r = effect(59, 121, 1);
    r.pointsPerComboPoint[0] = 185.0f;
    f.spells[26865] = r;
    f.player["AP"] = 200.0;
    CHECK(formatSpellDescriptionText(26865,
        "   1 point  : ${$m1+($b1*1)+$AP*0.03}-${$M1+($b1*1)+$AP*0.03} damage", f.env) ==
        "   1 point  : 251-371 damage");
}

TEST_CASE("Plural follows the number before it, whitespace is kept", "[spell-description]") {
    Fixture f;
    SpellDescriptionRecord r = effect(0);
    r.basePoints[1] = 0; r.dieSides[1] = 1; r.baseDice[1] = 1;
    f.spells[1776] = r;
    CHECK(formatSpellDescriptionText(1776, "Any damage.  Awards $s2 combo $lpoint:points;.", f.env) ==
          "Any damage.  Awards 1 combo point.");
    f.spells[14183] = effect(1);
    CHECK(formatSpellDescriptionText(14183, "adds $s1 combo $lpoint:points;", f.env) ==
          "adds 2 combo points");
}

TEST_CASE("Regeneration totals are not whole ticks", "[spell-description]") {
    Fixture f;
    // Food 433 (Darnassian Bleu): 17 per 5 sec, aura 84, 18 sec.
    SpellDescriptionRecord r = effect(16);
    r.auraType[0] = 84;
    r.durationMs = 18000;
    f.spells[433] = r;
    CHECK(formatSpellDescriptionText(433, "Restores $o1 health over $d.", f.env) ==
          "Restores 61 health over 18 sec.");
}

TEST_CASE("Unknown tokens stay visible and are reported", "[spell-description]") {
    Fixture f;
    f.spells[1] = effect(0);
    CHECK(formatSpellDescriptionText(1, "Deals ${$MW*2} damage", f.env) == "Deals ${$MW*2} damage");
    REQUIRE(f.unresolved.size() == 1);
    CHECK(f.unresolved[0] == "${$MW*2}");
}

TEST_CASE("Durations use the largest whole unit", "[spell-description]") {
    CHECK(formatSpellDescriptionDuration(6000) == "6 sec");
    CHECK(formatSpellDescriptionDuration(1800000) == "30 min");
    CHECK(formatSpellDescriptionDuration(7200000) == "2 hrs");
    CHECK(formatSpellDescriptionDuration(3600000) == "1 hour");
    CHECK(formatSpellDescriptionDuration(1500) == "1.5 sec");
}

TEST_CASE("An aura tooltip states its stacked amount", "[spell-description]") {
    // Native pair 8: Sunder Armor 25225 ($s1 = 520) at three stacks reads
    // "Armor decreased by 1560." (_experiments/20261118T0807.aura_tooltip_dispel).
    Fixture f;
    f.spells[25225] = effect(-521);
    CHECK(formatSpellDescriptionText(25225, "Armor decreased by $s1.", f.env) ==
          "Armor decreased by 520.");
    f.env.auraStacks = 3;
    CHECK(formatSpellDescriptionText(25225, "Armor decreased by $s1.", f.env) ==
          "Armor decreased by 1560.");
}
