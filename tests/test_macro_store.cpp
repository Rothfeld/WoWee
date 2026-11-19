#include <catch_amalgamated.hpp>

#include "game/macro_store.hpp"

#include <sstream>

using wowee::game::kCharacterMacroFlag;
using wowee::game::Macro;
using wowee::game::MacroMergeStep;
using wowee::game::MacroSet;
using wowee::game::mergeAccountMacros;
using wowee::game::nextMacroId;
using Outcome = MacroMergeStep::Outcome;

namespace {
const Macro kDie{".die", "k", "Interface\\Icons\\ability_creature_cursed_02"};
const Macro kDamage{".damage 200000", "d", "Interface\\Icons\\ability_creature_cursed_03"};
const Macro kLevel{".levelup 70\n.maxskill", "lv", "Interface\\Icons\\ability_creature_cursed_01"};
}  // namespace

TEST_CASE("macro keys read back what they wrote", "[macro_store]") {
    MacroSet macros{{3, kLevel}, {kCharacterMacroFlag | 1, Macro{"a\\b\nc", "ChrA", ""}}};
    std::ostringstream out;
    wowee::game::writeMacroKeys(out, macros);
    REQUIRE(out.str().find("macro_3_text=.levelup 70\\n.maxskill\n") != std::string::npos);
    MacroSet read;
    std::istringstream in(out.str());
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        REQUIRE(wowee::game::readMacroKey(line.substr(0, eq), line.substr(eq + 1), read));
    }
    REQUIRE(read.size() == 2);
    REQUIRE(wowee::game::sameMacro(read.at(3), kLevel));
    REQUIRE(read.at(kCharacterMacroFlag | 1).text == "a\\b\nc");
    REQUIRE_FALSE(wowee::game::readMacroKey("action_bar_1_id", "3", read));
    REQUIRE(wowee::game::macroKeyId("macro_16777217_name") == (kCharacterMacroFlag | 1));
    REQUIRE(wowee::game::macroKeyId("macro_x_name") == 0);
}

// Native 2.4.3: GenA 1, GenB 2; GenA deleted, GenC created as 3.
TEST_CASE("a new macro takes one past the highest id of its half", "[macro_store]") {
    MacroSet macros{{2, kDie}, {kCharacterMacroFlag | 1, kDamage}};
    REQUIRE(nextMacroId(macros, false) == 3);
    REQUIRE(nextMacroId(macros, true) == (kCharacterMacroFlag | 2));
    REQUIRE(nextMacroId(MacroSet{}, false) == 1);
}

// The shared client's configs: Graulberon k, d; Zaruji and Aevy k, d, lv.
TEST_CASE("identical account macros of several characters become one set", "[macro_store]") {
    MacroSet account;
    MacroSet graulberon;
    const auto first = mergeAccountMacros(account, {{1, kDie}, {2, kDamage}}, 18, 18, &graulberon);
    REQUIRE(first.size() == 2);
    REQUIRE(first[0].outcome == Outcome::Added);
    const auto second = mergeAccountMacros(account, {{1, kDie}, {2, kDamage}, {3, kLevel}}, 18, 18, nullptr);
    REQUIRE(second[0].outcome == Outcome::Present);
    REQUIRE(second[1].outcome == Outcome::Present);
    REQUIRE(second[2].outcome == Outcome::Added);
    REQUIRE(second[2].to == 3);
    REQUIRE(account.size() == 3);
    REQUIRE(graulberon.empty());
}

TEST_CASE("an icon differing only in case is the same macro", "[macro_store]") {
    MacroSet account{{1, kDie}};
    Macro upper = kDie;
    upper.icon = "Interface\\Icons\\Ability_Creature_Cursed_02";
    const auto steps = mergeAccountMacros(account, {{1, upper}}, 18, 18, nullptr);
    REQUIRE(steps[0].outcome == Outcome::Present);
}

TEST_CASE("a different macro under a taken id waits for its own character", "[macro_store]") {
    MacroSet account{{1, kDie}};
    const auto steps = mergeAccountMacros(account, {{1, kLevel}}, 18, 18, nullptr);
    REQUIRE(steps[0].outcome == Outcome::Deferred);
    REQUIRE(account.size() == 1);
}

TEST_CASE("the logging-in character's conflicting macro moves to the next account id", "[macro_store]") {
    MacroSet account{{1, kDie}, {2, kDamage}};
    MacroSet own;
    const auto steps = mergeAccountMacros(account, {{1, kLevel}}, 18, 18, &own);
    REQUIRE(steps[0].outcome == Outcome::Moved);
    REQUIRE(steps[0].to == 3);
    REQUIRE(wowee::game::sameMacro(account.at(3), kLevel));
    REQUIRE(wowee::game::sameMacro(account.at(1), kDie));
}

TEST_CASE("the same macro under another id joins it", "[macro_store]") {
    MacroSet account{{2, kDie}};
    MacroSet own;
    const auto steps = mergeAccountMacros(account, {{1, kDie}}, 18, 18, &own);
    REQUIRE(steps[0].outcome == Outcome::Joined);
    REQUIRE(steps[0].to == 2);
    REQUIRE(account.size() == 1);
    // Without its character it is not added a second time either.
    const auto other = mergeAccountMacros(account, {{1, kDie}}, 18, 18, nullptr);
    REQUIRE(other[0].outcome == Outcome::Deferred);
}

TEST_CASE("with the account half full a moved macro goes to its character", "[macro_store]") {
    MacroSet account;
    for (uint32_t id = 1; id <= 18; ++id) account[id] = Macro{std::to_string(id), "m" + std::to_string(id), ""};
    MacroSet own{{kCharacterMacroFlag | 4, kDie}};
    const auto steps = mergeAccountMacros(account, {{20, kLevel}}, 18, 18, &own);
    REQUIRE(steps[0].outcome == Outcome::Moved);
    REQUIRE(steps[0].to == (kCharacterMacroFlag | 5));
    REQUIRE(wowee::game::sameMacro(own.at(kCharacterMacroFlag | 5), kLevel));
    REQUIRE(account.size() == 18);
}

TEST_CASE("with no room in either half the macro stays deferred", "[macro_store]") {
    MacroSet account, own;
    for (uint32_t id = 1; id <= 18; ++id) {
        account[id] = Macro{std::to_string(id), "a", ""};
        own[kCharacterMacroFlag | id] = Macro{std::to_string(id), "c", ""};
    }
    const auto steps = mergeAccountMacros(account, {{1, kLevel}}, 18, 18, &own);
    REQUIRE(steps[0].outcome == Outcome::Deferred);
}
