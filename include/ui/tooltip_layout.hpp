#pragma once

// How the original 2.4.3 client (build 8606) sizes a GameTooltip and breaks
// its wrapped lines, measured through GetWidth/GetHeight/GetStringWidth of the
// tooltip and its TextLeft/TextRight regions at five tooltip scales:
//
//  - Lines start 10 units in and 10 down (GameTooltipTemplate's anchors) and
//    sit 2 units apart (the template's -2 offsets). Each line is as tall as its
//    rows: a row is the font's pixel height, with nothing between rows.
//  - The box is the content plus 20.48 units on each axis.
//  - A line with right text is left + 38.4 units + right wide.
//  - Unwrapped lines set the content width W first, every one of them, in
//    whatever order they were added.
//  - Wrapped lines then go in order. When W already reaches the default wrap
//    width (230.4 units, as pixels rounded up) a line wraps at W. Otherwise it
//    is wrapped at the default first, and W becomes the widest of those rows,
//    trailing spaces included; the line is then wrapped again at that W.
//
// "Greater Inscription of the Orb" (item 28909) on a 720-pixel window: rows at
// 216 pixels, widest row 213, five rows; box 247.68 x 136.21 units.
// Evidence: _experiments/20261117T193057Z.tooltip_text_wrap/ in the workspace.

#include <cmath>
#include <functional>
#include <string>
#include <vector>

#include "ui/native_text_metrics.hpp"
#include "ui/text_markup.hpp"
#include "ui/text_wrap.hpp"

namespace wowee::ui {

/// From GameTooltipTemplate: TextLeft1 at (10, -10), each next line 2 below.
constexpr float kTooltipTextInset = 10.0f;
constexpr float kTooltipLineGap = 2.0f;
/// Measured: box minus content, both axes, at every scale tried.
constexpr float kTooltipPadding = 20.48f;
/// Measured: content width of an AddDoubleLine minus both string widths.
constexpr float kTooltipRightGap = 38.4f;
/// Measured: the width a wrapped line breaks at when nothing wider is there.
constexpr float kTooltipWrapWidth = 230.4f;

struct TooltipLineInput {
    std::string left, right;
    bool wrap = false;
    float fontHeight = 12.0f;
    std::string fontFace;
};

struct TooltipLineLayout {
    int pixelHeight = 0;
    std::vector<NativeWrappedLine> rows;   ///< the left text, row by row
    float rightWidth = 0.0f;               ///< pixels
    /// Units from the top of this line to the top of the next.
    float advanceUnits(float pixelsPerUnit) const {
        return static_cast<float>(rows.size()) * static_cast<float>(pixelHeight) / pixelsPerUnit +
               kTooltipLineGap;
    }
};

struct TooltipLayout {
    std::vector<TooltipLineLayout> lines;
    float contentWidth = 0.0f;  ///< pixels
    float pixelsPerUnit = 1.0f;

    float widthUnits() const { return contentWidth / pixelsPerUnit + kTooltipPadding; }
    float heightUnits() const {
        float h = kTooltipPadding;
        for (const auto& l : lines) h += l.advanceUnits(pixelsPerUnit);
        return lines.empty() ? h : h - kTooltipLineGap;
    }
};

/// Lays out a tooltip's lines. `measureFor(face, pixelHeight)` supplies the
/// original's measure for a line's face.
inline TooltipLayout layoutTooltip(
        const std::vector<TooltipLineInput>& in, float pixelsPerUnit,
        const std::function<NativeTextMeasure(const std::string&, int)>& measureFor) {
    TooltipLayout out;
    out.pixelsPerUnit = pixelsPerUnit;
    out.lines.resize(in.size());
    const auto textureWidth = [&](int pixelHeight) {
        return [pixelHeight, pixelsPerUnit](const WrapRun& run) {
            if (run.texWidth > 0.0f) return run.texWidth * pixelsPerUnit;
            if (run.texHeight > 0.0f) return run.texHeight * pixelsPerUnit;
            return static_cast<float>(pixelHeight);
        };
    };
    const float defaultWrap = std::ceil(kTooltipWrapWidth * pixelsPerUnit - 1e-3f);

    float W = 0.0f;
    std::vector<NativeTextMeasure> measures(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        const int h = nativePixelHeight(in[i].fontHeight, pixelsPerUnit);
        measures[i] = measureFor(in[i].fontFace, h);
        out.lines[i].pixelHeight = h;
        if (in[i].wrap) continue;
        const auto& m = measures[i];
        out.lines[i].rows = wrapTextNative(parseMarkup(in[i].left), 0.0f, false, m, textureWidth(h));
        float wide = 0.0f;
        for (const auto& row : out.lines[i].rows) wide = std::max(wide, row.width);
        if (!in[i].right.empty()) {
            float right = 0.0f;
            for (const auto& row : wrapTextNative(parseMarkup(in[i].right), 0.0f, false, m,
                                                  textureWidth(h)))
                right = std::max(right, row.width);
            out.lines[i].rightWidth = right;
            wide += kTooltipRightGap * pixelsPerUnit + right;
        }
        W = std::max(W, wide);
    }
    for (size_t i = 0; i < in.size(); ++i) {
        if (!in[i].wrap) continue;
        const auto& m = measures[i];
        const int h = out.lines[i].pixelHeight;
        const auto runs = parseMarkup(in[i].left);
        float at = W;
        if (W < defaultWrap) {
            at = W;
            for (const auto& row : wrapTextNative(runs, defaultWrap, false, m, textureWidth(h)))
                at = std::max(at, row.width);
        }
        out.lines[i].rows = wrapTextNative(runs, at, false, m, textureWidth(h));
        W = std::max(W, at);
    }
    out.contentWidth = W;
    return out;
}

} // namespace wowee::ui
