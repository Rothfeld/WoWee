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

TEST_CASE("a click on a plate resolves to that plate's unit", "[nameplate][click]") {
    using wowee::ui::PlateBox;
    using wowee::ui::PlateHit;
    using wowee::ui::plateUnitAt;
    // Ledger #94: right-clicking an enemy's plate with a corpse selected
    // looted the corpse, because the click never resolved to the plate's unit.
    const std::vector<PlateHit> plates = {
        {.guid = 0xF130000000000A01ull, .box = PlateBox{.x0 = 100, .y0 = 50, .x1 = 230, .y1 = 80}},
        {.guid = 0xF130000000000B02ull, .box = PlateBox{.x0 = 180, .y0 = 60, .x1 = 310, .y1 = 90}},
    };
    CHECK(plateUnitAt(plates, 120, 60) == 0xF130000000000A01ull);
    CHECK(plateUnitAt(plates, 300, 85) == 0xF130000000000B02ull);
    // Overlap: the plate drawn last is on top.
    CHECK(plateUnitAt(plates, 200, 70) == 0xF130000000000B02ull);
    // Edges belong to the box; outside is nobody.
    CHECK(plateUnitAt(plates, 100, 50) == 0xF130000000000A01ull);
    CHECK(plateUnitAt(plates, 99, 60) == 0);
    CHECK(plateUnitAt(plates, 400, 400) == 0);
    CHECK(plateUnitAt({}, 120, 60) == 0);
}

TEST_CASE("Hovering an NPC does not reveal a name its setting hides") {
    using wowee::ui::unitWorldNameShown;
    // UnitNameNPC=0, NPC hovered but not targeted, no plate: no name.
    CHECK_FALSE(unitWorldNameShown(false, false, false, false));
    CHECK(unitWorldNameShown(false, false, true, false));   // target
    CHECK(unitWorldNameShown(false, true, false, false));   // visible plate
    CHECK(unitWorldNameShown(false, false, false, true));   // UnitNameNPC=1
    CHECK(unitWorldNameShown(true, false, false, false));   // own character
}

namespace {
// Build 8606 plates read back with GetPoint on pair 7
// (_experiments/20261118T074350Z.plates_fct), in UI units on the 768-unit
// screen: x centre, frame top measured upward, anchor depth.
struct Measured { float x, topUp, depth, observedX, observedTopUp; };

void checkSpread(const std::vector<Measured>& measured) {
    const float artScale = 156.651184f / 128.0f;
    std::vector<LegacyPlate> plates;
    for (const auto& m : measured)
        plates.push_back({.naturalX = m.x, .naturalTop = 768.0f - m.topUp, .depth = m.depth});
    std::vector<LegacyPlate*> order;
    for (auto& p : plates) order.push_back(&p);
    legacyPlateLayout(order, artScale);
    for (size_t i = 0; i < measured.size(); ++i) {
        CHECK(plates[i].x == Catch::Approx(measured[i].observedX).margin(1.0f));
        CHECK(768.0f - plates[i].top == Catch::Approx(measured[i].observedTopUp).margin(1.0f));
    }
}
}  // namespace

TEST_CASE("Build 8606 lifts the nearer of two stacked plates one frame height", "[nameplates][spread]") {
    // g2 59: Warrior 10 (nearer) above-left of Warrior 1.
    checkSpread({{704.88f, 663.94f, 7.90f, 704.88f, 663.94f},
                 {678.32f, 682.36f, 5.61f, 678.32f, 663.94f + 39.162796f}});
}

TEST_CASE("Build 8606 sets nearly level plates side by side", "[nameplates][spread]") {
    // g1 95: the right plate moves right to one frame width.
    checkSpread({{612.65f, 672.95f, 7.74f, 612.65f, 672.95f},
                 {694.74f, 680.48f, 5.55f, 612.65f + 156.651184f, 680.48f}});
    // w1 23: the right plate is the far one; it is the one that moves.
    checkSpread({{735.99f, 673.79f, 7.77f, 672.82f + 156.651184f, 673.79f},
                 {672.82f, 680.31f, 5.55f, 672.82f, 680.31f}});
}

TEST_CASE("Build 8606 spreads four plates into rows", "[nameplates][spread]") {
    // t1 0: Warrior 15 above Warrior 20, Warrior 1 above Warrior 15.
    checkSpread({{333.68f, 430.06f, 10.29f, 333.58f, 449.80f},
                 {546.80f, 452.25f, 11.12f, 546.81f, 452.08f},
                 {359.55f, 391.54f, 12.38f, 359.52f, 410.64f},
                 {501.42f, 371.57f, 13.12f, 501.41f, 371.48f}});
    // cal 5: only the nearest plate moves.
    checkSpread({{856.08f, 483.72f, 11.31f, 856.10f, 483.24f},
                 {1048.85f, 467.43f, 9.66f, 1048.93f, 480.43f},
                 {961.12f, 441.35f, 12.66f, 961.10f, 441.27f},
                 {1095.32f, 384.67f, 11.52f, 1095.31f, 384.58f}});
}
