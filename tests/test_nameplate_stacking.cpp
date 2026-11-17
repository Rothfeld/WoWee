// Keeping nameplates off one another when overlap is turned off.
//
// The cases that matter are the ones a glance at the code agrees with and the
// screen does not: a plate lifted onto a third plate, and a lift that clears
// by so little the two bars still read as touching.
#include <catch_amalgamated.hpp>
#include "ui/nameplate_stacking.hpp"
#include "game/level_difficulty.hpp"

using namespace wowee::ui;

TEST_CASE("Legacy nameplate visibility follows unclamped viewport anchor", "[nameplates][viewport]") {
    CHECK(legacyPlateAnchorVisible(0, 0, 1, 2));
    CHECK(legacyPlateAnchorVisible(-2, 2, 1, 2));
    CHECK_FALSE(legacyPlateAnchorVisible(-2.01f, 0, 1, 2));
    CHECK_FALSE(legacyPlateAnchorVisible(2.01f, 0, 1, 2));
    CHECK_FALSE(legacyPlateAnchorVisible(0, -2.01f, 1, 2));
    CHECK_FALSE(legacyPlateAnchorVisible(0, 2.01f, 1, 2));
    CHECK_FALSE(legacyPlateAnchorVisible(0, 0, -1, 2));
    CHECK_FALSE(legacyPlateAnchorVisible(0, 0, 3, 2));
    CHECK_FALSE(legacyPlateAnchorVisible(0, 0, 0, -2));
}

TEST_CASE("Legacy plate artwork clamps with native frame dimensions", "[nameplates][viewport]") {
    for (float scale : {0.75f, 1.0f, 1.5f}) {
        CHECK(legacyPlateCenterX(640, 1280, scale) == 640);
        CHECK(legacyPlateCenterX(0, 1280, scale) == 64 * scale);
        CHECK(legacyPlateCenterX(1280, 1280, scale) == 1280 - 64 * scale);
        CHECK(legacyPlateBarTop(300, scale) == 300);
        CHECK(legacyPlateBarTop(-20, scale) == 67 * scale);
    }
    const float nativeScale = 156.65118408203f / 128.0f;
    const float top = legacyPlateBarTop(0, nativeScale) - 19 * nativeScale;
    CHECK(top == Catch::Approx(768.0f - 709.255859375f).margin(0.0001f));
}

TEST_CASE("Creature level colors include the no-XP gray boundary", "[nameplates][difficulty]") {
    using namespace wowee::game;
    CHECK(creatureLevelDifficulty(40, 24) == LevelDifficulty::Gray);
    CHECK(creatureLevelDifficulty(40, 31) == LevelDifficulty::Gray);
    CHECK(creatureLevelDifficulty(40, 32) == LevelDifficulty::Green);
    CHECK(creatureLevelDifficulty(40, 38) == LevelDifficulty::Yellow);
    CHECK(creatureLevelDifficulty(40, 43) == LevelDifficulty::Orange);
    CHECK(creatureLevelDifficulty(40, 45) == LevelDifficulty::Red);
    CHECK(grayLevel(5) == 0);
    CHECK(grayLevel(39) == 31);
    CHECK(grayLevel(59) == 47);
    CHECK(grayLevel(60) == 51);
}

namespace {
constexpr float kBarH = 8.0f;
constexpr float kTop = kBarH + 24.0f;

// The box a plate occupies once its bar top is known.
PlateBox boxFor(float x0, float x1, float y) {
    return PlateBox{x0, y - kTop, x1, y + kBarH};
}
}  // namespace

TEST_CASE("a plate with nothing in the way does not move", "[nameplates]") {
    std::vector<PlateBox> placed;
    CHECK(plateTopClearOf(placed, 100.0f, 180.0f, 400.0f, kBarH, kTop) == 400.0f);
}

TEST_CASE("a plate that would land on another is lifted clear", "[nameplates]") {
    std::vector<PlateBox> placed{boxFor(100.0f, 180.0f, 400.0f)};
    const float y = plateTopClearOf(placed, 100.0f, 180.0f, 400.0f, kBarH, kTop);
    CHECK(y < 400.0f);
    // Clear means clear: the lifted bar's bottom must sit above the other
    // plate's top, not merely at it.
    CHECK(y + kBarH < placed[0].y0);
}

TEST_CASE("plates that do not share screen space are left alone", "[nameplates]") {
    std::vector<PlateBox> placed{boxFor(100.0f, 180.0f, 400.0f)};
    // Well to the right, same height.
    CHECK(plateTopClearOf(placed, 400.0f, 480.0f, 400.0f, kBarH, kTop) == 400.0f);
    // Same column, far enough above that the boxes never meet.
    CHECK(plateTopClearOf(placed, 100.0f, 180.0f, 200.0f, kBarH, kTop) == 200.0f);
}

TEST_CASE("a plate lifted onto a third is lifted again", "[nameplates]") {
    // The case a single pass gets wrong: the space above the first plate is
    // already taken, so one lift is not enough.
    std::vector<PlateBox> placed{
        boxFor(100.0f, 180.0f, 400.0f),
        boxFor(100.0f, 180.0f, 360.0f),
    };
    auto clearsAll = [&](float y) {
        for (const auto& r : placed) {
            if (!((y + kBarH < r.y0) || (y - kTop > r.y1))) return false;
        }
        return true;
    };
    CHECK(clearsAll(plateTopClearOf(placed, 100.0f, 180.0f, 400.0f, kBarH, kTop)));

    // And that this case is really the one being tested: allowed a single
    // lift, the plate lands on the second box and stays there. Without this
    // the check above passes for a version that only ever lifts once.
    CHECK_FALSE(clearsAll(
        plateTopClearOf(placed, 100.0f, 180.0f, 400.0f, kBarH, kTop, 1)));
}

TEST_CASE("the lift gives up rather than spinning", "[nameplates]") {
    // Forty plates on one spot is not a real frame, but the loop must still
    // return: the guard bounds the work, and every lift moves upward.
    std::vector<PlateBox> placed;
    for (int i = 0; i < 40; ++i) placed.push_back(boxFor(100.0f, 180.0f, 400.0f));
    const float y = plateTopClearOf(placed, 100.0f, 180.0f, 400.0f, kBarH, kTop);
    CHECK(y <= 400.0f);
}
