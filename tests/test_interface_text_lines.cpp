// How many lines a label takes, asked on demand.
//
// FrameXML sets a string's text and reads its height in the same breath -
// WatchFrame_SetLine writes an objective, gives it a width and asks how tall it
// came out, all before anything is drawn again. So the count cannot come from
// the last frame's measure: a row that was just reset measures zero, and the
// tracker then gave a two-line objective a one-line row and drew the rows below
// it over the second line.
//
// Measured without a font here, which is the fallback path: no ImGui context
// means half the point size per character, so at the default twelve a character
// is six units wide and the widths below read as character counts times six.
#include <catch_amalgamated.hpp>

#include "ui/interface_fonts.hpp"

using wowee::ui::interfaceTextLines;

namespace {
/// The default point size with no context, so the numbers below are readable.
constexpr float kChar = 6.0f;  // 12.0f * 0.5f
}  // namespace

TEST_CASE("A label with nothing in it takes no lines", "[fonts][wrap]") {
    REQUIRE(interfaceTextLines("", "", 0.0f, 100.0f, false) == 0);
}

TEST_CASE("A label that wraps at nothing is one line", "[fonts][wrap]") {
    // Zero width is "as wide as your own text", which is what a label sized by
    // its text asks for - it cannot wrap, however long it is.
    REQUIRE(interfaceTextLines("Return to Tavernkeep Smitts at Darkshire", "",
                               0.0f, 0.0f, false) == 1);
}

TEST_CASE("A label wraps inside the width it was given", "[fonts][wrap]") {
    // "Return to" is nine characters and fits; " the" would make thirteen and
    // does not.
    REQUIRE(interfaceTextLines("Return to the inn", "", 0.0f, 10 * kChar, false) == 2);
    // Wide enough for all seventeen, so it stays on one line.
    REQUIRE(interfaceTextLines("Return to the inn", "", 0.0f, 20 * kChar, false) == 1);
}

TEST_CASE("A newline breaks a line at any width", "[fonts][wrap]") {
    // |n is WoW's spelling of a line break, and it breaks whether or not
    // anything is wrapping.
    REQUIRE(interfaceTextLines("one|ntwo", "", 0.0f, 0.0f, false) == 2);
}

TEST_CASE("TBC font rasterization limit applies after UI scaling", "[fonts][scale]") {
    using wowee::ui::interfaceFontSize;
    wowee::ui::setInterfaceFontSizeLimit(32.0f);
    for (float scale : {0.5f, 0.9375f, 1.0f, 2.0f}) {
        CHECK(interfaceFontSize(102.0f, scale) * scale == Catch::Approx(32.0f));
        CHECK(interfaceFontSize(12.0f, scale) * scale == Catch::Approx(12.0f * scale));
    }
    wowee::ui::setInterfaceFontSizeLimit(0.0f);
    CHECK(interfaceFontSize(102.0f, 2.0f) == 102.0f);
}

TEST_CASE("A drop shadow to the right widens a string by whole pixels", "[fonts][shadow]") {
    // Read off 2.4.3 at 0.9375 pixels per unit: SetShadowOffset(x, y) grows
    // GetStringWidth("Strength:") by round(x * ppu) for x > 0, never for x < 0,
    // and y changes nothing.
    using wowee::ui::shadowPixels;
    CHECK(shadowPixels(0.0f, 0.9375f) == 0);
    CHECK(shadowPixels(0.4f, 0.9375f) == 0);
    CHECK(shadowPixels(0.5f, 0.9375f) == 0);
    CHECK(shadowPixels(0.6f, 0.9375f) == 1);
    CHECK(shadowPixels(1.0f, 0.9375f) == 1);
    CHECK(shadowPixels(1.5f, 0.9375f) == 1);
    CHECK(shadowPixels(2.0f, 0.9375f) == 2);
    CHECK(shadowPixels(3.0f, 0.9375f) == 3);
    CHECK(shadowPixels(-1.0f, 0.9375f) == 0);
    CHECK(shadowPixels(-2.0f, 0.9375f) == 0);
}

TEST_CASE("Line spacing rounds up to whole pixels", "[fonts][spacing]") {
    // SetSpacing on 2.4.3, read back through GetHeight of an eight-row string.
    using wowee::ui::spacingPixels;
    CHECK(spacingPixels(0.0f, 0.5625f) == 0);
    CHECK(spacingPixels(1.0f, 0.5625f) == 1);
    CHECK(spacingPixels(2.0f, 0.5625f) == 2);
    CHECK(spacingPixels(3.0f, 0.5625f) == 2);
    CHECK(spacingPixels(5.0f, 0.5625f) == 3);
    CHECK(spacingPixels(2.0f, 0.9375f) == 2);
    CHECK(spacingPixels(5.0f, 0.9375f) == 5);
}
