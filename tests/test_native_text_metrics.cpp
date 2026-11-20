// Text measured and wrapped the way the original 2.4.3 client does it.
//
// Every expected number here was read off the original client (build 8606)
// through GetStringWidth/GetHeight on FRIZQT__.TTF, on a 1280x720 window
// (0.9375 pixels per interface unit): GameTooltipText (12) is 11 pixels high,
// GameTooltipHeaderText (14) 13. Measuring addon and dumps:
// _experiments/20261117T193057Z.tooltip_text_wrap/ in the workspace.
//
// The face's tables come from a fixture of its font-unit data, because the
// font itself is not in the repository.
#include <catch_amalgamated.hpp>

#include <string>
#include <unordered_map>

#include "frizqt_metrics_fixture.hpp"
#include "ui/native_text_metrics.hpp"
#include "ui/native_pixel_rules.hpp"
#include "ui/text_markup.hpp"
#include "ui/text_wrap.hpp"

using namespace wowee::ui;

namespace {

const NativeFace& frizqt() {
    static const NativeFace face = [] {
        std::unordered_map<uint32_t, NativeGlyph> glyphs;
        for (const auto& g : frizqt_fixture::kGlyphs)
            glyphs[g.cp] = NativeGlyph{g.advance, g.lsb, g.xMax, g.outline, true};
        std::unordered_map<uint64_t, int> kerning;
        for (const auto& k : frizqt_fixture::kKerning)
            kerning[NativeFace::pairKey(k.left, k.right)] = k.value;
        return NativeFace(frizqt_fixture::kUnitsPerEm, std::move(glyphs), std::move(kerning));
    }();
    return face;
}

NativeTextMeasure at(int pixelHeight) {
    NativeTextMeasure m;
    m.face = &frizqt();
    m.pixelHeight = pixelHeight;
    return m;
}

std::vector<NativeWrappedLine> wrap(const std::string& text, float px, int h = 11) {
    const NativeTextMeasure m = at(h);
    return wrapTextNative(parseMarkup(text), px, false, m,
                          [](const WrapRun&) { return 0.0f; });
}

std::string lineText(const NativeWrappedLine& l) {
    std::string s;
    for (const auto& r : l.runs) s += r.text;
    return s;
}

const std::string kOrb =
    "Use: Permanently adds 15 spell critical strike rating and up to 12 spell damage to a "
    "shoulder slot item. Does not stack with other enchantments for the selected equipment slot.";
const std::string kFaith =
    "Use: Permanently adds 29 healing and 10 spell damage to a shoulder slot item. Does not "
    "stack with other enchantments for the selected equipment slot.";
const std::string kGreaterFaith =
    "Use: Permanently adds up to 33 healing and 11 spell damage and 4 mana per 5 sec. to a "
    "shoulder slot item. Does not stack with other enchantments for the selected equipment slot.";
const std::string kFerocity =
    "Use: Permanently adds 34 attack power and 16 hit rating to a head slot item. Does not "
    "stack with other enchantments for the selected equipment slot.";
const std::string kElementium =
    "\"Flawless. Indestructible. You cannot comprehend the craftsmanship involved with the "
    "creation of this ring.\"";
const std::string kDynamite =
    "Use: Inflicts 213 to 287 Fire damage in a 5 yard radius (Assuming that it gets to the "
    "target, some restrictions may apply.)";

}  // namespace

TEST_CASE("Font heights round to whole pixels", "[native-text]") {
    REQUIRE(nativePixelHeight(12.0f, 0.9375f) == 11);   // 11.25
    REQUIRE(nativePixelHeight(14.0f, 0.9375f) == 13);   // 13.125
    REQUIRE(nativePixelHeight(14.0f, 0.75f) == 11);     // 10.5 rounds up
    REQUIRE(nativePixelHeight(12.0f, 0.84375f) == 10);  // 10.125
    REQUIRE(nativePixelHeight(18.0f, 0.9375f) == 17);   // 16.875
}

TEST_CASE("String widths match the original's GetStringWidth", "[native-text]") {
    const auto m11 = at(11);
    REQUIRE(m11.width("Binds when picked up") == 119);
    REQUIRE(m11.width("Requires Level 70") == 98);
    REQUIRE(m11.width("Requires The Scryers - Exalted") == 162);
    REQUIRE(m11.width(kOrb) == 941);
    REQUIRE(m11.width("Does not stack with other enchantments") == 218);
    REQUIRE(at(13).width("Greater Inscription of the Orb") == 187);
    // Single glyphs, a space and digits ('1' is narrower than its advance).
    REQUIRE(m11.width(" ") == 3);
    REQUIRE(m11.width("1") == 6);
    REQUIRE(m11.width("11") == 14);
    REQUIRE(m11.width("iiiiiiiiiiiiiiiiiiii") == 61);
    // Negative kerning, truncated toward zero; positive kerning unused.
    REQUIRE(m11.width("Ta") == 12);
    REQUIRE(m11.width("LY") == 13);
    REQUIRE(m11.width("Yo") == 15);
    REQUIRE(m11.width("20 Energy") == 57);
    // Other heights.
    REQUIRE(at(9).width("Requires Level 70") == 81);
    REQUIRE(at(13).width("Requires The Scryers - Exalted") == 190);
    REQUIRE(at(10).width("Use: Permanently adds 29 healing and") == 188);
    REQUIRE(at(7).width("ABCDEFGHIJKLMNOPQRSTUVWXYZ") == 129);
}

TEST_CASE("A word fits while the pen after it is inside the width", "[native-text][wrap]") {
    // Bare FontString:SetWidth(w) thresholds read off the original.
    REQUIRE(wrap(kFaith, 216).size() == 5);
    REQUIRE(wrap(kFaith, 217).size() == 4);
    REQUIRE(wrap(kGreaterFaith, 215).size() == 6);
    REQUIRE(wrap(kGreaterFaith, 216).size() == 5);
    REQUIRE(wrap(kFerocity, 216).size() == 5);
    REQUIRE(wrap(kFerocity, 217).size() == 4);
    REQUIRE(wrap(kOrb, 201).size() == 6);
    REQUIRE(wrap(kOrb, 202).size() == 5);
    REQUIRE(wrap(kElementium, 207).size() == 4);
    REQUIRE(wrap(kElementium, 208).size() == 3);
    REQUIRE(wrap(kDynamite, 213).size() == 4);
    REQUIRE(wrap(kDynamite, 214).size() == 3);
}

TEST_CASE("Lines break where the original breaks them", "[native-text][wrap]") {
    const auto lines = wrap(kOrb, 216);
    REQUIRE(lines.size() == 5);
    CHECK(lineText(lines[0]) == "Use: Permanently adds 15 spell critical ");
    CHECK(lineText(lines[1]) == "strike rating and up to 12 spell damage ");
    CHECK(lineText(lines[2]) == "to a shoulder slot item. Does not stack ");
    CHECK(lineText(lines[3]) == "with other enchantments for the ");
    CHECK(lineText(lines[4]) == "selected equipment slot.");
    // Widths count the trailing space; the widest is what the tooltip grows to.
    CHECK(lines[1].width == 213.0f);
}

TEST_CASE("The last word of a text is held to its full width", "[native-text][wrap]") {
    // 'x' ends 1 pixel past its pen; pen 216 fits mid-text but not at the end.
    const std::string end = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxi xx";  // pen 216, width 217
    REQUIRE(at(11).width(end) == 217);
    REQUIRE(wrap(end, 216).size() == 2);
    REQUIRE(wrap(end, static_cast<float>(at(11).width(end))).size() == 1);
}

TEST_CASE("Hyphen, slash and colon allow a break; other punctuation does not",
          "[native-text][wrap]") {
    std::string slash;
    for (int i = 0; i < 30; ++i) slash += "abcdef/";
    slash += "x";
    const auto s = wrap(slash, 216);
    REQUIRE(s.size() == 7);
    CHECK(lineText(s[0]) == "abcdef/abcdef/abcdef/abcdef/abcdef/");
    CHECK(lineText(s[6]) == "x");

    std::string hyphen;
    for (int i = 0; i < 20; ++i) hyphen += "abc-def ";
    hyphen += "x";
    REQUIRE(wrap(hyphen, 216).size() == 5);

    std::string colon, comma;
    for (int i = 0; i < 12; ++i) { colon += "abcde:"; comma += "abcde,"; }
    REQUIRE(wrap(colon, 216).size() == 2);
    REQUIRE(wrap(comma, 216).size() == 1);  // one word: overflows its line
}

TEST_CASE("Spaces stay on the line they follow", "[native-text][wrap]") {
    const std::string spaces = "a" + std::string(200, ' ') + "b";
    const auto lines = wrap(spaces, 216);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].width == 606.0f);
    CHECK(lineText(lines[1]) == "b");
}

TEST_CASE("|n breaks at any width and colour runs survive a break", "[native-text][wrap]") {
    const auto lines = wrap("Reagents: |n|cffff2020Flash Powder|r", 0.0f);
    REQUIRE(lines.size() == 2);
    REQUIRE(lines[1].runs.size() == 1);
    CHECK(lines[1].runs[0].hasColor);
    CHECK(lines[1].runs[0].text == "Flash Powder");
}

// ── GameTooltip layout ──────────────────────────────────────────────────────

#include "ui/tooltip_layout.hpp"

namespace {

TooltipLayout tooltip(const std::vector<TooltipLineInput>& lines, float ppu = 0.9375f) {
    return layoutTooltip(lines, ppu, [](const std::string&, int h) { return at(h); });
}

TooltipLineInput title(const std::string& s) { return {s, "", false, 14.0f, ""}; }
TooltipLineInput plain(const std::string& s) { return {s, "", false, 12.0f, ""}; }
TooltipLineInput wrapped(const std::string& s) { return {s, "", true, 12.0f, ""}; }

}  // namespace

TEST_CASE("Greater Inscription of the Orb lays out as the original's", "[native-text][tooltip]") {
    const auto t = tooltip({title("Greater Inscription of the Orb"), plain("Binds when picked up"),
                            plain("Requires Level 70"), plain("Requires The Scryers - Exalted"),
                            wrapped(kOrb)});
    REQUIRE(t.lines[4].rows.size() == 5);
    CHECK(t.contentWidth == 213.0f);
    CHECK(t.widthUnits() == Catch::Approx(247.68f).margin(0.01f));
    CHECK(t.heightUnits() == Catch::Approx(136.213f).margin(0.01f));
}

TEST_CASE("A wrapped line grows the tooltip by its trailing space and rewraps",
          "[native-text][tooltip]") {
    // At 216 the second row ends "item. " at 218: the line is wrapped again at
    // 218, where "enchantments" (pen 217) joins the third row.
    const auto t = tooltip({title("Inscription of Faith"), plain("Binds when picked up"),
                            plain("Requires Level 64"), plain("Requires The Aldor - Honored"),
                            wrapped(kFaith)});
    REQUIRE(t.lines[4].rows.size() == 4);
    CHECK(t.contentWidth == 218.0f);
    CHECK(t.widthUnits() == Catch::Approx(253.01f).margin(0.01f));
    CHECK(t.heightUnits() == Catch::Approx(124.48f).margin(0.01f));
}

TEST_CASE("An unwrapped line at the default width wraps the rest at it", "[native-text][tooltip]") {
    // "Requires Cenarion Expedition - Revered" is exactly 216: wrapping happens
    // there and the trailing space of "power " does not widen anything.
    const auto t = tooltip({title("Glyph of Ferocity"), plain("Binds when picked up"),
                            plain("Requires Level 70"),
                            plain("Requires Cenarion Expedition - Revered"), wrapped(kFerocity)});
    REQUIRE(t.lines[4].rows.size() == 5);
    CHECK(t.contentWidth == 216.0f);
    // Without that line the same text grows to 217 and takes four rows.
    const auto u = tooltip({title("Glyph of Ferocity"), wrapped(kFerocity)});
    CHECK(u.lines[1].rows.size() == 4);
    CHECK(u.contentWidth == 217.0f);
}

TEST_CASE("Wrapped lines widen the tooltip one after another", "[native-text][tooltip]") {
    const auto t = tooltip({title("Neltharion's Tear"), plain("Binds when picked up"),
                            plain("Unique-Equipped"), plain("Trinket"), plain("Requires Level 60"),
                            wrapped("Equip: Increases damage and healing done by magical spells "
                                    "and effects by up to 44."),
                            wrapped("Equip: Increases your spell hit rating by 16."),
                            wrapped("\"A silver liquid flows within the impenetrable ebony shell. "
                                    "The item feels pure. It radiates an essence of extreme power.\"")});
    CHECK(t.lines[5].rows.size() == 3);
    CHECK(t.lines[6].rows.size() == 2);
    CHECK(t.lines[7].rows.size() == 4);
    CHECK(t.contentWidth == 212.0f);
    CHECK(t.heightUnits() == Catch::Approx(200.88f).margin(0.01f));
}

TEST_CASE("A wider unwrapped line added later still sets the wrap", "[native-text][tooltip]") {
    // AddLine(Faith text, wrap) then AddLine of 36 'x' (217 wide).
    const auto t = tooltip({title("T"), wrapped(kFaith), plain(std::string(36, 'x'))});
    CHECK(t.lines[1].rows.size() == 4);
    CHECK(t.contentWidth == 217.0f);
    const auto u = tooltip({title("T"), wrapped(kFaith), plain(std::string(48, 'x'))});
    CHECK(u.lines[1].rows.size() == 3);
}

TEST_CASE("Right text sits 38.4 units past the left", "[native-text][tooltip]") {
    TooltipLineInput dbl = plain(std::string(20, 'x'));
    dbl.right = std::string(10, 'x');
    const auto t = tooltip({title("T"), dbl});
    // 121 + 36 + 61 pixels.
    CHECK(t.contentWidth == 218.0f);
}

TEST_CASE("Tooltip scale changes pixel heights and the wrap", "[native-text][tooltip]") {
    // GameTooltip:SetScale(0.8): 9-pixel text, default wrap ceil(172.8) = 173.
    const auto t = tooltip({title("Inscription of Faith"), plain("Binds when picked up"),
                            plain("Requires Level 64"), plain("Requires The Aldor - Honored"),
                            wrapped(kFaith)}, 0.75f);
    CHECK(t.lines[0].pixelHeight == 11);
    CHECK(t.lines[4].pixelHeight == 9);
    CHECK(t.lines[4].rows.size() == 5);
    CHECK(t.contentWidth == 160.0f);
}

// ── Pixel rules (native_pixel_rules.hpp) ────────────────────────────────────
// Read off build 8606 at 1280x720 with a probe addon, one interface unit
// apart through a whole 15/16 cycle: _experiments/20261119T0935Z.raidinfo_clip
// (logs/sweep1.txt, textclipv.txt, textsweep2.txt) in the workspace.

TEST_CASE("A scroll frame keeps textures from its rounded edges, far ends included", "[native-pixels]") {
    struct Case { float x0, x1, y0, y1; int first, last, top, bottom; };
    const Case cases[] = {
        {120.0f, 240.0f, 120.0f, 180.0f, 120, 240, 120, 180},
        {120.9375f, 241.875f, 119.0625f, 180.0f, 121, 242, 119, 180},
        {121.875f, 243.75f, 118.125f, 180.0f, 122, 244, 118, 180},
        {122.8125f, 245.625f, 117.1875f, 180.0f, 123, 246, 117, 180},
        {123.75f, 247.5f, 116.25f, 180.0f, 124, 248, 116, 180},
        {124.6875f, 249.375f, 115.3125f, 180.0f, 125, 249, 115, 180},
        {125.625f, 251.25f, 114.375f, 180.0f, 126, 251, 114, 180},
        {126.5625f, 253.125f, 113.4375f, 180.0f, 127, 253, 113, 180},
        {127.5f, 255.0f, 112.5f, 180.0f, 128, 255, 113, 180},
        {128.4375f, 256.875f, 111.5625f, 180.0f, 128, 257, 112, 180},
        {129.375f, 258.75f, 110.625f, 180.0f, 129, 259, 111, 180},
        {130.3125f, 260.625f, 109.6875f, 180.0f, 130, 261, 110, 180},
        {131.25f, 262.5f, 108.75f, 180.0f, 131, 263, 109, 180},
        {132.1875f, 264.375f, 107.8125f, 180.0f, 132, 264, 108, 180},
        {133.125f, 266.25f, 106.875f, 180.0f, 133, 266, 107, 180},
        {134.0625f, 268.125f, 105.9375f, 180.0f, 134, 268, 106, 180},
    };
    for (const Case& c : cases) {
        const auto cols = wowee::ui::scrollClipTextureSpan(c.x0, c.x1);
        const auto rows = wowee::ui::scrollClipTextureSpan(c.y0, c.y1);
        CAPTURE(c.x0, c.x1, c.y0, c.y1);
        CHECK(cols.first == c.first);
        CHECK(cols.last == c.last);
        CHECK(rows.first == c.top);
        CHECK(rows.last == c.bottom);
    }
}

TEST_CASE("A scroll frame keeps text on even rows and on every column it touches", "[native-pixels]") {
    struct Edge { float edge; int row; };
    // The last row of 'IIIII' hanging past the bottom edge.
    const Edge bottoms[] = {
        {131.2500f, 132},
        {130.3125f, 132},
        {129.3750f, 130},
        {128.4375f, 130},
        {127.5000f, 128},
        {126.5625f, 128},
        {125.6250f, 126},
        {124.6875f, 126},
        {123.7500f, 124},
        {122.8125f, 124},
        {121.8750f, 122},
        {120.9375f, 122},
        {120.0000f, 120},
        {119.0625f, 120},
        {118.1250f, 120},
        {117.1875f, 118},
    };
    for (const Edge& e : bottoms) {
        CAPTURE(e.edge);
        CHECK(wowee::ui::scrollClipTextRows(0.0f, e.edge).last == e.row);
    }
    // The first row of 'IIIII' reaching above the top edge.
    const Edge tops[] = {
        {120.0000f, 120},
        {119.0625f, 120},
        {118.1250f, 118},
        {117.1875f, 118},
        {116.2500f, 116},
        {115.3125f, 116},
        {114.3750f, 114},
        {113.4375f, 114},
        {112.5000f, 112},
        {111.5625f, 112},
        {110.6250f, 110},
        {109.6875f, 110},
        {108.7500f, 108},
        {107.8125f, 108},
        {106.8750f, 106},
        {105.9375f, 106},
    };
    for (const Edge& e : tops) {
        CAPTURE(e.edge);
        CHECK(wowee::ui::scrollClipTextRows(e.edge, 400.0f).first == e.row);
    }
    // The raid information list: frame left 392 units, 367.5 pixels; native
    // draws the first column of "Resets in" at 367.
    CHECK(wowee::ui::scrollClipTextColumns(367.5f, 587.8125f).first == 367);
    CHECK(wowee::ui::scrollClipTextColumns(120.9375f, 158.4375f).first == 120);
    CHECK(wowee::ui::scrollClipTextColumns(125.625f, 163.125f).first == 125);
}

TEST_CASE("Outlined text widens extent and insets pen like 2.4.3", "[native-text][outline]") {
    CHECK(outlinePixelsForFlag("") == 0);
    CHECK(outlinePixelsForFlag("NORMAL") == 1);
    CHECK(outlinePixelsForFlag("OUTLINE") == 1);
    CHECK(outlinePixelsForFlag("THICK") == 2);
    CHECK(outlinePixelsForFlag("THICKOUTLINE") == 2);
    CHECK(outlinePixelsForFlag("OUTLINE, THICKOUTLINE, MONOCHROME") == 2);

    NativeTextMeasure plain = at(11);
    NativeTextMeasure normal = at(11);
    normal.outlinePixels = 1;
    NativeTextMeasure thick = at(11);
    thick.outlinePixels = 2;

    // OUTLINE adds +4 px to total width and +0 to per-glyph pen advance.
    CHECK(normal.width("1") == plain.width("1") + 4);
    CHECK(normal.width("11") == plain.width("11") + 4);
    CHECK(normal.width("222") == plain.width("222") + 4);
    CHECK(normal.pen('2', '2') == plain.pen('2', '2'));

    // THICKOUTLINE adds +8 px to total width and +1 px to each outlined glyph advance.
    CHECK(thick.width("1") == plain.width("1") + 8);
    CHECK(thick.width("11") == plain.width("11") + 9);
    CHECK(thick.width("222") == plain.width("222") + 10);
    CHECK(thick.pen('2', '2') == plain.pen('2', '2') + 1);

    // RIGHT-justified placement with outline pen inset matches build 8606:
    // plain 22 (12 px) vs OUTLINE (+4, +1 inset -> -3 px) vs THICKOUTLINE (+9, +2 inset -> -7 px).
    const float left = 215.625f;
    const float boxW = 56.25f;
    const float penPlain = placeTextLine(left, boxW, 12.0f, 0.0f, "RIGHT") + 0.0f;
    const float penOut   = placeTextLine(left, boxW, 16.0f, 0.0f, "RIGHT") + 1.0f;
    const float penThick = placeTextLine(left, boxW, 21.0f, 0.0f, "RIGHT") + 2.0f;
    CHECK(penPlain == 261.0f);
    CHECK(penOut   == 258.0f);
    CHECK(penThick == 254.0f);

    // LEFT and CENTER placement with outline pen inset matches build 8606.
    CHECK(placeTextLine(93.75f, boxW, 12.0f, 0.0f, "LEFT") + 0.0f == 95.0f);
    CHECK(placeTextLine(93.75f, boxW, 16.0f, 0.0f, "LEFT") + 1.0f == 96.0f);
    CHECK(placeTextLine(93.75f, boxW, 21.0f, 0.0f, "LEFT") + 2.0f == 97.0f);
    CHECK(placeTextLine(154.6875f, boxW, 12.0f, 0.0f, "CENTER") + 0.0f == 178.0f);
    CHECK(placeTextLine(154.6875f, boxW, 16.0f, 0.0f, "CENTER") + 1.0f == 177.0f);
    CHECK(placeTextLine(154.6875f, boxW, 21.0f, 0.0f, "CENTER") + 2.0f == 175.0f);
}
