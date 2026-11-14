#include <catch_amalgamated.hpp>
#include "game/tbc_cast_results.hpp"
#include "game/spell_defines.hpp"
#include <string_view>

using namespace wowee::game;

TEST_CASE("TBC insufficient power keeps the player's resource name", "[tbc][spells]") {
    REQUIRE(kTbcCastResults.at(0x50) == 85);
    CHECK(std::string_view(getSpellCastResultString(kTbcCastResults.at(0x50), 3)) == "Not enough energy");
    CHECK(std::string_view(getSpellCastResultString(kTbcCastResults.at(0x50), 1)) == "Not enough rage");
    CHECK(std::string_view(getSpellCastResultString(kTbcCastResults.at(0x50), 0)) == "Not enough mana");
    CHECK(std::string_view(getSpellCastResultString(kTbcCastResults.at(0x4C), 3)) == "No fish");
}

TEST_CASE("TBC result insertions and removals preserve meanings", "[tbc][spells]") {
    CHECK(kTbcCastResults.at(0) == 1);
    CHECK(kTbcCastResults.at(0x3F) == 67); // Not ready.
    CHECK(kTbcCastResults.at(0x5C) == 97); // Out of range.
    CHECK(kTbcCastResults.at(0x7F) == 132); // Try again.
    CHECK(kTbcCastResults.at(0xA8) == 187); // Unknown.
    for (auto result : kTbcCastResults) CHECK(getSpellCastResultString(result) != nullptr);
}
