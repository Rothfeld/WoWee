#pragma once

// Which screen pixels a scroll frame leaves visible in build 8606, where its
// edges fall between pixels. Text line placement is placeTextLine in
// text_wrap.hpp. Measured on 1280x720 (15/16 pixel per interface unit) with a
// probe addon: scroll frames stepped through all sixteen sub-pixel phases,
// white textures and glyphs on black, both clients captured (workspace _experiments/20261119T0935Z.raidinfo_clip).
//
// Pixel coordinates run right and down from the window's top-left corner, the
// way the draw list counts them; a pixel span is first..last inclusive.
//
//   texture clipped by a scroll frame   pixels round(a) .. round(b), both ends
//                                       kept: one column/row past the far edge
//   glyph clipped by a scroll frame     across: every column the rect touches,
//                                       floor(a) .. ceil(b) - 1; down: rows on
//                                       an even grid, the first even row at or
//                                       below floor(top) to the first even row
//                                       at or below ceil(bottom), inclusive
//
// Ties (an edge on exactly .5) follow the native float error of the anchor
// chain and land either way there; these rules take the round-half-up side.

#include <cmath>

namespace wowee::ui {

struct PixelSpan {
    int first = 0;
    int last = -1;  ///< inclusive
};

/// Columns (or rows) a scroll frame spanning a..b pixels leaves visible to
/// the textures and backdrops inside it.
inline PixelSpan scrollClipTextureSpan(float a, float b) {
    return {static_cast<int>(std::floor(a + 0.5f)), static_cast<int>(std::floor(b + 0.5f))};
}

/// Columns a scroll frame spanning a..b pixels leaves visible to the glyphs
/// inside it: any column the frame covers in part is kept. The right edge was
/// not measured on its own (glyph gaps hide it); it is taken as the mirror of
/// the left.
inline PixelSpan scrollClipTextColumns(float a, float b) {
    return {static_cast<int>(std::floor(a)), static_cast<int>(std::ceil(b)) - 1};
}

/// Rows a scroll frame spanning a..b pixels (top..bottom) leaves visible to
/// the glyphs inside it. Both edges land on even rows, moved down: 32 of 32
/// measured phases (top 119.06 -> 120, 118.13 -> 118; bottom 130.31 -> 132,
/// 129.38 -> 130), while a texture in the same frame stops on odd rows too.
inline PixelSpan scrollClipTextRows(float a, float b) {
    const auto evenUp = [](int v) { return v + (v & 1); };
    return {evenUp(static_cast<int>(std::floor(a))), evenUp(static_cast<int>(std::ceil(b)))};
}

} // namespace wowee::ui
