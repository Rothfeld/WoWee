#pragma once

// Text measured the way the original 2.4.3 client (build 8606) measures it.
//
// The original rasterizes each interface font at a whole number of pixels and
// lays glyphs out on whole pixels, so every FontString width it reports is an
// integer pixel count divided by the pixels per interface unit. Measured on
// FRIZQT__.TTF at pixel heights 7..23 (2083 strings, all but one exact):
//
//   pixel height  floor(fontHeight * pixelsPerUnit + 0.5)
//   advance       (round(advance * h * 64 / unitsPerEm) >> 6) + 1
//   kerning       negative pairs truncated toward zero; positive pairs unused
//   last glyph    ceil(round26.6(xMax * h / unitsPerEm)) + (lsb != 0 ? 1 : 0)
//   space alone   ceil(h / 4)
//
// "Binds when picked up" at FontHeight 12 on a 720-pixel window is 11 pixels
// high and 119 pixels wide. Evidence and the measuring addon live in
// _experiments/20261117T193057Z.tooltip_text_wrap/ of the workspace.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace wowee::ui {

/// What one glyph contributes to a measure, in font units.
struct NativeGlyph {
    int advance = 0;
    int lsb = 0;
    int xMax = 0;
    bool outline = false;  ///< has contours; a space has none
    bool present = false;  ///< the face maps the codepoint to a real glyph
};

/// A face's metric tables: advances, bounding boxes and kerning.
class NativeFace {
public:
    /// From a TrueType file. The bytes are copied. Null when the file does
    /// not parse.
    static std::shared_ptr<NativeFace> fromTrueType(const void* data, size_t size);

    /// From tables given directly, for tests that cannot ship a font file.
    NativeFace(int unitsPerEm, std::unordered_map<uint32_t, NativeGlyph> glyphs,
               std::unordered_map<uint64_t, int> kerning);
    ~NativeFace();
    NativeFace(const NativeFace&) = delete;
    NativeFace& operator=(const NativeFace&) = delete;

    int unitsPerEm() const { return unitsPerEm_; }
    NativeGlyph glyph(uint32_t codepoint) const;
    /// Kerning between two codepoints in font units, zero for none.
    int kerning(uint32_t left, uint32_t right) const;

    static uint64_t pairKey(uint32_t left, uint32_t right) {
        return (static_cast<uint64_t>(left) << 32) | right;
    }

private:
    NativeFace() = default;
    struct Stb;
    int unitsPerEm_ = 0;
    mutable std::unordered_map<uint32_t, NativeGlyph> glyphs_;
    mutable std::unordered_map<uint64_t, int> kerning_;
    std::vector<uint8_t> bytes_;
    std::unique_ptr<Stb> stb_;
};

/// The pixel height a font of `fontHeight` interface units is rasterized at.
int nativePixelHeight(float fontHeight, float pixelsPerUnit);

/// Outline thickness in pixels for a WoW font outline flag:
/// "" -> 0, "NORMAL" / "OUTLINE" -> 1, "THICK" / "THICKOUTLINE" -> 2.
inline int outlinePixelsForFlag(std::string_view outline) {
    if (outline.empty()) return 0;
    if (outline.find("THICK") != std::string_view::npos) return 2;
    return 1;
}

/// One face at one pixel height. Codepoints the face does not have are
/// measured through `missingAdvance` (the width the merged fallback face
/// draws them at, in pixels), rounded up: the original's localized clients
/// ship their own faces, so there is no measurement of this case to follow.
///
/// `outlinePixels` (0 none, 1 OUTLINE, 2 THICKOUTLINE) applies 2.4.3's outline
/// metrics measured on build 8606: OUTLINE adds 4 px to the final glyph extent
/// (+0 per-glyph pen), THICKOUTLINE adds 8 px to the final glyph extent and
/// +1 px to each outlined glyph's pen advance.
struct NativeTextMeasure {
    const NativeFace* face = nullptr;
    int pixelHeight = 0;
    int outlinePixels = 0;
    float (*missingAdvance)(uint32_t codepoint, int pixelHeight, const void* ctx) = nullptr;
    const void* missingCtx = nullptr;

    /// Pen advance from `cp` to the glyph after it, kerning included.
    /// `next` is 0 at the end of the text.
    int pen(uint32_t cp, uint32_t next) const;
    /// Width `cp` adds when it is the last glyph of the text.
    int extent(uint32_t cp) const;
    /// GetStringWidth of one line of plain text, in pixels.
    int width(std::string_view utf8) const;
};

/// Decode one UTF-8 codepoint at `at`, advancing it. Invalid bytes decode as
/// themselves.
uint32_t nextCodepoint(std::string_view text, size_t& at);

} // namespace wowee::ui
