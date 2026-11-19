// text_wrap.hpp - Breaking a run of text into lines that fit a width.
//
// Pulled out of the renderer so it can be tested without a font. The widths
// come from a callable rather than from ImFont, which is the only thing the
// drawing side has that a test does not.
//
// FontStrings never wrapped at all before this: every draw went through one
// AddText at an unbounded width, so a label given a size by its XML or by two
// anchors drew a single line straight out of its own frame.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wowee::ui {

/// A piece of text and whether it carries a colour of its own. The renderer's
/// markup parser produces these; the wrap works on them rather than on the
/// stripped string so a colour run split across two lines keeps its colour on
/// both.
struct WrapRun {
    std::string text;
    bool  hasColor = false;
    float rgba[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    /// The payload of the |H...|h this run sits inside - "item:3299" and the
    /// like - or empty when the run is not part of a link. Carried so a click
    /// has something to name; the display text alone cannot say what was
    /// clicked.
    std::string link;
    /// The whole escape the link was written as, colour included -
    /// "|cffa335ee|Hitem:1168:0:0:0:0:0:0:0|h[Skullflame Shield]|h|r". It is
    /// what OnHyperlinkClick hands FrameXML as its text: SetItemRef passes it
    /// on to HandleModifiedItemClick, so a Ctrl-click tries the item on and a
    /// Shift-click puts the link itself into the chat box.
    std::string hyperlink;

    /// An inline texture: the file named by a |Tpath:h:w:ox:oy|t escape.
    ///
    /// When this is set the run draws the picture and `text` is empty. Coin
    /// amounts are written this way - GOLD_AMOUNT_TEXTURE is "%d" followed by
    /// one of these - so a run of them is what a price is made of.
    std::string texture;
    /// Height and width from the escape, in interface units. Zero means "the
    /// height of the line", which is what every coin escape asks for.
    float texHeight = 0.0f;
    float texWidth = 0.0f;
};

/// How many lines a font string's text takes: a final break opens no line of
/// its own. 2.4.3 sizes TradeSkillReagentLabel, "Reagents: |n", one line high
/// (9.6 at its 12-unit font); counting the empty line after the break put the
/// reagent buttons anchored under it 12 units low.
template <typename Lines>
int textLineCount(const Lines& lines) {
    size_t n = lines.size();
    if (n > 1 && lines.back().empty()) --n;
    return static_cast<int>(n);
}

/// Whether two runs are the same style, so adjacent pieces can be merged back
/// into one rather than left as a string of one-word runs.
inline bool sameStyle(const WrapRun& a, const WrapRun& b) {
    // Two links are not one run even in the same colour: merging them would
    // leave a single rect covering both, and a click could not say which.
    if (a.link != b.link) return false;
    // A picture is never merged into a neighbouring run: it draws instead of
    // text rather than beside it, so a merge would lose one or the other.
    if (!a.texture.empty() || !b.texture.empty()) return false;
    if (a.hasColor != b.hasColor) return false;
    if (!a.hasColor) return true;
    return std::equal(std::begin(a.rgba), std::end(a.rgba), std::begin(b.rgba));
}

/// Break runs into lines no wider than wrapWidth.
///
/// `measure` answers the width of a string. A wrapWidth of zero means no soft
/// wrapping, which is what an auto-sized label wants - it is as wide as its own
/// text. A newline still breaks the line at any width: |n is WoW's spelling of
/// one, and a label with an explicit break gets one whether or not anything is
/// wrapping.
///
/// Words, not characters, unless a single word is wider than the whole box and
/// nonSpaceWrap says it may be broken. Thirty-six labels in FrameXML ask for
/// that, all of them prose in a narrow column.
template <typename Measure>
std::vector<std::vector<WrapRun>> wrapText(const std::vector<WrapRun>& runs,
                                           float wrapWidth, bool nonSpaceWrap,
                                           Measure measure, float continuationIndent = 0.0f) {
    std::vector<std::vector<WrapRun>> lines;
    lines.emplace_back();
    float x = 0.0f;
    auto place = [&](const WrapRun& style, const std::string& piece) {
        if (!lines.back().empty() && sameStyle(lines.back().back(), style)) {
            lines.back().back().text += piece;
        } else {
            WrapRun add = style;
            add.text = piece;
            lines.back().push_back(std::move(add));
        }
    };

    for (const WrapRun& run : runs) {
        // A picture is one indivisible piece. It carries no text, so the loop
        // below would skip it entirely and the coin beside a price would be
        // dropped between the parser and the screen.
        if (!run.texture.empty()) {
            // Zero means "as tall as the line", and the line's height is the
            // font's - which the measure callable is the only way to reach
            // from here.
            const float w = run.texWidth > 0.0f    ? run.texWidth
                          : run.texHeight > 0.0f   ? run.texHeight
                                                   : measure("W");
            if (wrapWidth > 0.0f && !lines.back().empty() && x + w > wrapWidth) {
                lines.emplace_back();
                x = continuationIndent;
            }
            lines.back().push_back(run);
            x += w;
            continue;
        }
        size_t at = 0;
        while (at < run.text.size()) {
            // A newline breaks the line whatever the width is. |n is WoW's
            // spelling of one and the markup parser turns it into this, so a
            // label with an explicit break gets one even when nothing is
            // wrapping.
            if (run.text[at] == '\n') {
                lines.emplace_back();
                x = continuationIndent;
                ++at;
                continue;
            }
            if (wrapWidth <= 0.0f) {
                // No soft wrapping: take everything up to the next hard break.
                const size_t stop = run.text.find('\n', at);
                place(run, run.text.substr(at, stop == std::string::npos
                                                   ? std::string::npos : stop - at));
                at = (stop == std::string::npos) ? run.text.size() : stop;
                continue;
            }
            // A word and the spaces that follow it, so a break falls between
            // words and the trailing space stays with the line above.
            size_t end = run.text.find_first_of(" \n", at);
            if (end == std::string::npos) {
                end = run.text.size();
            } else if (run.text[end] == '\n') {
                // Stop before it; the branch above takes the break itself.
            } else {
                while (end < run.text.size() && run.text[end] == ' ') ++end;
            }
            std::string word = run.text.substr(at, end - at);
            at = end;

            float w = measure(word);
            if (!lines.back().empty() && x + w > wrapWidth) {
                lines.emplace_back();
                x = continuationIndent;
                // The break stands in for the space that separated them.
                while (!word.empty() && word.front() == ' ') word.erase(0, 1);
                w = measure(word);
            }

            if (x + w > wrapWidth && nonSpaceWrap) {
                // One word wider than the whole box. Broken by character,
                // which is the only thing left and what the attribute asks for.
                std::string part;
                for (char c : word) {
                    if (!part.empty() && x + measure(part + c) > wrapWidth) {
                        place(run, part);
                        lines.emplace_back();
                        x = continuationIndent;
                        part.clear();
                    }
                    part += c;
                }
                if (!part.empty()) {
                    place(run, part);
                    x += measure(part);
                }
                continue;
            }

            place(run, word);
            x += w;
        }
    }
    return lines;
}

/// One line of a native wrap: its runs and its width in pixels.
struct NativeWrappedLine {
    std::vector<WrapRun> runs;
    /// Pixels from the line's start to where the next line's first glyph
    /// would have gone (trailing spaces included); for the text's last line,
    /// the width GetStringWidth reports.
    float width = 0.0f;
};

/// Characters after which the original may break a line, besides a space.
/// Measured: of the printable ASCII punctuation only these three break; a run
/// of any other non-space characters is one unbreakable word.
inline bool breaksAfter(uint32_t cp) { return cp == '-' || cp == '/' || cp == ':'; }

/// Break runs into lines the way the original 2.4.3 client does, in pixels.
///
/// `m` answers `int pen(cp, next)` (advance to the next glyph, kerning
/// included) and `int extent(cp)` (what the last glyph of a text adds);
/// `textureWidth(run)` answers an inline picture's width in pixels.
///
/// A word goes on the current line when the pen position after it - trailing
/// spaces excluded - is at most `wrapPx`; the paragraph's last word is held to
/// its full width instead. Spaces always stay on the line they follow, however
/// far past the edge. `|n` breaks at any width; `wrapPx <= 0` means no soft
/// wrapping. A single word wider than the line takes a line of its own, broken
/// by character only when `nonSpaceWrap` asks for it. Lines after the first
/// have `continuationIndent` pixels less room (a chat frame's indented wrap).
template <typename Measure, typename TextureWidth>
std::vector<NativeWrappedLine> wrapTextNative(const std::vector<WrapRun>& runs, float wrapPx,
                                              bool nonSpaceWrap, const Measure& m,
                                              TextureWidth textureWidth,
                                              float continuationIndent = 0.0f) {
    struct Cell {
        uint32_t cp = 0;     // 0 for a picture
        size_t run = 0;
        size_t b0 = 0, b1 = 0;
    };
    std::vector<Cell> cells;
    for (size_t r = 0; r < runs.size(); ++r) {
        if (!runs[r].texture.empty()) { cells.push_back({0, r, 0, 0}); continue; }
        const std::string& t = runs[r].text;
        size_t at = 0;
        while (at < t.size()) {
            const size_t b0 = at;
            // UTF-8 decode, invalid bytes as themselves.
            const unsigned char c = static_cast<unsigned char>(t[at]);
            int len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
            if (at + static_cast<size_t>(len) > t.size()) len = 1;
            uint32_t cp = c;
            if (len > 1) {
                cp = c & (0x7Fu >> len);
                for (int i = 1; i < len; ++i) {
                    const unsigned char cc = static_cast<unsigned char>(t[at + static_cast<size_t>(i)]);
                    if ((cc & 0xC0) != 0x80) { len = 1; cp = c; break; }
                    cp = (cp << 6) | (cc & 0x3Fu);
                }
            }
            at += static_cast<size_t>(len);
            cells.push_back({cp, r, b0, at});
        }
    }

    std::vector<NativeWrappedLine> lines;
    const auto emit = [&](size_t a, size_t b, float width) {
        NativeWrappedLine line;
        line.width = width;
        for (size_t i = a; i < b;) {
            const Cell& c = cells[i];
            if (c.cp == 0) { line.runs.push_back(runs[c.run]); ++i; continue; }
            size_t j = i;
            while (j + 1 < b && cells[j + 1].cp != 0 && cells[j + 1].run == c.run) ++j;
            WrapRun piece = runs[c.run];
            piece.text = runs[c.run].text.substr(c.b0, cells[j].b1 - c.b0);
            if (!line.runs.empty() && sameStyle(line.runs.back(), piece)) line.runs.back().text += piece.text;
            else line.runs.push_back(std::move(piece));
            i = j + 1;
        }
        lines.push_back(std::move(line));
    };

    size_t p0 = 0;
    while (true) {
        size_t p1 = p0;
        while (p1 < cells.size() && cells[p1].cp != '\n') ++p1;
        // Pen positions within the paragraph: P[j] is where glyph j starts.
        std::vector<float> P(p1 - p0 + 1, 0.0f);
        const auto cpAt = [&](size_t i) { return i < p1 ? cells[i].cp : 0u; };
        const auto advanceOf = [&](size_t i) -> float {
            if (cells[i].cp == 0) return textureWidth(runs[cells[i].run]);
            return static_cast<float>(m.pen(cells[i].cp, cpAt(i + 1)));
        };
        const auto extentOf = [&](size_t i) -> float {
            if (cells[i].cp == 0) return textureWidth(runs[cells[i].run]);
            return static_cast<float>(m.extent(cells[i].cp));
        };
        for (size_t i = p0; i < p1; ++i) P[i - p0 + 1] = P[i - p0] + advanceOf(i);
        const auto pos = [&](size_t i) { return P[i - p0]; };
        // Width of [a, b) ending the paragraph.
        const auto finalWidth = [&](size_t a, size_t b) {
            return b > a ? pos(b - 1) - pos(a) + extentOf(b - 1) : 0.0f;
        };

        size_t ls = p0;
        size_t s = p0;
        while (s < p1) {
            // A word: up to a space or just past a break character, then the
            // spaces that follow it.
            size_t e = s;
            while (e < p1 && cells[e].cp != ' ') {
                ++e;
                if (breaksAfter(cells[e - 1].cp)) break;
            }
            size_t ce = e;
            while (e < p1 && cells[e].cp == ' ') ++e;
            const bool last = e >= p1;
            const float room = wrapPx - (lines.empty() ? 0.0f : continuationIndent);
            const auto fits = [&](size_t a) {
                if (ce == a) return true;
                const float w = last ? finalWidth(a, ce) : pos(ce) - pos(a);
                return wrapPx <= 0.0f || w <= room;
            };
            if (s > ls && !fits(ls)) {
                emit(ls, s, pos(s) - pos(ls));
                ls = s;
            }
            if (s == ls && !fits(ls) && nonSpaceWrap) {
                // One word wider than the line: by character, as asked.
                for (size_t k = ls + 1; k < ce; ++k) {
                    if (pos(k + 1) - pos(ls) > wrapPx - (lines.empty() ? 0.0f : continuationIndent)) {
                        emit(ls, k, pos(k) - pos(ls));
                        ls = k;
                    }
                }
            }
            s = e;
        }
        emit(ls, p1, finalWidth(ls, p1));
        if (p1 >= cells.size()) break;
        p0 = p1 + 1;
    }
    return lines;
}

/// Where 2.4.3 starts a line of text in a box, in whole pixels. Measured with
/// white strings on black, faces FRIZQT and MORPHEUS at 6 to 28 pixels, frame
/// scales 1 and 0.5 (_experiments/20261119T092759Z.questlog_fonts, FP_Place/
/// FP_Place2/FP_Place3), and "HHHH" in boxes 50, 50.5, 51.25 and 51.75 pixels
/// wide at all sixteen 1/16-pixel lefts, shadow on and off
/// (_experiments/20261119T0935Z.raidinfo_clip, CT(); centerfit.py, rightfit.py):
///   LEFT    floor(left) + 2
///   CENTER  floor(left + boxWidth / 2) + 2 - ceil((width + shadow) / 2)
///   RIGHT   floor(left + boxWidth) + 1 - width    with a shadow
///           ceil(left + boxWidth) + 1 - width     without
/// `width` is the line's measured width (NativeWrappedLine::width), `shadow`
/// the pixels a drop shadow adds (shadowPixels); width + shadow is
/// GetStringWidth. A box sized to its own text is width + shadow wide, so
/// centred it lands on round(left) + 1 without a shadow's odd pixel.
/// Edges landing exactly on a whole pixel follow native float error (its
/// GetStringWidth reads 36.0000014 for 36 pixels) and can fall one pixel to
/// the left there. The glyphs then sit on these pens as FreeType places them.
inline float placeTextLine(float left, float boxWidth, float width, float shadow, const std::string& justify) {
    // A thousandth of a pixel of slack: rects come out of float layout, and a
    // box edge solved to 22.9999 is the pixel at 23.
    constexpr float kSlack = 1e-3f;
    if (justify == "CENTER")
        return std::floor(left + boxWidth * 0.5f + kSlack) + 2.0f - std::ceil((width + shadow) * 0.5f - kSlack);
    if (justify == "RIGHT") {
        const float right = left + boxWidth;
        return (shadow > 0.0f ? std::floor(right + kSlack) : std::ceil(right - kSlack)) + 1.0f - width;
    }
    return std::floor(left + kSlack) + 2.0f;
}

/// The pixel row a text box's top lands on: the next whole pixel down, as
/// 2.4.3 snaps it (a top at 10.25 draws as at 11).
inline float textOriginY(float top) { return std::ceil(top - 1e-3f); }

/// One line of plain text cut to fit a width, ending in "..." where it had to
/// be cut - what WoW does with a label too long for the width it was given.
///
/// Text that fits comes back as it is. Otherwise the longest start of it that
/// fits with the dots after it, cut between characters rather than inside one
/// (UTF-8), and without a space left hanging before the dots. When not even
/// the dots fit, the dots alone, and the clip takes what it must.
template <typename Measure>
std::string fitWithEllipsis(const std::string& text, float width, Measure measure) {
    if (measure(text) <= width) return text;
    static const std::string kDots = "...";
    std::size_t cut = text.size();
    while (cut > 0) {
        do {
            --cut;
        } while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80);
        std::string head = text.substr(0, cut);
        while (!head.empty() && head.back() == ' ') head.pop_back();
        if (head.empty()) break;
        if (measure(head + kDots) <= width) return head + kDots;
    }
    return kDots;
}

}  // namespace wowee::ui
