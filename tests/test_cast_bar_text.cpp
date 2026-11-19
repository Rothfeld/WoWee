#include <catch_amalgamated.hpp>
#include "game/cast_bar_text.hpp"

using namespace wowee::game;

// Original 2.4.3, pair 8 (_experiments/20261119T1330Z.castbar_text) and
// pair 0 (_experiments/20261117T171500Z.pair0_go_ghost_pet).
TEST_CASE("Cast bar text: object text, else nothing for NO_CASTING_BAR_TEXT, else the spell",
          "[tbc][spells][castbar]") {
    // Battered Chest, empty opening text, Opening 3365 (AttributesEx3 0).
    CHECK(castBarText("Opening", 0, std::string{}) == "Opening");
    // Sapphire Signal Fire, "Extinguishing", Opening 3365.
    CHECK(castBarText("Opening", 0, std::string{"Extinguishing"}) == "Extinguishing");
    // Glowcap, "Collecting", Opening - No Text 22810 (AttributesEx3 0x4).
    CHECK(castBarText("Opening - No Text", 0x4, std::string{"Collecting"}) == "Collecting");
    // Scarlet Monastery Torch, empty opening text, Opening 6477 (0x4).
    CHECK(castBarText("Opening", 0x4, std::string{}).empty());
    // Rizzle's Guarded Plans, "Examining", Opening 6477.
    CHECK(castBarText("Opening", 0x4, std::string{"Examining"}) == "Examining");
    // Not an OPEN_LOCK cast at an object: Hearthstone.
    CHECK(castBarText("Hearthstone", 0, std::nullopt) == "Hearthstone");
    CHECK(castBarText("Spawn Magic Sucker Device Bunny", 0x4, std::nullopt).empty());
}
