// edit_caret.hpp - Visual line layout and (x, y) -> byte-offset caret hit-testing
// for single-line and multi-line EditBox widgets.
//
// Header-only and decoupled from ImGui/SDL/Lua so it can be unit-tested directly
// with synthetic glyph metrics and shared identically between mouse hit-testing,
// vertical arrow navigation, OnCursorChanged reporting, and caret/selection rendering.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "ui/text_wrap.hpp"

namespace wowee::ui {

/// One visual atom on a laid-out EditBox line, mapped back to its raw byte span
/// in `Widget::editText`.
struct EditVisualCell {
    size_t rawStart = 0;
    size_t rawEnd = 0;
    float  x0 = 0.0f;
    float  x1 = 0.0f;
};

/// One visual line of an EditBox after explicit `\n`/`|n` breaks and optional
/// word wrapping.
struct EditVisualLine {
    /// Raw byte offset at the start of this visual line.
    size_t rawStart = 0;
    /// Raw byte offset at the end of this visual line (before any `\n` / `|n`).
    size_t rawEnd = 0;
    std::vector<EditVisualCell> cells;
    float width = 0.0f;
};

/// Visual position of a caret byte offset within a laid-out EditBox.
struct EditCaretPos {
    size_t lineIdx = 0;
    float  x = 0.0f;
};

/// Highlight rectangle on a single visual line for a text selection.
struct EditSelectionSpan {
    size_t lineIdx = 0;
    float  x0 = 0.0f;
    float  x1 = 0.0f;
};

namespace detail {

inline uint32_t decodeUtf8At(std::string_view text, size_t& at) {
    const unsigned char c = static_cast<unsigned char>(text[at]);
    int len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
    if (at + static_cast<size_t>(len) > text.size()) len = 1;
    uint32_t cp = c;
    if (len > 1) {
        cp = c & (0x7Fu >> len);
        for (int i = 1; i < len; ++i) {
            const unsigned char cc = static_cast<unsigned char>(text[at + static_cast<size_t>(i)]);
            if ((cc & 0xC0) != 0x80) {
                len = 1;
                cp = c;
                break;
            }
            cp = (cp << 6) | (cc & 0x3Fu);
        }
    }
    at += static_cast<size_t>(len);
    return cp;
}

template <typename Measure>
float measurePlainDisplay(std::string_view display, const Measure& m) {
    std::vector<uint32_t> cps;
    size_t at = 0;
    while (at < display.size()) {
        if (display[at] == '|' && at + 1 < display.size()) {
            const char tag = display[at + 1];
            if (tag == 'c' || tag == 'C') {
                at = std::min(at + 10, display.size());
                continue;
            }
            if (tag == 'r' || tag == 'R') {
                at += 2;
                continue;
            }
            if (tag == '|') {
                cps.push_back('|');
                at += 2;
                continue;
            }
        }
        cps.push_back(decodeUtf8At(display, at));
    }
    float sum = 0.0f;
    for (size_t i = 0; i < cps.size(); ++i) {
        const uint32_t next = (i + 1 < cps.size()) ? cps[i + 1] : 0u;
        sum += static_cast<float>(m.pen(cps[i], next));
    }
    return sum;
}

}  // namespace detail

/// Lay out `rawText` into visual lines matching `wrapTextNative`, preserving
/// exact raw byte spans for every codepoint, hyperlink, texture escape, and
/// line break. `wrapPx <= 0` disables soft wrapping (single-line mode).
template <typename Measure, typename TextureWidth>
std::vector<EditVisualLine> layoutEditLines(std::string_view rawText, float wrapPx,
                                            const Measure& m, TextureWidth textureWidth) {
    struct Atom {
        size_t   rawStart = 0;
        size_t   rawEnd = 0;
        uint32_t cp = 0;        // 0 for atomic link/texture
        float    atomicW = 0.0f;
        bool     isNewline = false;
    };

    std::vector<Atom> atoms;
    atoms.reserve(rawText.size());
    size_t i = 0;
    while (i < rawText.size()) {
        if (rawText[i] == '\n') {
            atoms.push_back({i, i + 1, '\n', 0.0f, true});
            ++i;
            continue;
        }
        if (rawText[i] == '|' && i + 1 < rawText.size()) {
            const char tag = rawText[i + 1];
            if (tag == 'n') {
                atoms.push_back({i, i + 2, '\n', 0.0f, true});
                i += 2;
                continue;
            }
            if (tag == '|') {
                atoms.push_back({i, i + 2, '|', 0.0f, false});
                i += 2;
                continue;
            }
            if (tag == 'c' || tag == 'C') {
                i = std::min(i + 10, rawText.size());
                continue;
            }
            if (tag == 'r' || tag == 'R' || tag == 'h' || tag == 't') {
                i += 2;
                continue;
            }
            if (tag == 'H') {
                const size_t payloadEnd = rawText.find("|h", i + 2);
                const size_t close = (payloadEnd == std::string_view::npos)
                    ? std::string_view::npos
                    : rawText.find("|h", payloadEnd + 2);
                const size_t end = (close == std::string_view::npos) ? rawText.size() : close + 2;
                std::string_view display;
                if (payloadEnd != std::string_view::npos) {
                    const size_t dStart = payloadEnd + 2;
                    const size_t dEnd = (close == std::string_view::npos) ? rawText.size() : close;
                    if (dEnd > dStart) display = rawText.substr(dStart, dEnd - dStart);
                }
                const float w = detail::measurePlainDisplay(display, m);
                atoms.push_back({i, end, 0u, w, false});
                i = end;
                continue;
            }
            if (tag == 'T') {
                const size_t close = rawText.find("|t", i + 2);
                const size_t end = (close == std::string_view::npos) ? rawText.size() : close + 2;
                const size_t stop = (close == std::string_view::npos) ? rawText.size() : close;
                const std::string_view payload = rawText.substr(i + 2, stop - (i + 2));
                const size_t firstColon = payload.find(':');
                WrapRun tex;
                tex.texture = std::string(payload.substr(0, firstColon));
                if (firstColon != std::string_view::npos) {
                    size_t at = firstColon + 1;
                    float* fields[2] = {&tex.texHeight, &tex.texWidth};
                    for (float* field : fields) {
                        if (at > payload.size()) break;
                        const size_t next = payload.find(':', at);
                        const std::string part(payload.substr(
                            at, next == std::string_view::npos ? std::string_view::npos : next - at));
                        *field = static_cast<float>(std::atof(part.c_str()));
                        if (next == std::string_view::npos) break;
                        at = next + 1;
                    }
                }
                const float w = tex.texture.empty() ? 0.0f : textureWidth(tex);
                atoms.push_back({i, end, 0u, w, false});
                i = end;
                continue;
            }
        }
        const size_t b0 = i;
        const uint32_t cp = detail::decodeUtf8At(rawText, i);
        atoms.push_back({b0, i, cp, 0.0f, false});
    }

    std::vector<EditVisualLine> lines;
    size_t p0 = 0;
    size_t paraRawStart = 0;
    while (true) {
        size_t p1 = p0;
        while (p1 < atoms.size() && !atoms[p1].isNewline) ++p1;

        std::vector<float> P(p1 - p0 + 1, 0.0f);
        const auto cpAt = [&](size_t idx) -> uint32_t {
            return (idx < p1) ? atoms[idx].cp : 0u;
        };
        const auto advanceOf = [&](size_t idx) -> float {
            if (atoms[idx].cp == 0u) return atoms[idx].atomicW;
            return static_cast<float>(m.pen(atoms[idx].cp, cpAt(idx + 1)));
        };
        const auto extentOf = [&](size_t idx) -> float {
            if (atoms[idx].cp == 0u) return atoms[idx].atomicW;
            return static_cast<float>(m.extent(atoms[idx].cp));
        };
        for (size_t idx = p0; idx < p1; ++idx) {
            P[idx - p0 + 1] = P[idx - p0] + advanceOf(idx);
        }
        const auto pos = [&](size_t idx) { return P[idx - p0]; };
        const auto finalWidth = [&](size_t a, size_t b) {
            return b > a ? pos(b - 1) - pos(a) + extentOf(b - 1) : 0.0f;
        };

        const auto emit = [&](size_t a, size_t b, float width) {
            EditVisualLine line;
            line.width = width;
            if (a == b) {
                line.rawStart = paraRawStart;
                line.rawEnd = paraRawStart;
            } else {
                line.rawStart = atoms[a].rawStart;
                line.rawEnd = atoms[b - 1].rawEnd;
                line.cells.reserve(b - a);
                const float base = pos(a);
                for (size_t idx = a; idx < b; ++idx) {
                    line.cells.push_back({
                        atoms[idx].rawStart,
                        atoms[idx].rawEnd,
                        pos(idx) - base,
                        pos(idx + 1) - base,
                    });
                }
            }
            lines.push_back(std::move(line));
        };

        size_t ls = p0;
        size_t s = p0;
        while (s < p1) {
            size_t e = s;
            while (e < p1 && atoms[e].cp != ' ') {
                ++e;
                if (breaksAfter(atoms[e - 1].cp)) break;
            }
            const size_t ce = e;
            while (e < p1 && atoms[e].cp == ' ') ++e;
            const bool last = (e >= p1);
            const auto fits = [&](size_t a) {
                if (ce == a) return true;
                const float w = last ? finalWidth(a, ce) : (pos(ce) - pos(a));
                return wrapPx <= 0.0f || w <= wrapPx;
            };
            if (s > ls && !fits(ls)) {
                emit(ls, s, pos(s) - pos(ls));
                ls = s;
            }
            s = e;
        }
        emit(ls, p1, finalWidth(ls, p1));

        if (p1 >= atoms.size()) break;
        paraRawStart = atoms[p1].rawEnd;
        p0 = p1 + 1;
    }

    return lines;
}

/// Map a local pixel click `(localX, localY)` relative to the text origin to
/// the nearest valid caret byte offset in `rawText`.
/// - Above line 0 clamps to line 0.
/// - Below the last line (multiLine) returns the end of the text (`lines.back().rawEnd`).
/// - Within a line, snaps to the nearest glyph midpoint; past right end returns `line.rawEnd`
///   (right before `\n` on newline-terminated lines).
inline size_t hitTestEditCaret(const std::vector<EditVisualLine>& lines,
                               float localX, float localY,
                               float linePx, bool multiLine) {
    if (lines.empty()) return 0;
    size_t lineIdx = 0;
    if (multiLine && linePx > 0.0f) {
        if (localY < 0.0f) {
            lineIdx = 0;
        } else {
            const size_t row = static_cast<size_t>(std::floor(localY / linePx));
            if (row >= lines.size()) {
                return lines.back().rawEnd;
            }
            lineIdx = row;
        }
    }
    const EditVisualLine& line = lines[lineIdx];
    if (line.cells.empty()) return line.rawStart;
    for (const EditVisualCell& cell : line.cells) {
        const float mid = cell.x0 + (cell.x1 - cell.x0) * 0.5f;
        if (localX < mid) return cell.rawStart;
    }
    return line.rawEnd;
}

/// Locate the visual line index and pixel x-offset of a caret byte offset.
inline EditCaretPos locateEditCaret(const std::vector<EditVisualLine>& lines, size_t cursorPos) {
    if (lines.empty()) return {};
    for (size_t i = 0; i < lines.size(); ++i) {
        const EditVisualLine& line = lines[i];
        if (cursorPos < line.rawStart) {
            return {i, 0.0f};
        }
        if (cursorPos <= line.rawEnd) {
            // Soft-wrap boundary: prefer the start of the wrapped continuation
            // line so clicking the left edge of line i+1 draws the caret there.
            if (cursorPos == line.rawEnd &&
                i + 1 < lines.size() &&
                cursorPos == lines[i + 1].rawStart) {
                continue;
            }
            for (const EditVisualCell& cell : line.cells) {
                if (cursorPos <= cell.rawStart || cursorPos < cell.rawEnd) {
                    return {i, cell.x0};
                }
            }
            const float endX = line.cells.empty() ? 0.0f : line.cells.back().x1;
            return {i, endX};
        }
    }
    const EditVisualLine& last = lines.back();
    return {lines.size() - 1, last.cells.empty() ? 0.0f : last.cells.back().x1};
}

/// Compute per-line highlight spans `[x0, x1]` for a selection `[selStart, selEnd)`.
inline std::vector<EditSelectionSpan> editSelectionSpans(const std::vector<EditVisualLine>& lines,
                                                         size_t selStart, size_t selEnd,
                                                         float newlineHighlightW = 4.0f) {
    std::vector<EditSelectionSpan> spans;
    if (selEnd <= selStart || lines.empty()) return spans;

    const auto xAt = [](const EditVisualLine& line, size_t rawByte) -> float {
        for (const EditVisualCell& cell : line.cells) {
            if (rawByte <= cell.rawStart || rawByte < cell.rawEnd) return cell.x0;
        }
        return line.cells.empty() ? 0.0f : line.cells.back().x1;
    };

    for (size_t i = 0; i < lines.size(); ++i) {
        const EditVisualLine& line = lines[i];
        const bool crossesNewline = (selEnd > line.rawEnd) && (i + 1 < lines.size()) &&
                                    (lines[i + 1].rawStart > line.rawEnd);
        if (selEnd <= line.rawStart) break;
        if (selStart > line.rawEnd) continue;
        if (selStart == line.rawEnd && !crossesNewline) continue;

        const size_t from = std::max(selStart, line.rawStart);
        const size_t to = std::min(selEnd, line.rawEnd);
        const float x0 = xAt(line, from);
        float x1 = xAt(line, to);
        if (crossesNewline) x1 += newlineHighlightW;
        if (x1 > x0) spans.push_back({i, x0, x1});
    }
    return spans;
}

}  // namespace wowee::ui
