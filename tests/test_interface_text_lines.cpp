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
#include "ui/message_frame_layout.hpp"
#include "ui/text_wrap.hpp"

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

TEST_CASE("MessageFrame and ScrollingMessageFrame line placement follows build 8606", "[message-frame][layout]") {
    using wowee::ui::placeMessageFrameLines;
    using wowee::ui::textOriginY;
    using wowee::ui::spacingPixels;

    // UIErrorsFrame on 1280x720 (ws = 0.9375): TOP y=-122, height=60, ErrorFont=16.
    // Box spans y0=114.375..170.625; pixelHeight=15, slot advance=15.
    {
        const float ws = 0.9375f;
        const float y0 = 122.0f * ws, y1 = 182.0f * ws;
        const auto one = placeMessageFrameLines(
            y0, y1, 16.0f * ws, 15, 0, 0.0f, 1.0f, true, true, 0, 1,
            [](size_t) { return true; }, [](size_t) { return size_t{1}; });
        REQUIRE(one.size() == 1);
        CHECK(one[0].index == 0);
        CHECK(textOriginY(one[0].top) == 115.0f);

        // Five messages: newest three (indices 4, 3, 2) stack downward from y0.
        const auto five = placeMessageFrameLines(
            y0, y1, 16.0f * ws, 15, 0, 0.0f, 1.0f, true, true, 0, 5,
            [](size_t) { return true; }, [](size_t) { return size_t{1}; });
        REQUIRE(five.size() == 3);
        CHECK(five[0].index == 4);
        CHECK(five[1].index == 3);
        CHECK(five[2].index == 2);
        CHECK(textOriginY(five[0].top) == 115.0f);
        CHECK(textOriginY(five[1].top) == 130.0f);
        CHECK(textOriginY(five[2].top) == 145.0f);
    }

    // MessageFrame insertMode="BOTTOM", height=60, font=12 at ws=0.9375:
    // scaled=11.25 -> slot advance=12, 4 slots in 56.25 px (slotBottom = y0 + 48).
    // Two messages (indices 0, 1) occupy slots 2 and 3 (newest at slot 3).
    {
        const float ws = 0.9375f;
        const float y0 = 260.0f * ws, y1 = 320.0f * ws; // 243.75 .. 300.0
        const auto two = placeMessageFrameLines(
            y0, y1, 12.0f * ws, 11, 0, 0.0f, 1.0f, true, false, 0, 2,
            [](size_t) { return true; }, [](size_t) { return size_t{1}; });
        REQUIRE(two.size() == 2);
        CHECK(two[0].index == 1);
        CHECK(two[1].index == 0);
        CHECK(textOriginY(two[0].top) == 280.0f);
        CHECK(textOriginY(two[1].top) == 268.0f);
    }

    // Wrapped message in TOP MessageFrame: first row steps by ceil(size)=12,
    // wrapped continuation inside the message steps by pixelHeight=11.
    {
        const float ws = 0.9375f;
        const float y0 = 260.0f * ws, y1 = 320.0f * ws;
        // Index 0 wrapped to 2 rows, index 1 (newest) 1 row.
        const auto wrapped = placeMessageFrameLines(
            y0, y1, 12.0f * ws, 11, 0, 0.0f, 1.0f, true, true, 0, 2,
            [](size_t) { return true; },
            [](size_t i) { return i == 0 ? size_t{2} : size_t{1}; });
        REQUIRE(wrapped.size() == 2);
        CHECK(textOriginY(wrapped[0].top) == 244.0f);
        CHECK(textOriginY(wrapped[1].top) == 256.0f);
        CHECK(wrapped[1].rowHeight == 11.0f);
    }

    // ScrollingMessageFrame insertMode="TOP" with scroll offset (GuildEventMessageFrame).
    {
        const float ws = 0.9375f;
        const float y0 = 260.0f * ws, y1 = 300.0f * ws; // 37.5 px -> 3 rows of 11 px
        const auto scrolled = placeMessageFrameLines(
            y0, y1, 12.0f * ws, 11, spacingPixels(0.0f, ws), 0.0f, 1.0f,
            false, true, 1, 5,
            [](size_t) { return true; }, [](size_t) { return size_t{1}; });
        REQUIRE(scrolled.size() == 3);
        CHECK(scrolled[0].index == 3);
        CHECK(scrolled[1].index == 2);
        CHECK(scrolled[2].index == 1);
        CHECK(textOriginY(scrolled[0].top) == 244.0f);
        CHECK(textOriginY(scrolled[1].top) == 255.0f);
        CHECK(textOriginY(scrolled[2].top) == 266.0f);
    }
}
