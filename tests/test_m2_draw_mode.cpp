// What a model's draw mode comes out as, and how tall it says it is.
//
// modelHeight used to be filled in only on the paths that swayed, so
// everything else reached the vertex shader with zero and ModelHeight came out
// a flat 1.0. A fragment that wants to know how far up the model it sits - a
// flame card fading out over its own tip - had nothing to ask.
#include <catch_amalgamated.hpp>

#include "rendering/m2_draw_mode.hpp"

using wowee::rendering::m2DrawModeFor;

TEST_CASE("every model reports its own height", "[m2][drawmode]") {
    SECTION("a still prop") {
        // OrcBonFire's own numbers: its geometry runs from -0.30 to 8.75, and
        // 9.05 is the height the shader divides by.
        const auto draw = m2DrawModeFor(false, false, -0.3f, 8.75f);
        CHECK(draw.mode == 0);
        CHECK(draw.modelHeight == Catch::Approx(9.05f));
    }

    SECTION("measured from the model's base, or z=0 when the base is above it") {
        CHECK(m2DrawModeFor(false, false, -4.0f, 2.0f).modelHeight == Catch::Approx(6.0f));
        CHECK(m2DrawModeFor(false, false, 3.0f, 5.0f).modelHeight == Catch::Approx(5.0f));
    }

    SECTION("never zero, so a fraction of it is never a divide by nothing") {
        CHECK(m2DrawModeFor(false, false, 0.0f, 0.0f).modelHeight >= 0.05f);
    }
}

TEST_CASE("draw modes", "[m2][drawmode]") {
    CHECK(m2DrawModeFor(true, false, 0.0f, 40.0f).mode == -1);
    CHECK(m2DrawModeFor(false, true, 0.0f, 0.8f).mode == 2);
    // A banner, a tree or a root is a placed model like any other.
    CHECK(m2DrawModeFor(false, false, 0.0f, 20.0f).mode == 0);
}
