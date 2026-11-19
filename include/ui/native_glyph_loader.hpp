#pragma once

// Interface glyphs rasterized the way the 2.4.3 client (build 8606) draws them.
//
// Measured pixel by pixel against native captures of FRIZQT__, ARIALN, SKURRI
// and MORPHEUS at 9..24 pixels (GlyphProbe addon, experiment
// _experiments/20261119T112856Z.freetype_glyphs/ in the workspace):
//
//   outline      unhinted (FT_LOAD_NO_HINTING), em = the pixel height
//   coverage     FreeType 2.1.10's gray rasterizer (ui/ft2110_grays.h)
//   levels       4 bit: coverage >> 4, drawn as level * 17 alpha
//   rows         only the first `pixel height` rows of a glyph are drawn
//   x            pen + ceil(xMin) - 1 (one pixel left of the bitmap box
//                when the outline starts exactly on a pixel edge)
//   baseline     line top + h + 1 - round(h * descender / (ascender + descender)),
//                hhea ascender and descender
//
// FreeType's own hinting (bytecode v35/v40, auto, light) matches none of it.
// All 2728 probe glyphs land with no ink outside the box this predicts;
// remaining coverage differences total 273 4-bit levels.

#include <cassert>
#include <cstdint>

struct ImFontLoader;

namespace wowee::ui {

/// The ImGui font loader for interface faces (ImFontConfig::FontLoader).
/// Sizes are em sizes: a face added at N draws its em square N pixels tall,
/// so ExtraSizeScale stays 1 for these sources.
const ImFontLoader* nativeGlyphLoader();

/// Rows from the line top to the baseline at pixel height `h`, for a face
/// with hhea `ascender` and `descender` (font units, descender negative):
/// h + 1 - round(h * -descender / (ascender - descender)), halves up.
inline int nativeBaselineRow(int h, int ascender, int descender) {
    assert(h > 0 && ascender > 0 && descender <= 0);
    const int64_t below = -static_cast<int64_t>(descender);
    const int64_t span = static_cast<int64_t>(ascender) + below;
    return h + 1 - static_cast<int>((2 * h * below + span) / (2 * span));
}

/// Native 4-bit coverage of an 8-bit rasterizer value, back on the 0..255 scale.
inline unsigned char nativeCoverage(unsigned char value) {
    return static_cast<unsigned char>((value >> 4) * 17);
}

/// Pixels from the pen to a glyph bitmap's left column, for an outline whose
/// control box starts at `xMin` (26.6 pixels): ceil(xMin) - 1.
inline int nativeGlyphLeft(long xMin) {
    return static_cast<int>((xMin - 1) >> 6);
}

} // namespace wowee::ui
