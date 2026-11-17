#include "ui/native_text_metrics.hpp"

#include <cassert>
#include <cmath>
#include <cstring>

// A private copy of ImGui's stb_truetype, static to this file: only the table
// readers are used, never the rasterizer.
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "imstb_truetype.h"

namespace wowee::ui {

struct NativeFace::Stb {
    stbtt_fontinfo info{};
};

std::shared_ptr<NativeFace> NativeFace::fromTrueType(const void* data, size_t size) {
    if (!data || size < 12) return nullptr;
    std::shared_ptr<NativeFace> face(new NativeFace());
    face->bytes_.assign(static_cast<const uint8_t*>(data),
                        static_cast<const uint8_t*>(data) + size);
    face->stb_ = std::make_unique<Stb>();
    const unsigned char* bytes = face->bytes_.data();
    const int offset = stbtt_GetFontOffsetForIndex(bytes, 0);
    if (offset < 0 || !stbtt_InitFont(&face->stb_->info, bytes, offset)) return nullptr;
    const int head = face->stb_->info.head;
    if (head <= 0 || static_cast<size_t>(head) + 20 > size) return nullptr;
    face->unitsPerEm_ = (bytes[head + 18] << 8) | bytes[head + 19];
    if (face->unitsPerEm_ <= 0) return nullptr;
    return face;
}

NativeFace::NativeFace(int unitsPerEm, std::unordered_map<uint32_t, NativeGlyph> glyphs,
                       std::unordered_map<uint64_t, int> kerning)
    : unitsPerEm_(unitsPerEm), glyphs_(std::move(glyphs)), kerning_(std::move(kerning)) {
    assert(unitsPerEm_ > 0);
}

NativeFace::~NativeFace() = default;

NativeGlyph NativeFace::glyph(uint32_t codepoint) const {
    const auto it = glyphs_.find(codepoint);
    if (it != glyphs_.end()) return it->second;
    NativeGlyph g;
    if (stb_) {
        const int index = stbtt_FindGlyphIndex(&stb_->info, static_cast<int>(codepoint));
        if (index > 0) {
            g.present = true;
            stbtt_GetGlyphHMetrics(&stb_->info, index, &g.advance, &g.lsb);
            int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            g.outline = stbtt_GetGlyphBox(&stb_->info, index, &x0, &y0, &x1, &y1) != 0 &&
                        !stbtt_IsGlyphEmpty(&stb_->info, index);
            g.xMax = x1;
        }
    }
    glyphs_.emplace(codepoint, g);
    return g;
}

int NativeFace::kerning(uint32_t left, uint32_t right) const {
    const uint64_t key = pairKey(left, right);
    const auto it = kerning_.find(key);
    if (it != kerning_.end()) return it->second;
    int k = 0;
    if (stb_) {
        const int a = stbtt_FindGlyphIndex(&stb_->info, static_cast<int>(left));
        const int b = stbtt_FindGlyphIndex(&stb_->info, static_cast<int>(right));
        if (a > 0 && b > 0) k = stbtt_GetGlyphKernAdvance(&stb_->info, a, b);
    }
    kerning_.emplace(key, k);
    return k;
}

int nativePixelHeight(float fontHeight, float pixelsPerUnit) {
    assert(std::isfinite(fontHeight) && std::isfinite(pixelsPerUnit) && pixelsPerUnit > 0.0f);
    return static_cast<int>(std::floor(fontHeight * pixelsPerUnit + 0.5f));
}

namespace {
/// round(units * h * 64 / upem): a font-unit length in 26.6 pixels.
int64_t to26_6(int units, int h, int upem) {
    const int64_t num = static_cast<int64_t>(units) * h * 64 * 2;
    const int64_t den = static_cast<int64_t>(upem) * 2;
    // Floor division of (num + upem) by den, correct for negative lengths too.
    const int64_t n = num + upem;
    return n >= 0 ? n / den : -((-n + den - 1) / den);
}
}  // namespace

int NativeTextMeasure::pen(uint32_t cp, uint32_t next) const {
    assert(face && pixelHeight > 0);
    const NativeGlyph g = face->glyph(cp);
    if (!g.present) {
        return missingAdvance
            ? static_cast<int>(std::ceil(missingAdvance(cp, pixelHeight, missingCtx))) : 0;
    }
    const int upem = face->unitsPerEm();
    int advance = static_cast<int>(to26_6(g.advance, pixelHeight, upem) >> 6) + 1;
    if (next != 0) {
        const int k = face->kerning(cp, next);
        if (k < 0) advance -= static_cast<int>((static_cast<int64_t>(-k) * pixelHeight) / upem);
    }
    return advance;
}

int NativeTextMeasure::extent(uint32_t cp) const {
    assert(face && pixelHeight > 0);
    const NativeGlyph g = face->glyph(cp);
    if (!g.present) {
        return missingAdvance
            ? static_cast<int>(std::ceil(missingAdvance(cp, pixelHeight, missingCtx))) : 0;
    }
    if (!g.outline) return (pixelHeight + 3) / 4;
    const int64_t x64 = to26_6(g.xMax, pixelHeight, face->unitsPerEm());
    return static_cast<int>((x64 + 63) >> 6) + (g.lsb != 0 ? 1 : 0);
}

int NativeTextMeasure::width(std::string_view utf8) const {
    size_t at = 0;
    if (at >= utf8.size()) return 0;
    uint32_t cp = nextCodepoint(utf8, at);
    int total = 0;
    while (at < utf8.size()) {
        const uint32_t next = nextCodepoint(utf8, at);
        total += pen(cp, next);
        cp = next;
    }
    return total + extent(cp);
}

uint32_t nextCodepoint(std::string_view text, size_t& at) {
    assert(at < text.size());
    const auto byte = [&](size_t i) { return static_cast<unsigned char>(text[i]); };
    const unsigned char c = byte(at);
    int len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
    if (at + static_cast<size_t>(len) > text.size()) len = 1;
    uint32_t cp = c;
    if (len > 1) {
        cp = c & (0x7F >> len);
        for (int i = 1; i < len; ++i) {
            const unsigned char cc = byte(at + static_cast<size_t>(i));
            if ((cc & 0xC0) != 0x80) { len = 1; cp = c; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
    }
    at += static_cast<size_t>(len);
    return cp;
}

} // namespace wowee::ui
