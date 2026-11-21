#include <catch_amalgamated.hpp>

#include "core/coordinates.hpp"
#include "game/reputation_standing.hpp"
#include "rendering/world_map/coordinate_projection.hpp"

using namespace wowee;
using rendering::world_map::ZoneBounds;
using rendering::world_map::renderPosToMapUV;

// The flight map places a node by the same chain the world map places the
// player: server position → canonical → render → UV against the continent
// rectangle. Every step of that is somewhere else's code; what is pinned here
// is the *order*, because getting it wrong does not fail, it draws every marker
// somewhere plausible and wrong.
//
// It has been wrong before. Feeding TaxiNodes.dbc positions straight into
// canonicalToRender - skipping the server-to-canonical swap because the numbers
// look like coordinates already - transposed every node on the flight map.

namespace {

/// What taxiNodeMapPos does, in the order it does it.
glm::vec2 nodeToMapUV(float serverX, float serverY, float serverZ,
                      const ZoneBounds& continent) {
    const glm::vec3 canonical =
        core::coords::serverToCanonical(glm::vec3(serverX, serverY, serverZ));
    const glm::vec3 render = core::coords::canonicalToRender(canonical);
    return renderPosToMapUV(render, continent, /*isContinent=*/true);
}

/// Shaped like a real continent row: the bounds descend left→right and
/// top→bottom, which is how WorldMapArea.dbc stores them.
ZoneBounds continent() {
    ZoneBounds b;
    b.locLeft = 16000.0f;  b.locRight  = -16000.0f;
    b.locTop  = 10000.0f;  b.locBottom = -10000.0f;
    return b;
}

} // namespace

TEST_CASE("The middle of the continent is the middle of the map", "[taxi][map]") {
    const glm::vec2 uv = nodeToMapUV(0.0f, 0.0f, 0.0f, continent());
    REQUIRE(uv.x == Catch::Approx(0.5f));
    REQUIRE(uv.y == Catch::Approx(0.5f));
}

TEST_CASE("Each corner of the rectangle lands in a corner of the map",
          "[taxi][map]") {
    const ZoneBounds c = continent();
    // Server order: x is the west axis, y is the north axis. The rectangle's
    // "left" bounds the axis renderPosToMapUV pairs with u, and "top" the one
    // it pairs with v - a pairing verified against real WorldMapArea values and
    // deliberately not "corrected" here.
    const glm::vec2 topLeft     = nodeToMapUV(c.locTop,    c.locLeft,  0.0f, c);
    const glm::vec2 bottomRight = nodeToMapUV(c.locBottom, c.locRight, 0.0f, c);

    REQUIRE(topLeft.x == Catch::Approx(0.0f));
    REQUIRE(topLeft.y == Catch::Approx(0.0f));
    REQUIRE(bottomRight.x == Catch::Approx(1.0f));
    REQUIRE(bottomRight.y == Catch::Approx(1.0f));
}

TEST_CASE("The two axes are not interchangeable", "[taxi][map]") {
    // The regression this file exists for. Swapping the pair moves a marker,
    // and if it did not, the order would not matter and nothing would have
    // broken the first time.
    const ZoneBounds c = continent();
    const glm::vec2 a = nodeToMapUV(4000.0f, 8000.0f, 0.0f, c);
    const glm::vec2 b = nodeToMapUV(8000.0f, 4000.0f, 0.0f, c);
    REQUIRE(a.x != Catch::Approx(b.x));
    REQUIRE(a.y != Catch::Approx(b.y));
}

TEST_CASE("Skipping the server-to-canonical swap moves the marker",
          "[taxi][map]") {
    // Stated as a test rather than a comment: this is the mistake that was
    // actually made, and it is silent - both paths return a perfectly good UV.
    const ZoneBounds c = continent();
    const float sx = 4000.0f, sy = -6000.0f;

    const glm::vec2 correct = nodeToMapUV(sx, sy, 0.0f, c);
    const glm::vec2 skipped = renderPosToMapUV(
        core::coords::canonicalToRender(glm::vec3(sx, sy, 0.0f)), c, true);

    REQUIRE(correct.x != Catch::Approx(skipped.x));
}

TEST_CASE("A node outside the continent still projects, off the map",
          "[taxi][map]") {
    // Not clamped: a node belonging to another continent should land outside
    // [0,1] so it reads as wrong, rather than being pinned to an edge where it
    // would look like a real place.
    const glm::vec2 uv = nodeToMapUV(0.0f, 32000.0f, 0.0f, continent());
    REQUIRE(uv.x < 0.0f);
}

TEST_CASE("WorldMapContinent TaxiMin/TaxiMax bounds project TBC Eastern Kingdoms nodes",
          "[taxi][map]") {
    // WorldMapContinent.dbc map 0: TaxiMin=(-16530, -16530), TaxiMax=(12270, 12270).
    ZoneBounds wmc0;
    wmc0.locLeft = 12270.0f;
    wmc0.locRight = -16530.0f;
    wmc0.locTop = 12270.0f;
    wmc0.locBottom = -16530.0f;

    // Stormwind (node 2): server (-8840.6, 489.7)
    const glm::vec2 sw = nodeToMapUV(-8840.6f, 489.7f, 0.0f, wmc0);
    CHECK(sw.x == Catch::Approx((12270.0f - 489.7f) / 28800.0f).margin(1e-4f));
    CHECK(1.0f - sw.y == Catch::Approx((-8840.6f + 16530.0f) / 28800.0f).margin(1e-4f));

    // Sentinel Hill (node 4): server (-10629.3, 1036.9) sits south-west of Stormwind.
    const glm::vec2 sh = nodeToMapUV(-10629.3f, 1036.9f, 0.0f, wmc0);
    CHECK(sh.x < sw.x);
    CHECK((1.0f - sh.y) < (1.0f - sw.y));
}

TEST_CASE("Taxi reputation price discount matches TBC 2.4.3 standing tiers and rounding",
          "[taxi][discount]") {
    using wowee::game::discountedTaxiLegCost;
    using wowee::game::reputationPriceMultiplier;

    CHECK(reputationPriceMultiplier(1) == Catch::Approx(1.0f));  // Hated
    CHECK(reputationPriceMultiplier(4) == Catch::Approx(1.0f));  // Neutral
    CHECK(reputationPriceMultiplier(5) == Catch::Approx(0.95f)); // Friendly (5% off)
    CHECK(reputationPriceMultiplier(6) == Catch::Approx(0.90f)); // Honored (10% off)
    CHECK(reputationPriceMultiplier(7) == Catch::Approx(0.85f)); // Revered (15% off)
    CHECK(reputationPriceMultiplier(8) == Catch::Approx(0.80f)); // Exalted (20% off)

    // Ironforge -> Stormwind (50c base) at Friendly (5% off): 47.5 -> 48c
    CHECK(discountedTaxiLegCost(50, reputationPriceMultiplier(5)) == 48u);
    // Ironforge -> Shattered Sun Staging Area (27s 75c = 2775c base) at Friendly: 2636.25 -> 2636c
    CHECK(discountedTaxiLegCost(2775, reputationPriceMultiplier(5)) == 2636u);
    // Honored / Revered / Exalted on 50c
    CHECK(discountedTaxiLegCost(50, reputationPriceMultiplier(6)) == 45u);
    CHECK(discountedTaxiLegCost(50, reputationPriceMultiplier(7)) == 42u);
    CHECK(discountedTaxiLegCost(50, reputationPriceMultiplier(8)) == 40u);
}
