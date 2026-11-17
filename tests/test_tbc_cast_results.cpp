#include <catch_amalgamated.hpp>
#include "game/tbc_cast_results.hpp"
#include "game/cast_result_text.hpp"
#include <string_view>

using namespace wowee::game;

TEST_CASE("TBC insufficient power names the spell's resource", "[tbc][spells]") {
    REQUIRE(kTbcCastResults.at(0x50) == 85);
    CHECK(std::string_view(noPowerGlobalName(3)) == "ERR_OUT_OF_ENERGY");
    CHECK(std::string_view(noPowerGlobalName(1)) == "ERR_OUT_OF_RAGE");
    CHECK(std::string_view(noPowerGlobalName(0)) == "ERR_OUT_OF_MANA");
    CHECK(std::string_view(noPowerGlobalName(254)) == "ERR_OUT_OF_HEALTH");
    CHECK(noPowerGlobalName(4) == nullptr);
}

TEST_CASE("TBC result insertions and removals preserve meanings", "[tbc][spells]") {
    CHECK(kTbcCastResults.at(0) == 1);
    CHECK(kTbcCastResults.at(0x3F) == 67); // Not ready.
    CHECK(kTbcCastResults.at(0x5C) == 97); // Out of range.
    CHECK(kTbcCastResults.at(0x7F) == 132); // Try again.
    CHECK(kTbcCastResults.at(0xA8) == 187); // Unknown.
}

TEST_CASE("Cast results name their GlobalStrings line", "[tbc][spells]") {
    // Riposte without a parry: TBC 0x14 -> CASTER_AURASTATE, "You can't do that yet".
    CHECK(std::string_view(castResultGlobalName(kTbcCastResults.at(0x14), true)) ==
          "SPELL_FAILED_CASTER_AURASTATE");
    CHECK(std::string_view(castResultGlobalName(kTbcCastResults.at(0x6A), true)) ==
          "SPELL_FAILED_TARGET_AURASTATE");
    CHECK(std::string_view(castResultGlobalName(kTbcCastResults.at(0x61), true)) ==
          "SPELL_FAILED_REQUIRES_SPELL_FOCUS");
    CHECK(std::string_view(castResultGlobalName(kTbcCastResults.at(0x7E), true)) ==
          "SPELL_FAILED_TRAINING_POINTS");
    CHECK(std::string_view(castResultGlobalName(kTbcCastResults.at(0x89), true)) ==
          "SPELL_FAILED_PROSPECT_NEED_MORE");
    CHECK(std::string_view(castResultGlobalName(55, false)) == "SPELL_FAILED_NEED_MORE_ITEMS");
    CHECK(castResultGlobalName(kTbcCastResults.at(0x19), true) == nullptr); // DONT_REPORT
    // Every TBC wire value but DONT_REPORT has a name; the TBC enum comment
    // beside each table entry is that name.
    for (size_t raw = 0; raw < kTbcCastResults.size(); ++raw) {
        if (raw == 0x19) continue;
        CHECK(castResultGlobalName(kTbcCastResults[raw], true) != nullptr);
    }
}

TEST_CASE("GlobalStrings patterns fill like Lua format", "[spells]") {
    CHECK(formatGlobalString("Requires %s", {"Forge"}) == "Requires Forge");
    CHECK(formatGlobalString("Must have a %s equipped in the main hand", {"Dagger"}) ==
          "Must have a Dagger equipped in the main hand");
    CHECK(formatGlobalString("Requires %d %s.", {"5", "Copper Ore"}) == "Requires 5 Copper Ore.");
    CHECK(formatGlobalString("100%% %s", {"done"}) == "100% done");
    CHECK(formatGlobalString("Requires %s", {}).empty());
    CHECK(formatGlobalString("Requires %s", {"a", "b"}).empty());
    CHECK(globalStringHasConversions("Missing reagent: %s"));
    CHECK_FALSE(globalStringHasConversions("You can't do that yet"));
    CHECK_FALSE(globalStringHasConversions("100%% sure"));
}
