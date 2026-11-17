#include <catch_amalgamated.hpp>

#include "game/loot_chat.hpp"

#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

namespace lc = wowee::game::loot_chat;

// The two-player roll the user captured in Scarlet Monastery: Graulberto
// passed, Graulberon chose Greed, rolled 15 and won. Each line is the one the
// original client printed, read off its screenshot.
TEST_CASE("Aegis roll reads as the original client printed it", "[loot][chat]") {
    const std::string link = "[Aegis of the Scarlet Commander]";

    auto passed = lc::decodeRollEcho(128, 128);
    REQUIRE(passed == lc::RollEcho::Passed);
    CHECK(lc::rollEchoLine(*passed, 128, false, true, false, "Graulberto", link) ==
          "You passed on: [Aegis of the Scarlet Commander]");

    auto greed = lc::decodeRollEcho(128, 2);
    REQUIRE(greed == lc::RollEcho::GreedSelected);
    CHECK(lc::rollEchoLine(*greed, 128, false, false, false, "Graulberon", link) ==
          "Graulberon has selected Greed for: [Aegis of the Scarlet Commander]");

    auto rolled = lc::decodeRollEcho(15, 2);
    REQUIRE(rolled == lc::RollEcho::GreedRolled);
    CHECK(lc::rollEchoLine(*rolled, 15, false, false, false, "Graulberon", link) ==
          "Greed Roll - 15 for [Aegis of the Scarlet Commander] by Graulberon");

    CHECK(lc::rollWonLine(15, 2, false, true, "Graulberon", link) ==
          "Graulberon won: [Aegis of the Scarlet Commander]");

    CHECK(lc::itemPushLine(lc::itemPushKind(0, 0), false, 1, "Graulberon", link) ==
          "Graulberon receives loot: [Aegis of the Scarlet Commander].");
}

// Need is announced as (0, 0). Read as a vote of 0 it is a pass, which is
// what "passed despite rolling Need" was.
TEST_CASE("A chosen Need is not a pass", "[loot][chat]") {
    REQUIRE(lc::decodeRollEcho(0, 0) == lc::RollEcho::NeedSelected);
    CHECK(lc::rollEchoLine(lc::RollEcho::NeedSelected, 0, false, true, false, "X", "[I]") ==
          "You have selected Need for: [I]");
    CHECK(lc::rollEchoLine(lc::RollEcho::NeedSelected, 0, false, false, false, "Edrik", "[I]") ==
          "Edrik has selected Need for: [I]");
    REQUIRE(lc::decodeRollEcho(97, 1) == lc::RollEcho::NeedRolled);
    CHECK(lc::rollEchoLine(lc::RollEcho::NeedRolled, 97, false, true, false, "Edric", "[I]") ==
          "Need Roll - 97 for [I] by Edric");
}

TEST_CASE("Steps the server never sends are not guessed at", "[loot][chat]") {
    CHECK_FALSE(lc::decodeRollEcho(128, 1));
    CHECK_FALSE(lc::decodeRollEcho(0, 2));
    CHECK_FALSE(lc::decodeRollEcho(101, 2));
    CHECK_FALSE(lc::decodeRollEcho(5, 3));
    CHECK_FALSE(lc::decodeRollEcho(0, 128));
}

TEST_CASE("Automatic passes and the winner without detail", "[loot][chat]") {
    CHECK(lc::rollEchoLine(lc::RollEcho::Passed, 128, true, true, false, "", "[I]") ==
          "You automatically passed on: [I] because you cannot loot that item.");
    CHECK(lc::rollEchoLine(lc::RollEcho::Passed, 128, true, false, true, "Edrika", "[I]") ==
          "Edrika automatically passed on: [I] because she cannot loot that item.");
    CHECK(lc::rollEchoLine(lc::RollEcho::Passed, 128, true, false, false, "Edrik", "[I]") ==
          "Edrik automatically passed on: [I] because he cannot loot that item.");
    CHECK(lc::rollWonLine(15, 2, false, false, "Graulberon", "[I]") ==
          "Graulberon won: [I] |cff818181(Greed - 15)|r");
    CHECK(lc::rollWonLine(88, 1, true, false, "Graulberon", "[I]") ==
          "You won: [I] |cff818181(Need - 88)|r");
    CHECK(lc::rollWonLine(88, 1, true, true, "Graulberon", "[I]") == "You won: [I]");
}

TEST_CASE("Item push lines follow the packet's received and created words", "[loot][chat]") {
    CHECK(lc::itemPushLine(lc::itemPushKind(0, 0), true, 1, "", "[I]") == "You receive loot: [I].");
    CHECK(lc::itemPushLine(lc::itemPushKind(0, 0), true, 4, "", "[I]") == "You receive loot: [I]x4.");
    CHECK(lc::itemPushLine(lc::itemPushKind(1, 0), true, 1, "", "[I]") == "You receive item: [I].");
    CHECK(lc::itemPushLine(lc::itemPushKind(1, 1), true, 2, "", "[I]") == "You create: [I]x2.");
    CHECK(lc::itemPushLine(lc::itemPushKind(0, 0), false, 3, "Edrik", "[I]") ==
          "Edrik receives loot: [I]x3.");
    CHECK_FALSE(lc::itemPushLine(lc::itemPushKind(1, 0), false, 1, "Edrik", "[I]"));
}

// Every pattern against the client's own GlobalStrings.lua. The path comes
// from the environment because the extracted client data is not part of this
// repository, e.g.
//   WOWEE_TBC_GLOBALSTRINGS=/workspace/azerothcore/client/wowee-data/expansions/tbc/interface/framexml/globalstrings.lua
TEST_CASE("Patterns are GlobalStrings.lua verbatim", "[loot][chat][data]") {
    const char* path = std::getenv("WOWEE_TBC_GLOBALSTRINGS");
    if (!path) SKIP("WOWEE_TBC_GLOBALSTRINGS not set");
    std::ifstream in(path);
    REQUIRE(in.good());
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string lua = ss.str();
    for (const auto& p : lc::kAllPatterns) {
        INFO(p.name);
        const std::regex line("(^|\\n)" + std::string(p.name) + " = \"([^\"]*)\";");
        std::smatch m;
        REQUIRE(std::regex_search(lua, m, line));
        CHECK(m[2].str() == p.text);
    }
}
