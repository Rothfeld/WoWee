#pragma once

// Interface glyphs rasterized the way the 2.4.3 client (build 8606) draws them.
//
// Measured pixel by pixel against native captures of FRIZQT__, ARIALN, SKURRI
// and MORPHEUS at 9..24 pixels (GlyphProbe addon, experiment
// _experiments/20261119T112856Z.freetype_glyphs/ in the workspace):
//
//   outline      unhinted TrueType outline, em = the pixel height, scaled
//                with FreeType's fixed-point arithmetic (NativeGlyphFace)
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
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

struct ImFontLoader;

namespace wowee::ui {

/// One interface glyph as the loader draws it at an em size.
struct NativeGlyphRaster {
    int advance = 0;  ///< pen advance, whole pixels (kerning aside)
    int left = 0;     ///< bitmap's left column from the pen
    int top = 0;      ///< bitmap's top row from the line top
    int width = 0;
    int rows = 0;     ///< at most the pixel height
    std::vector<unsigned char> coverage;  ///< width * rows, level * 17; empty for a space
};

/// A TrueType face's glyphs as 2.4.3 draws them. Outlines come from the glyf
/// table unhinted and are scaled the way FreeType scales them without hinting:
/// 16.16 scale = em pixels * 64 / unitsPerEm (rounded), every point
/// multiplied by it (FT_MulFix rounding), the outline moved left by the scaled
/// xMin - lsb, the advance the scaled phantom points' distance. Off-curve
/// points stay off-curve; ui/ft2110_grays.h draws them.
class NativeGlyphFace {
public:
    /// nullptr (logged) when `data` is no TrueType face with glyf outlines.
    /// `data` must outlive the face.
    static std::unique_ptr<NativeGlyphFace> open(const void* data, size_t size, int index);
    ~NativeGlyphFace();

    bool contains(uint32_t codepoint) const;
    int ascender() const { return ascender_; }    ///< hhea, font units
    int descender() const { return descender_; }  ///< hhea, font units, <= 0

    /// Pen advance in whole pixels at em size `emPixels`; nullopt when the
    /// face lacks the glyph or its outline is malformed (logged).
    std::optional<int> advance(uint32_t codepoint, float emPixels) const;
    /// The glyph's bitmap and placement at em size `emPixels`.
    std::optional<NativeGlyphRaster> rasterize(uint32_t codepoint, float emPixels) const;

private:
    struct Stb;
    struct Outline;
    NativeGlyphFace() = default;
    bool loadGlyph(int glyph, int32_t scale, int depth, Outline& out) const;
    std::optional<Outline> loadOutline(uint32_t codepoint, float emPixels) const;

    std::unique_ptr<Stb> stb_;
    const unsigned char* data_ = nullptr;
    size_t size_ = 0;
    int unitsPerEm_ = 0;
    int ascender_ = 0;
    int descender_ = 0;
};

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
