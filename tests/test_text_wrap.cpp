// Word wrapping for FontStrings.
//
// Pure arithmetic over strings and widths, which is why it is testable at all:
// the drawing side hands in an ImFont and here a character is one unit wide, so
// a width of ten means ten characters.
#include "catch_amalgamated.hpp"
#include "ui/text_wrap.hpp"

#include <string>
#include <vector>

using wowee::ui::WrapRun;
using wowee::ui::wrapText;

TEST_CASE("Indented continuation lines reserve width and retain links", "[wrap]") {
    WrapRun run;
    run.text = "one two tri six ten";
    run.link = "item:117";
    const auto lines = wrapText(std::vector<WrapRun>{run}, 8.0f, false,
        [](const std::string& text) { return static_cast<float>(text.size()); }, 2.0f);
    REQUIRE(lines.size() == 4);
    CHECK(lines[0][0].text == "one two ");
    CHECK(lines[1][0].text == "tri ");
    CHECK(lines[2][0].text == "six ");
    CHECK(lines[3][0].text == "ten");
    for (const auto& line : lines) CHECK(line[0].link == "item:117");
}

namespace {

/// One unit per character, so the numbers in these tests read as counts.
float charWidth(const std::string& s) { return static_cast<float>(s.size()); }

std::vector<WrapRun> plain(const std::string& text) {
    WrapRun r;
    r.text = text;
    return {r};
}

/// A line joined back into one string, which is what a reader wants to check.
std::string joined(const std::vector<WrapRun>& line) {
    std::string out;
    for (const WrapRun& r : line) out += r.text;
    return out;
}

std::vector<std::string> lines(const std::string& text, float width,
                               bool nonSpaceWrap = false) {
    std::vector<std::string> out;
    for (const auto& line : wrapText(plain(text), width, nonSpaceWrap, charWidth)) {
        out.push_back(joined(line));
    }
    return out;
}

}  // namespace

TEST_CASE("a width of zero means no wrapping at all", "[text_wrap]") {
    // What an auto-sized label wants: it is as wide as its own text, so there
    // is nothing to break it to.
    const auto out = lines("the quick brown fox jumps", 0.0f);
    REQUIRE(out.size() == 1);
    REQUIRE(out[0] == "the quick brown fox jumps");
}

TEST_CASE("text shorter than the box stays on one line", "[text_wrap]") {
    const auto out = lines("short", 40.0f);
    REQUIRE(out.size() == 1);
    REQUIRE(out[0] == "short");
}

TEST_CASE("a break falls between words", "[text_wrap]") {
    // "the quick " is ten and would be eleven with "brown", so the break comes
    // before it rather than inside it.
    const auto out = lines("the quick brown fox", 12.0f);
    REQUIRE(out.size() == 2);
    REQUIRE(out[0] == "the quick ");
    REQUIRE(out[1] == "brown fox");
}

TEST_CASE("the space a break replaces does not start the next line",
          "[text_wrap]") {
    for (const std::string& line : lines("alpha beta gamma delta", 11.0f)) {
        REQUIRE(!line.empty());
        REQUIRE(line.front() != ' ');
    }
}

TEST_CASE("a word wider than the box stands alone by default", "[text_wrap]") {
    // Without nonspacewrap there is nowhere to break it, so it overflows on a
    // line of its own rather than being cut.
    const auto out = lines("hi extraordinarily ok", 6.0f);
    REQUIRE(out.size() == 3);
    REQUIRE(out[0] == "hi ");
    REQUIRE(out[1] == "extraordinarily ");
    REQUIRE(out[2] == "ok");
}

TEST_CASE("nonspacewrap breaks inside a word too long to fit", "[text_wrap]") {
    const auto out = lines("abcdefghij", 4.0f, /*nonSpaceWrap=*/true);
    REQUIRE(out.size() == 3);
    REQUIRE(out[0] == "abcd");
    REQUIRE(out[1] == "efgh");
    REQUIRE(out[2] == "ij");
    // Nothing is dropped on the way.
    std::string all;
    for (const auto& l : out) all += l;
    REQUIRE(all == "abcdefghij");
}

TEST_CASE("no text is lost across a wrap", "[text_wrap]") {
    const std::string text =
        "You have been chosen to carry word of this to the King himself.";
    for (float width : {8.0f, 13.0f, 21.0f, 34.0f}) {
        std::string all;
        for (const auto& l : lines(text, width)) all += l;
        // The only thing a break removes is the space it stands in for.
        std::string stripped;
        for (char c : text) if (c != ' ') stripped += c;
        std::string got;
        for (char c : all) if (c != ' ') got += c;
        REQUIRE(got == stripped);
    }
}

TEST_CASE("a colour run split across lines keeps its colour on both",
          "[text_wrap]") {
    // This is why the wrap works on runs rather than on the stripped string.
    WrapRun red;
    red.text = "alpha beta gamma";
    red.hasColor = true;
    red.rgba[0] = 1.0f; red.rgba[1] = 0.0f; red.rgba[2] = 0.0f; red.rgba[3] = 1.0f;

    const auto out = wrapText({red}, 12.0f, false, charWidth);
    REQUIRE(out.size() == 2);
    for (const auto& line : out) {
        REQUIRE(!line.empty());
        for (const WrapRun& r : line) {
            REQUIRE(r.hasColor);
            REQUIRE(r.rgba[0] == 1.0f);
            REQUIRE(r.rgba[1] == 0.0f);
        }
    }
}

TEST_CASE("adjacent pieces of one style merge back into a single run",
          "[text_wrap]") {
    // Otherwise a line of five words is five runs, and every one of them is a
    // separate draw call for no reason.
    const auto out = wrapText(plain("a b c d e"), 40.0f, false, charWidth);
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].size() == 1);
    REQUIRE(out[0][0].text == "a b c d e");
}

TEST_CASE("a break between two runs does not merge their styles",
          "[text_wrap]") {
    WrapRun plainRun;
    plainRun.text = "white ";
    WrapRun colored;
    colored.text = "red";
    colored.hasColor = true;
    colored.rgba[0] = 1.0f; colored.rgba[1] = 0.0f; colored.rgba[2] = 0.0f;

    const auto out = wrapText({plainRun, colored}, 40.0f, false, charWidth);
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].size() == 2);
    REQUIRE(out[0][0].text == "white ");
    REQUIRE(!out[0][0].hasColor);
    REQUIRE(out[0][1].text == "red");
    REQUIRE(out[0][1].hasColor);
}

TEST_CASE("a newline breaks the line even with no wrapping", "[text_wrap]") {
    // |n is WoW's spelling of one and the markup parser turns it into this, so
    // a label with an explicit break gets one whether or not anything wraps.
    const auto out = lines("first\nsecond", 0.0f);
    REQUIRE(out.size() == 2);
    REQUIRE(out[0] == "first");
    REQUIRE(out[1] == "second");
}

TEST_CASE("two newlines leave a blank line between", "[text_wrap]") {
    // INSTANCE_LOCK_SEPARATOR is "%s|n|n%s", which is a paragraph break.
    const auto out = lines("a\n\nb", 0.0f);
    REQUIRE(out.size() == 3);
    REQUIRE(out[0] == "a");
    REQUIRE(out[1].empty());
    REQUIRE(out[2] == "b");
}

TEST_CASE("a hard break wins over a soft one", "[text_wrap]") {
    // The word before the break must not be carried onto the wrapped line.
    const auto out = lines("ab\ncd ef gh", 5.0f);
    REQUIRE(out.size() >= 2);
    REQUIRE(out[0] == "ab");
    REQUIRE(out[1].substr(0, 2) == "cd");
}

TEST_CASE("a word is not carried across a hard break", "[text_wrap]") {
    const auto out = lines("alpha\nbeta", 40.0f);
    REQUIRE(out.size() == 2);
    REQUIRE(out[0] == "alpha");
    REQUIRE(out[1] == "beta");
}

TEST_CASE("an empty string produces one empty line", "[text_wrap]") {
    const auto out = wrapText(plain(""), 10.0f, false, charWidth);
    REQUIRE(out.size() == 1);
    REQUIRE(joined(out[0]).empty());
}

// ---------------------------------------------------------------------------
// A label too long for its box is cut at the end with "...", as WoW cuts it.
// Before this it was justified as if it fitted: a right-aligned one started
// out past the left of its box and lost its first characters to the clip, so
// the video options' resolution read "920x1080 (Wide)".

using wowee::ui::fitWithEllipsis;

TEST_CASE("text that fits is left alone", "[wrap][ellipsis]") {
    CHECK(fitWithEllipsis("1920x1080", 9.0f, charWidth) == "1920x1080");
    CHECK(fitWithEllipsis("", 0.0f, charWidth).empty());
}

TEST_CASE("text that does not fit keeps its start and loses its end", "[wrap][ellipsis]") {
    // Sixteen characters in fifteen: the dots take three, so twelve remain.
    const std::string cut = fitWithEllipsis("1920x1080 (Wide)", 15.0f, charWidth);
    CHECK(cut == "1920x1080 (W...");
    CHECK(charWidth(cut) <= 15.0f);
    CHECK(cut.rfind("1920", 0) == 0);
}

TEST_CASE("no space is left hanging before the dots", "[wrap][ellipsis]") {
    // "Long " would be cut to fit six, and the space before the dots dropped.
    CHECK(fitWithEllipsis("Long quest name", 8.0f, charWidth) == "Long...");
}

TEST_CASE("a character is never cut in half", "[wrap][ellipsis]") {
    // "é" is two bytes. Measured in bytes here, so the cut has to back up over
    // the continuation byte rather than land between the two.
    const std::string cut = fitWithEllipsis("caf\xC3\xA9 au lait", 7.0f, charWidth);
    CHECK(cut == "caf...");
    const std::string whole = fitWithEllipsis("caf\xC3\xA9 au lait", 8.0f, charWidth);
    CHECK(whole == "caf\xC3\xA9...");
}

TEST_CASE("a box too small for the dots gets the dots", "[wrap][ellipsis]") {
    CHECK(fitWithEllipsis("Anything", 2.0f, charWidth) == "...");
}

TEST_CASE("Lines start where 2.4.3 starts them", "[wrap][place]") {
    using wowee::ui::placeTextLine;
    using wowee::ui::textOriginY;
    // White strings on black, FRIZQT 12 on a 720-pixel window (FP_Place in
    // _experiments/20261119T092759Z.questlog_fonts): "T" (width 7) and "o"
    // (width 8) in a 60-pixel box at x = 380 and 460.
    CHECK(placeTextLine(32.0f, 0.0f, 7.0f, 0.0f, "LEFT") == 34.0f);
    CHECK(placeTextLine(100.5f, 0.0f, 7.0f, 0.0f, "LEFT") == 102.0f);
    CHECK(placeTextLine(240.75f, 0.0f, 7.0f, 0.0f, "LEFT") == 242.0f);
    CHECK(placeTextLine(380.0f, 60.0f, 7.0f, 0.0f, "CENTER") == 408.0f);
    CHECK(placeTextLine(380.0f, 60.0f, 8.0f, 1.0f, "CENTER") == 407.0f);
    CHECK(placeTextLine(460.0f, 60.0f, 7.0f, 0.0f, "RIGHT") == 514.0f);
    CHECK(placeTextLine(460.0f, 60.0f, 8.0f, 0.0f, "RIGHT") == 513.0f);
    CHECK(textOriginY(10.0f) == 10.0f);
    CHECK(textOriginY(10.25f) == 11.0f);
    CHECK(textOriginY(10.75f) == 11.0f);
}

TEST_CASE("Lines start where 2.4.3 starts them in boxes off the pixel grid", "[wrap][place]") {
    // "HHHH" (GameFontNormal, 11 px, width 36, shadow 1 or 0) at all sixteen
    // 1/16-pixel lefts in boxes 50 to 51.75 pixels wide: the pen under the
    // first strong ink column (CT() in _experiments/20261119T0935Z.raidinfo_clip,
    // shots/original-ct1w.png). Edges on an exact pixel are left out: native
    // float error decides those.
    struct Case { const char* justify; float left, box, shadow; int pen; };
    const Case cases[] = {
        {"CENTER", 80.0625f, 50.0f, 1.0f, 88},
        {"CENTER", 140.125f, 50.0f, 1.0f, 148},
        {"CENTER", 200.1875f, 50.0f, 1.0f, 208},
        {"CENTER", 260.25f, 50.0f, 1.0f, 268},
        {"CENTER", 320.3125f, 50.0f, 1.0f, 328},
        {"CENTER", 380.375f, 50.0f, 1.0f, 388},
        {"CENTER", 440.4375f, 50.0f, 1.0f, 448},
        {"CENTER", 500.5f, 50.0f, 1.0f, 508},
        {"CENTER", 560.5625f, 50.0f, 1.0f, 568},
        {"CENTER", 620.625f, 50.0f, 1.0f, 628},
        {"CENTER", 680.6875f, 50.0f, 1.0f, 688},
        {"CENTER", 740.75f, 50.0f, 1.0f, 748},
        {"CENTER", 800.8125f, 50.0f, 1.0f, 808},
        {"CENTER", 860.875f, 50.0f, 1.0f, 868},
        {"CENTER", 920.9375f, 50.0f, 1.0f, 928},
        {"CENTER", 80.0625f, 50.0f, 0.0f, 89},
        {"CENTER", 140.125f, 50.0f, 0.0f, 149},
        {"CENTER", 200.1875f, 50.0f, 0.0f, 209},
        {"CENTER", 260.25f, 50.0f, 0.0f, 269},
        {"CENTER", 320.3125f, 50.0f, 0.0f, 329},
        {"CENTER", 380.375f, 50.0f, 0.0f, 389},
        {"CENTER", 440.4375f, 50.0f, 0.0f, 449},
        {"CENTER", 500.5f, 50.0f, 0.0f, 509},
        {"CENTER", 560.5625f, 50.0f, 0.0f, 569},
        {"CENTER", 620.625f, 50.0f, 0.0f, 629},
        {"CENTER", 680.6875f, 50.0f, 0.0f, 689},
        {"CENTER", 740.75f, 50.0f, 0.0f, 749},
        {"CENTER", 800.8125f, 50.0f, 0.0f, 809},
        {"CENTER", 860.875f, 50.0f, 0.0f, 869},
        {"CENTER", 920.9375f, 50.0f, 0.0f, 929},
        {"CENTER", 20.0f, 50.5f, 1.0f, 28},
        {"CENTER", 80.0625f, 50.5f, 1.0f, 88},
        {"CENTER", 140.125f, 50.5f, 1.0f, 148},
        {"CENTER", 200.1875f, 50.5f, 1.0f, 208},
        {"CENTER", 260.25f, 50.5f, 1.0f, 268},
        {"CENTER", 320.3125f, 50.5f, 1.0f, 328},
        {"CENTER", 380.375f, 50.5f, 1.0f, 388},
        {"CENTER", 440.4375f, 50.5f, 1.0f, 448},
        {"CENTER", 500.5f, 50.5f, 1.0f, 508},
        {"CENTER", 560.5625f, 50.5f, 1.0f, 568},
        {"CENTER", 620.625f, 50.5f, 1.0f, 628},
        {"CENTER", 680.6875f, 50.5f, 1.0f, 688},
        {"CENTER", 800.8125f, 50.5f, 1.0f, 809},
        {"CENTER", 860.875f, 50.5f, 1.0f, 869},
        {"CENTER", 920.9375f, 50.5f, 1.0f, 929},
        {"CENTER", 20.0f, 50.5f, 0.0f, 29},
        {"CENTER", 80.0625f, 50.5f, 0.0f, 89},
        {"CENTER", 140.125f, 50.5f, 0.0f, 149},
        {"CENTER", 200.1875f, 50.5f, 0.0f, 209},
        {"CENTER", 260.25f, 50.5f, 0.0f, 269},
        {"CENTER", 320.3125f, 50.5f, 0.0f, 329},
        {"CENTER", 380.375f, 50.5f, 0.0f, 389},
        {"CENTER", 440.4375f, 50.5f, 0.0f, 449},
        {"CENTER", 500.5f, 50.5f, 0.0f, 509},
        {"CENTER", 560.5625f, 50.5f, 0.0f, 569},
        {"CENTER", 620.625f, 50.5f, 0.0f, 629},
        {"CENTER", 680.6875f, 50.5f, 0.0f, 689},
        {"CENTER", 800.8125f, 50.5f, 0.0f, 810},
        {"CENTER", 860.875f, 50.5f, 0.0f, 870},
        {"CENTER", 920.9375f, 50.5f, 0.0f, 930},
        {"CENTER", 20.0f, 51.25f, 1.0f, 28},
        {"CENTER", 80.0625f, 51.25f, 1.0f, 88},
        {"CENTER", 140.125f, 51.25f, 1.0f, 148},
        {"CENTER", 200.1875f, 51.25f, 1.0f, 208},
        {"CENTER", 260.25f, 51.25f, 1.0f, 268},
        {"CENTER", 320.3125f, 51.25f, 1.0f, 328},
        {"CENTER", 440.4375f, 51.25f, 1.0f, 449},
        {"CENTER", 500.5f, 51.25f, 1.0f, 509},
        {"CENTER", 560.5625f, 51.25f, 1.0f, 569},
        {"CENTER", 620.625f, 51.25f, 1.0f, 629},
        {"CENTER", 680.6875f, 51.25f, 1.0f, 689},
        {"CENTER", 740.75f, 51.25f, 1.0f, 749},
        {"CENTER", 800.8125f, 51.25f, 1.0f, 809},
        {"CENTER", 860.875f, 51.25f, 1.0f, 869},
        {"CENTER", 920.9375f, 51.25f, 1.0f, 929},
        {"CENTER", 20.0f, 51.25f, 0.0f, 29},
        {"CENTER", 80.0625f, 51.25f, 0.0f, 89},
        {"CENTER", 140.125f, 51.25f, 0.0f, 149},
        {"CENTER", 200.1875f, 51.25f, 0.0f, 209},
        {"CENTER", 260.25f, 51.25f, 0.0f, 269},
        {"CENTER", 320.3125f, 51.25f, 0.0f, 329},
        {"CENTER", 440.4375f, 51.25f, 0.0f, 450},
        {"CENTER", 500.5f, 51.25f, 0.0f, 510},
        {"CENTER", 560.5625f, 51.25f, 0.0f, 570},
        {"CENTER", 620.625f, 51.25f, 0.0f, 630},
        {"CENTER", 680.6875f, 51.25f, 0.0f, 690},
        {"CENTER", 740.75f, 51.25f, 0.0f, 750},
        {"CENTER", 800.8125f, 51.25f, 0.0f, 810},
        {"CENTER", 860.875f, 51.25f, 0.0f, 870},
        {"CENTER", 920.9375f, 51.25f, 0.0f, 930},
        {"CENTER", 20.0f, 51.75f, 1.0f, 28},
        {"CENTER", 80.0625f, 51.75f, 1.0f, 88},
        {"CENTER", 200.1875f, 51.75f, 1.0f, 209},
        {"CENTER", 260.25f, 51.75f, 1.0f, 269},
        {"CENTER", 320.3125f, 51.75f, 1.0f, 329},
        {"CENTER", 380.375f, 51.75f, 1.0f, 389},
        {"CENTER", 440.4375f, 51.75f, 1.0f, 449},
        {"CENTER", 500.5f, 51.75f, 1.0f, 509},
        {"CENTER", 560.5625f, 51.75f, 1.0f, 569},
        {"CENTER", 620.625f, 51.75f, 1.0f, 629},
        {"CENTER", 680.6875f, 51.75f, 1.0f, 689},
        {"CENTER", 740.75f, 51.75f, 1.0f, 749},
        {"CENTER", 800.8125f, 51.75f, 1.0f, 809},
        {"CENTER", 860.875f, 51.75f, 1.0f, 869},
        {"CENTER", 920.9375f, 51.75f, 1.0f, 929},
        {"CENTER", 20.0f, 51.75f, 0.0f, 29},
        {"CENTER", 80.0625f, 51.75f, 0.0f, 89},
        {"CENTER", 200.1875f, 51.75f, 0.0f, 210},
        {"CENTER", 260.25f, 51.75f, 0.0f, 270},
        {"CENTER", 320.3125f, 51.75f, 0.0f, 330},
        {"CENTER", 380.375f, 51.75f, 0.0f, 390},
        {"CENTER", 440.4375f, 51.75f, 0.0f, 450},
        {"CENTER", 500.5f, 51.75f, 0.0f, 510},
        {"CENTER", 560.5625f, 51.75f, 0.0f, 570},
        {"CENTER", 620.625f, 51.75f, 0.0f, 630},
        {"CENTER", 680.6875f, 51.75f, 0.0f, 690},
        {"CENTER", 740.75f, 51.75f, 0.0f, 750},
        {"CENTER", 800.8125f, 51.75f, 0.0f, 810},
        {"CENTER", 860.875f, 51.75f, 0.0f, 870},
        {"CENTER", 920.9375f, 51.75f, 0.0f, 930},
        {"LEFT", 20.0f, 50.0f, 1.0f, 22},
        {"LEFT", 80.0625f, 50.0f, 1.0f, 82},
        {"LEFT", 140.125f, 50.0f, 1.0f, 142},
        {"LEFT", 200.1875f, 50.0f, 1.0f, 202},
        {"LEFT", 260.25f, 50.0f, 1.0f, 262},
        {"LEFT", 320.3125f, 50.0f, 1.0f, 322},
        {"LEFT", 380.375f, 50.0f, 1.0f, 382},
        {"LEFT", 440.4375f, 50.0f, 1.0f, 442},
        {"LEFT", 500.5f, 50.0f, 1.0f, 502},
        {"LEFT", 560.5625f, 50.0f, 1.0f, 562},
        {"LEFT", 620.625f, 50.0f, 1.0f, 622},
        {"LEFT", 680.6875f, 50.0f, 1.0f, 682},
        {"LEFT", 740.75f, 50.0f, 1.0f, 742},
        {"LEFT", 800.8125f, 50.0f, 1.0f, 802},
        {"LEFT", 860.875f, 50.0f, 1.0f, 862},
        {"LEFT", 920.9375f, 50.0f, 1.0f, 922},
        {"LEFT", 20.0f, 50.0f, 0.0f, 22},
        {"LEFT", 80.0625f, 50.0f, 0.0f, 82},
        {"LEFT", 140.125f, 50.0f, 0.0f, 142},
        {"LEFT", 200.1875f, 50.0f, 0.0f, 202},
        {"LEFT", 260.25f, 50.0f, 0.0f, 262},
        {"LEFT", 320.3125f, 50.0f, 0.0f, 322},
        {"LEFT", 380.375f, 50.0f, 0.0f, 382},
        {"LEFT", 440.4375f, 50.0f, 0.0f, 442},
        {"LEFT", 500.5f, 50.0f, 0.0f, 502},
        {"LEFT", 560.5625f, 50.0f, 0.0f, 562},
        {"LEFT", 620.625f, 50.0f, 0.0f, 622},
        {"LEFT", 680.6875f, 50.0f, 0.0f, 682},
        {"LEFT", 740.75f, 50.0f, 0.0f, 742},
        {"LEFT", 800.8125f, 50.0f, 0.0f, 802},
        {"LEFT", 860.875f, 50.0f, 0.0f, 862},
        {"LEFT", 920.9375f, 50.0f, 0.0f, 922},
        {"RIGHT", 20.0f, 50.5f, 1.0f, 35},
        {"RIGHT", 80.0625f, 50.5f, 1.0f, 95},
        {"RIGHT", 140.125f, 50.5f, 1.0f, 155},
        {"RIGHT", 200.1875f, 50.5f, 1.0f, 215},
        {"RIGHT", 260.25f, 50.5f, 1.0f, 275},
        {"RIGHT", 320.3125f, 50.5f, 1.0f, 335},
        {"RIGHT", 380.375f, 50.5f, 1.0f, 395},
        {"RIGHT", 440.4375f, 50.5f, 1.0f, 455},
        {"RIGHT", 560.5625f, 50.5f, 1.0f, 576},
        {"RIGHT", 620.625f, 50.5f, 1.0f, 636},
        {"RIGHT", 680.6875f, 50.5f, 1.0f, 696},
        {"RIGHT", 740.75f, 50.5f, 1.0f, 756},
        {"RIGHT", 800.8125f, 50.5f, 1.0f, 816},
        {"RIGHT", 860.875f, 50.5f, 1.0f, 876},
        {"RIGHT", 920.9375f, 50.5f, 1.0f, 936},
        {"RIGHT", 20.0f, 50.5f, 0.0f, 36},
        {"RIGHT", 80.0625f, 50.5f, 0.0f, 96},
        {"RIGHT", 140.125f, 50.5f, 0.0f, 156},
        {"RIGHT", 200.1875f, 50.5f, 0.0f, 216},
        {"RIGHT", 260.25f, 50.5f, 0.0f, 276},
        {"RIGHT", 320.3125f, 50.5f, 0.0f, 336},
        {"RIGHT", 380.375f, 50.5f, 0.0f, 396},
        {"RIGHT", 440.4375f, 50.5f, 0.0f, 456},
        {"RIGHT", 560.5625f, 50.5f, 0.0f, 577},
        {"RIGHT", 620.625f, 50.5f, 0.0f, 637},
        {"RIGHT", 680.6875f, 50.5f, 0.0f, 697},
        {"RIGHT", 740.75f, 50.5f, 0.0f, 757},
        {"RIGHT", 800.8125f, 50.5f, 0.0f, 817},
        {"RIGHT", 860.875f, 50.5f, 0.0f, 877},
        {"RIGHT", 920.9375f, 50.5f, 0.0f, 937},
    };
    for (const Case& c : cases) {
        CAPTURE(c.justify, c.left, c.box, c.shadow);
        CHECK(wowee::ui::placeTextLine(c.left, c.box, 36.0f, c.shadow, c.justify) == static_cast<float>(c.pen));
    }
}

TEST_CASE("a final break opens no line of its own", "[text-wrap]") {
    // TradeSkillReagentLabel says "Reagents: |n"; 2.4.3 sizes it one line high.
    using wowee::ui::textLineCount;
    CHECK(textLineCount(wrapText(plain("Reagents: \n"), 0.0f, false, charWidth)) == 1);
    CHECK(textLineCount(wrapText(plain("a\nb"), 0.0f, false, charWidth)) == 2);
    CHECK(textLineCount(wrapText(plain("a\n\n"), 0.0f, false, charWidth)) == 2);
}
