// The "<Owner's ...>" line under summoned units (game/summon_title.hpp).
#include <catch_amalgamated.hpp>

#include "game/summon_title.hpp"

using namespace wowee::game;

TEST_CASE("NPC-summoned elemental is a Minion", "[summon][title]") {
    // Stone Rumbler 0xF140000235000004, UNIT_FIELD_CREATEDBY = Razorfen
    // Geomancer 0xF130000012C40001, creature type 4 (Elemental).
    const auto t = summonTitle(4, 0xF130000012C40001ull, 0);
    REQUIRE(t == SummonTitle::Minion);
    CHECK(std::string(summonTitleGlobal(t)) == "UNITNAME_TITLE_MINION");
    CHECK(formatSummonTitle("%s's Minion", "Razorfen Geomancer") == "Razorfen Geomancer's Minion");
}

TEST_CASE("NPC-summoned beast is a Pet, demon is a Minion", "[summon][title]") {
    // Original captures, 20261117T164500Z.summon_title: Tamed Hyena (beast,
    // family 25) and Tamed Ravager (beast, family 0) read "<X's Pet>";
    // Voidwalker Minion (demon, family 16) reads "<Defias Conjurer's Minion>".
    const auto hyena = summonTitle(1, 0xF130000012C50001ull, 0);
    REQUIRE(hyena == SummonTitle::Pet);
    CHECK(std::string(summonTitleGlobal(hyena)) == "UNITNAME_TITLE_PET");
    CHECK(summonTitle(3, 0xF130000026B00001ull, 0) == SummonTitle::Minion);
}

TEST_CASE("non-combat pet is a Companion", "[summon][title]") {
    CHECK(summonTitle(12, 0x0000000000000004ull, 0) == SummonTitle::Companion);
    CHECK(std::string(summonTitleGlobal(SummonTitle::Companion)) == "UNITNAME_TITLE_COMPANION");
}

TEST_CASE("unowned unit has no summon title", "[summon][title]") {
    CHECK(summonTitle(4, 0, 0) == SummonTitle::None);
    CHECK(summonTitleGlobal(SummonTitle::None) == nullptr);
    CHECK(formatSummonTitle("no placeholder", "x").empty());
}

TEST_CASE("player-created units follow SummonProperties.dbc Title then fallback rules", "[summon][title]") {
    constexpr uint64_t player = 0x0000000000000027ull;

    // SummonProperties.dbc Title 1..5 overrides creature type (empirically verified on Native 2.4.3):
    // - Priest Shadowfiend (spell 34433, entry 19668, type 10 Not Specified, sp=67 Title=1) -> Pet
    CHECK(summonTitle(10, player, player, 0, /*spTitle=*/1) == SummonTitle::Pet);
    // - Mage Water Elemental (spell 31687, entry 510, type 4 Elemental, sp=67 Title=1) -> Pet
    CHECK(summonTitle(4, player, player, 0, /*spTitle=*/1) == SummonTitle::Pet);
    // - Druid Force of Nature (spell 33831, entry 1964, type 4, sp=61 Title=2) -> Guardian
    CHECK(summonTitle(4, player, 0, 0, /*spTitle=*/2) == SummonTitle::Guardian);
    // - Engineering Target Dummy (4071) & Arcanite Dragonling (19804) (type 9 Mechanical, sp=61 Title=2) -> Guardian
    CHECK(summonTitle(9, player, 0, 0, /*spTitle=*/2) == SummonTitle::Guardian);
    // - Shaman Searing Totem (3599) & Fire Elemental Totem (2894) (type 11 Totem, sp=63 Title=4) -> Totem
    CHECK(summonTitle(11, player, 0, 0, /*spTitle=*/4) == SummonTitle::Totem);
    // - Non-combat Mechanical Squirrel (4055, sp=41 Title=5) -> Companion
    CHECK(summonTitle(12, player, 0, 0, /*spTitle=*/5) == SummonTitle::Companion);
    // - Explicit Title 3 -> Minion
    CHECK(summonTitle(10, player, player, 0, /*spTitle=*/3) == SummonTitle::Minion);

    // Fallback when SummonProperties Title == 0 (e.g., SPELL_EFFECT_SUMMON_PET 56):
    CHECK(summonTitle(1, player, player) == SummonTitle::Pet);      // Hunter tamed beast
    CHECK(std::string(summonTitleGlobal(SummonTitle::Pet)) == "UNITNAME_TITLE_PET");
    CHECK(summonTitle(11, player, 0) == SummonTitle::Totem);        // Stoneskin Totem
    CHECK(std::string(summonTitleGlobal(SummonTitle::Totem)) == "UNITNAME_SUMMON_TITLE4");
    CHECK(summonTitle(3, player, player) == SummonTitle::Minion);   // Imp / Felguard (SUMMON_PET)
    CHECK(summonTitle(3, 0, player) == SummonTitle::Minion);        // Summoned demon without CREATEDBY
    CHECK(summonTitle(6, player, 0) == SummonTitle::Guardian);      // Skeletal Warrior
    CHECK(summonTitle(1, player, 0) == SummonTitle::Guardian);      // Boar Spirit
    CHECK(std::string(summonTitleGlobal(SummonTitle::Guardian)) == "UNITNAME_TITLE_GUARDIAN");
    // Charmed unit (Enslave Demon / Mind Control): neither CREATEDBY nor SUMMONEDBY set.
    CHECK(summonTitle(3, 0, 0, player) == SummonTitle::Charm);
    CHECK(std::string(summonTitleGlobal(SummonTitle::Charm)) == "UNITNAME_TITLE_CHARM");
}
