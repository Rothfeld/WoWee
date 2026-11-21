// Caret hit-testing, visual line layout, and selection spans for EditBox widgets.
#include "catch_amalgamated.hpp"
#include "ui/edit_caret.hpp"

#include <string>
#include <vector>

namespace {

// Fixed-width synthetic metrics: every glyph is 10px wide, spaces 6px wide.
struct MonoMeasure {
    int pen(uint32_t cp, uint32_t /*next*/) const { return cp == ' ' ? 6 : 10; }
    int extent(uint32_t cp) const { return cp == ' ' ? 6 : 10; }
};

float zeroTexWidth(const wowee::ui::WrapRun& r) {
    return r.texWidth > 0.0f ? r.texWidth : (r.texHeight > 0.0f ? r.texHeight : 12.0f);
}

}  // namespace

TEST_CASE("Single-line EditBox hit-testing snaps to glyph midpoints", "[edit_caret]") {
    const MonoMeasure m;
    const auto lines = wowee::ui::layoutEditLines("hello", 0.0f, m, zeroTexWidth);
    REQUIRE(lines.size() == 1);
    REQUIRE(lines[0].cells.size() == 5);

    // Before first glyph and left half of 'h' [0, 10) -> 0
    CHECK(wowee::ui::hitTestEditCaret(lines, -5.0f, 0.0f, 12.0f, false) == 0);
    CHECK(wowee::ui::hitTestEditCaret(lines, 0.0f, 0.0f, 12.0f, false) == 0);
    CHECK(wowee::ui::hitTestEditCaret(lines, 4.9f, 0.0f, 12.0f, false) == 0);
    // Right half of 'h' -> 1
    CHECK(wowee::ui::hitTestEditCaret(lines, 5.1f, 0.0f, 12.0f, false) == 1);
    // Left half of 'e' [10, 20) -> 1, right half -> 2
    CHECK(wowee::ui::hitTestEditCaret(lines, 14.9f, 0.0f, 12.0f, false) == 1);
    CHECK(wowee::ui::hitTestEditCaret(lines, 15.1f, 0.0f, 12.0f, false) == 2);
    // Past right end -> 5
    CHECK(wowee::ui::hitTestEditCaret(lines, 120.0f, 0.0f, 12.0f, false) == 5);
}

TEST_CASE("UTF-8 multi-byte codepoints snap cleanly to codepoint boundaries", "[edit_caret]") {
    const MonoMeasure m;
    // "a" (1 byte) + "é" (2 bytes: C3 A9) + "中" (3 bytes: E4 B8 AD) + "z" (1 byte) = 7 bytes
    const std::string text = "a\xC3\xA9\xE4\xB8\xADz";
    const auto lines = wowee::ui::layoutEditLines(text, 0.0f, m, zeroTexWidth);
    REQUIRE(lines.size() == 1);
    REQUIRE(lines[0].cells.size() == 4);

    CHECK(wowee::ui::hitTestEditCaret(lines, 4.0f, 0.0f, 12.0f, false) == 0);
    CHECK(wowee::ui::hitTestEditCaret(lines, 6.0f, 0.0f, 12.0f, false) == 1);   // before é
    CHECK(wowee::ui::hitTestEditCaret(lines, 14.0f, 0.0f, 12.0f, false) == 1);  // left half of é
    CHECK(wowee::ui::hitTestEditCaret(lines, 16.0f, 0.0f, 12.0f, false) == 3);  // after é (before 中)
    CHECK(wowee::ui::hitTestEditCaret(lines, 24.0f, 0.0f, 12.0f, false) == 3);  // left half of 中
    CHECK(wowee::ui::hitTestEditCaret(lines, 26.0f, 0.0f, 12.0f, false) == 6);  // after 中 (before z)
    CHECK(wowee::ui::hitTestEditCaret(lines, 36.0f, 0.0f, 12.0f, false) == 7);  // after z
}

TEST_CASE("Hyperlink escape is atomic for caret hit-testing", "[edit_caret]") {
    const MonoMeasure m;
    // "hi " (3 bytes, 26px) + "|Hitem:1|h[AB]|h" (16 bytes, display "[AB]" = 40px) + "!" (1 byte, 10px)
    const std::string text = "hi |Hitem:1|h[AB]|h!";
    const auto lines = wowee::ui::layoutEditLines(text, 0.0f, m, zeroTexWidth);
    REQUIRE(lines.size() == 1);
    REQUIRE(lines[0].cells.size() == 5); // 'h','i',' ',link,'!'

    // Link spans x=[26, 66), midpoint = 46
    CHECK(wowee::ui::hitTestEditCaret(lines, 45.0f, 0.0f, 12.0f, false) == 3);
    CHECK(wowee::ui::hitTestEditCaret(lines, 47.0f, 0.0f, 12.0f, false) == 19);
    CHECK(wowee::ui::hitTestEditCaret(lines, 80.0f, 0.0f, 12.0f, false) == 20);
}

TEST_CASE("Multi-line explicit newlines place caret before newline and handle empty lines", "[edit_caret]") {
    const MonoMeasure m;
    // Line 0: "#show" [0..5), '\n' at 5
    // Line 1: "" [6..6), '\n' at 6
    // Line 2: "/cast" [7..12)
    const std::string text = "#show\n\n/cast";
    const auto lines = wowee::ui::layoutEditLines(text, 200.0f, m, zeroTexWidth);
    REQUIRE(lines.size() == 3);
    CHECK(lines[0].rawStart == 0);
    CHECK(lines[0].rawEnd == 5);
    CHECK(lines[1].rawStart == 6);
    CHECK(lines[1].rawEnd == 6);
    CHECK(lines[2].rawStart == 7);
    CHECK(lines[2].rawEnd == 12);

    // Above first line clamps to line 0
    CHECK(wowee::ui::hitTestEditCaret(lines, 24.0f, -10.0f, 10.0f, true) == 2);
    // Past right end of line 0 lands right BEFORE '\n' (byte 5, not 6)
    CHECK(wowee::ui::hitTestEditCaret(lines, 180.0f, 5.0f, 10.0f, true) == 5);
    // Blank line 1 anywhere lands at byte 6
    CHECK(wowee::ui::hitTestEditCaret(lines, 50.0f, 15.0f, 10.0f, true) == 6);
    // Mid line 2 ("/cast", click at x=26 -> after "/ca" = byte 10)
    CHECK(wowee::ui::hitTestEditCaret(lines, 26.0f, 25.0f, 10.0f, true) == 10);
    // Below last line places caret at end of text (byte 12)
    CHECK(wowee::ui::hitTestEditCaret(lines, 5.0f, 55.0f, 10.0f, true) == 12);
}

TEST_CASE("Multi-line word-wrapped lines hit-test and locate caret accurately", "[edit_caret]") {
    const MonoMeasure m;
    // Each word is 40px + 6px space = 46px. wrapPx = 90 fits "aaaa bbbb" (86px) on line 0,
    // wraps "cccc dddd" onto line 1.
    const std::string text = "aaaa bbbb cccc dddd";
    const auto lines = wowee::ui::layoutEditLines(text, 90.0f, m, zeroTexWidth);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].rawStart == 0);
    CHECK(lines[0].rawEnd == 10);  // "aaaa bbbb "
    CHECK(lines[1].rawStart == 10);
    CHECK(lines[1].rawEnd == 19);  // "cccc dddd"

    // Click start of wrapped line 1 -> byte 10
    CHECK(wowee::ui::hitTestEditCaret(lines, 2.0f, 15.0f, 10.0f, true) == 10);
    // locateEditCaret(10) sits at start of line 1 (x = 0)
    const auto pos10 = wowee::ui::locateEditCaret(lines, 10);
    CHECK(pos10.lineIdx == 1);
    CHECK(pos10.x == Catch::Approx(0.0f));

    // Click middle of "cccc" on line 1 (x = 24 -> byte 12)
    CHECK(wowee::ui::hitTestEditCaret(lines, 24.0f, 15.0f, 10.0f, true) == 12);
    const auto pos12 = wowee::ui::locateEditCaret(lines, 12);
    CHECK(pos12.lineIdx == 1);
    CHECK(pos12.x == Catch::Approx(20.0f));
}

TEST_CASE("Selection spans cover single-line and multi-line selections", "[edit_caret]") {
    const MonoMeasure m;
    const std::string text = "abcde\nfghij";
    const auto lines = wowee::ui::layoutEditLines(text, 200.0f, m, zeroTexWidth);
    REQUIRE(lines.size() == 2);

    // Select "cde\nfg" -> bytes [2, 8)
    const auto spans = wowee::ui::editSelectionSpans(lines, 2, 8, 4.0f);
    REQUIRE(spans.size() == 2);
    CHECK(spans[0].lineIdx == 0);
    CHECK(spans[0].x0 == Catch::Approx(20.0f));
    CHECK(spans[0].x1 == Catch::Approx(54.0f)); // 50px + 4px newline highlight
    CHECK(spans[1].lineIdx == 1);
    CHECK(spans[1].x0 == Catch::Approx(0.0f));
    CHECK(spans[1].x1 == Catch::Approx(20.0f));
}

TEST_CASE("Password EditBox masks UTF-8 codepoints to asterisks while preserving raw byte offsets",
          "[edit_caret][password]") {
    const MonoMeasure m;
    // "a" (1) + U+00E9 (2) + " " (1) + U+4E2D (3) = 7 bytes, 4 codepoints -> "****" (each '*' is 10px, not 6px space)
    const std::string raw = "a\xC3\xA9 \xE4\xB8\xAD";
    CHECK(wowee::ui::maskPasswordText(raw) == "****");
    CHECK(wowee::ui::maskPasswordText("ab\ncd") == "**\n**");

    const auto lines = wowee::ui::layoutEditLines(raw, 0.0f, m, zeroTexWidth, /*password=*/true);
    REQUIRE(lines.size() == 1);
    REQUIRE(lines[0].cells.size() == 4);
    CHECK(lines[0].width == Catch::Approx(40.0f)); // 4 * 10px ('*'), hiding space width!
    CHECK(lines[0].cells[0].rawStart == 0);
    CHECK(lines[0].cells[1].rawStart == 1);
    CHECK(lines[0].cells[2].rawStart == 3);
    CHECK(lines[0].cells[3].rawStart == 4);
    CHECK(lines[0].cells[3].rawEnd == 7);
    CHECK(wowee::ui::hitTestEditCaret(lines, 26.0f, 0.0f, 12.0f, false) == 4);
}
