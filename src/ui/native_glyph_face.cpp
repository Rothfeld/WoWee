#include "ui/native_glyph_loader.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/logger.hpp"
#include "ui/ft2110_grays.h"

// A private copy of ImGui's stb_truetype, static to this file: only its table
// readers (cmap, hmtx, hhea, loca) are used. The glyf outlines are read below,
// with every off-curve point kept: stbtt_GetGlyphShape replaces two
// consecutive off-curve points by an on-curve midpoint computed in font units,
// where the rasterizer takes that midpoint after scaling.
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "imstb_truetype.h"

namespace wowee::ui {

namespace {

// FreeType's 16.16 arithmetic (ftcalc.c), which places every outline point.
long mulFix(long a, long b) {
    const int64_t ab = static_cast<int64_t>(a) * b;
    return static_cast<long>((ab + 0x8000 - (ab < 0 ? 1 : 0)) >> 16);
}

long divFix(long a, long b) {
    assert(a > 0 && b > 0);
    return static_cast<long>(((static_cast<int64_t>(a) << 16) + (b >> 1)) / b);
}

/// 16.16 font units to 26.6 pixels at em size `emPixels`, as FT_Set_Char_Size
/// (72 dpi) sets it for an unhinted load: FT_DivFix(em in 26.6, unitsPerEm).
long emScale(float emPixels, int unitsPerEm) {
    const long em = std::max(64L, std::lround(emPixels * 64.0f));
    return divFix(em, unitsPerEm);
}

// Composite glyph component flags (TrueType glyf).
constexpr uint16_t kArgsAreWords = 0x0001;
constexpr uint16_t kArgsAreXyValues = 0x0002;
constexpr uint16_t kHaveScale = 0x0008;
constexpr uint16_t kMoreComponents = 0x0020;
constexpr uint16_t kHaveXyScale = 0x0040;
constexpr uint16_t kHaveTwoByTwo = 0x0080;
constexpr uint16_t kUseMyMetrics = 0x0200;
constexpr uint16_t kScaledComponentOffset = 0x0800;

/// Big-endian reads inside the font, failing (not reading) past its end.
struct Reader {
    const unsigned char* data;
    size_t size;
    size_t pos;
    bool ok = true;

    bool has(size_t n) {
        if (pos + n > size) ok = false;
        return ok;
    }
    uint8_t u8() { return has(1) ? data[pos++] : 0; }
    int8_t s8() { return static_cast<int8_t>(u8()); }
    uint16_t u16() {
        if (!has(2)) return 0;
        const uint16_t v = static_cast<uint16_t>((data[pos] << 8) | data[pos + 1]);
        pos += 2;
        return v;
    }
    int16_t s16() { return static_cast<int16_t>(u16()); }
};

} // namespace

struct NativeGlyphFace::Stb {
    stbtt_fontinfo info{};

    /// The cmap's glyph, 0 for none. A cmap entry past the last glyph is none,
    /// as FT_Get_Char_Index has it (FRIZQT__ maps U+FFFF to glyph 65535).
    int glyphIndex(uint32_t codepoint) const {
        const int glyph = stbtt_FindGlyphIndex(&info, static_cast<int>(codepoint));
        return glyph < info.numGlyphs ? glyph : 0;
    }
};

/// A glyph's outline in 26.6 pixels, before the move by -pp1.
struct NativeGlyphFace::Outline {
    std::vector<long> xy;  ///< x, y per point
    std::vector<char> tags;  ///< 1 on curve, 0 quadratic control
    std::vector<short> ends;
    long pp1 = 0;  ///< scaled xMin - lsb
    long pp2 = 0;  ///< scaled xMin - lsb + advance width
};

std::unique_ptr<NativeGlyphFace> NativeGlyphFace::open(const void* data, size_t size, int index) {
    std::unique_ptr<NativeGlyphFace> face(new NativeGlyphFace());
    face->stb_ = std::make_unique<Stb>();
    face->data_ = static_cast<const unsigned char*>(data);
    face->size_ = size;
    const int offset = stbtt_GetFontOffsetForIndex(face->data_, index);
    if (size < 12 || offset < 0 || !stbtt_InitFont(&face->stb_->info, face->data_, offset)) {
        LOG_ERROR("Interface face ", index, " is no TrueType font stb_truetype can open");
        return nullptr;
    }
    const stbtt_fontinfo& info = face->stb_->info;
    if (info.glyf == 0 || info.hhea == 0 || static_cast<size_t>(info.head) + 54 > size) {
        LOG_ERROR("Interface face ", index, " has no glyf/hhea/head table");
        return nullptr;
    }
    face->unitsPerEm_ = (face->data_[info.head + 18] << 8) | face->data_[info.head + 19];
    int lineGap = 0;
    stbtt_GetFontVMetrics(&info, &face->ascender_, &face->descender_, &lineGap);
    if (face->unitsPerEm_ <= 0 || face->ascender_ <= 0 || face->descender_ > 0) {
        LOG_ERROR("Interface face ", index, ": unitsPerEm ", face->unitsPerEm_, ", hhea ascender ",
                  face->ascender_, ", descender ", face->descender_);
        return nullptr;
    }
    return face;
}

NativeGlyphFace::~NativeGlyphFace() = default;

bool NativeGlyphFace::contains(uint32_t codepoint) const {
    return stb_->glyphIndex(codepoint) != 0;
}

// Mirrors FreeType 2.13's unhinted TrueType load (truetype/ttgload.c,
// load_truetype_glyph): points scaled one by one, phantom points from the
// glyf header's xMin and hmtx, components transformed after scaling and
// moved by their scaled offset; USE_MY_METRICS takes the component's
// phantom points.
bool NativeGlyphFace::loadGlyph(int glyph, int32_t scale, int depth, Outline& out) const {
    const stbtt_fontinfo& info = stb_->info;
    if (depth > 8 || glyph < 0 || glyph >= info.numGlyphs) {
        LOG_ERROR("Interface face: glyph ", glyph, " out of range or nested too deep");
        return false;
    }
    int advanceWidth = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(&info, glyph, &advanceWidth, &lsb);
    const int offset = stbtt__GetGlyfOffset(&info, glyph);
    Reader r{data_, size_, offset < 0 ? 0 : static_cast<size_t>(offset)};
    const int contours = offset < 0 ? 0 : r.s16();
    const int xMin = contours == 0 ? 0 : r.s16();
    r.pos += 6;  // yMin, xMax, yMax
    out.pp1 = mulFix(xMin - lsb, scale);
    out.pp2 = mulFix(xMin - lsb + advanceWidth, scale);
    if (contours == 0) return r.ok;  // a space

    if (contours > 0) {
        const size_t base = out.tags.size();
        std::vector<int> ends(static_cast<size_t>(contours));
        for (int& e : ends) e = r.u16();
        for (size_t i = 1; i < ends.size(); ++i) {
            if (ends[i] <= ends[i - 1]) r.ok = false;
        }
        const size_t points = static_cast<size_t>(ends.back()) + 1;
        r.pos += r.u16();  // instructions
        if (!r.ok || base + points > 0x7fff) {
            LOG_ERROR("Interface face: glyph ", glyph, " has a malformed outline");
            return false;
        }
        std::vector<uint8_t> flags(points);
        for (size_t i = 0; i < points && r.ok;) {
            const uint8_t f = r.u8();
            size_t repeat = (f & 8) ? r.u8() : 0;
            flags[i++] = f;
            for (; repeat > 0 && i < points; --repeat) flags[i++] = f;
        }
        std::vector<long> xs(points), ys(points);
        long x = 0, y = 0;
        for (size_t i = 0; i < points; ++i) {
            const uint8_t f = flags[i];
            if (f & 2) {
                const long d = r.u8();
                x += (f & 16) ? d : -d;
            } else if (!(f & 16)) {
                x += r.s16();
            }
            xs[i] = x;
        }
        for (size_t i = 0; i < points; ++i) {
            const uint8_t f = flags[i];
            if (f & 4) {
                const long d = r.u8();
                y += (f & 32) ? d : -d;
            } else if (!(f & 32)) {
                y += r.s16();
            }
            ys[i] = y;
        }
        if (!r.ok) {
            LOG_ERROR("Interface face: glyph ", glyph, " outline runs past the font's end");
            return false;
        }
        for (size_t i = 0; i < points; ++i) {
            out.xy.push_back(mulFix(xs[i], scale));
            out.xy.push_back(mulFix(ys[i], scale));
            out.tags.push_back(static_cast<char>(flags[i] & 1));
        }
        for (int e : ends) out.ends.push_back(static_cast<short>(base + static_cast<size_t>(e)));
        return true;
    }

    // Composite: components one after another.
    for (uint16_t flags = kMoreComponents; flags & kMoreComponents;) {
        flags = r.u16();
        const int component = r.u16();
        long arg1 = 0, arg2 = 0;
        if (flags & kArgsAreWords) {
            arg1 = r.s16();
            arg2 = r.s16();
        } else {
            arg1 = r.s8();
            arg2 = r.s8();
        }
        long xx = 0x10000, xy = 0, yx = 0, yy = 0x10000;  // 16.16 from F2Dot14
        if (flags & kHaveScale) {
            xx = yy = r.s16() * 4L;
        } else if (flags & kHaveXyScale) {
            xx = r.s16() * 4L;
            yy = r.s16() * 4L;
        } else if (flags & kHaveTwoByTwo) {
            xx = r.s16() * 4L;
            yx = r.s16() * 4L;
            xy = r.s16() * 4L;
            yy = r.s16() * 4L;
        }
        const bool haveScale = (flags & (kHaveScale | kHaveXyScale | kHaveTwoByTwo)) != 0;
        if (!r.ok) {
            LOG_ERROR("Interface face: composite glyph ", glyph, " runs past the font's end");
            return false;
        }
        if (!(flags & kArgsAreXyValues) || (haveScale && (flags & kScaledComponentOffset))) {
            // Point-matched and scaled-offset components: no interface face has one.
            LOG_ERROR("Interface face: composite glyph ", glyph, " component flags ", flags,
                      " not supported");
            return false;
        }
        const long pp1 = out.pp1, pp2 = out.pp2;
        const size_t base = out.tags.size();
        if (!loadGlyph(component, scale, depth + 1, out)) return false;
        if (!(flags & kUseMyMetrics)) {
            out.pp1 = pp1;
            out.pp2 = pp2;
        }
        const long dx = mulFix(arg1, scale), dy = mulFix(arg2, scale);
        for (size_t i = base; i < out.tags.size(); ++i) {
            long& px = out.xy[2 * i];
            long& py = out.xy[2 * i + 1];
            if (haveScale) {
                const long tx = mulFix(px, xx) + mulFix(py, xy);
                const long ty = mulFix(px, yx) + mulFix(py, yy);
                px = tx;
                py = ty;
            }
            px += dx;
            py += dy;
        }
        if (out.tags.size() > 0x7fff) {
            LOG_ERROR("Interface face: composite glyph ", glyph, " has too many points");
            return false;
        }
    }
    return true;
}

std::optional<NativeGlyphFace::Outline> NativeGlyphFace::loadOutline(uint32_t codepoint,
                                                                     float emPixels) const {
    const int glyph = stb_->glyphIndex(codepoint);
    if (glyph == 0) return std::nullopt;
    Outline outline;
    if (!loadGlyph(glyph, static_cast<int32_t>(emScale(emPixels, unitsPerEm_)), 0, outline)) {
        LOG_ERROR("Interface face cannot load codepoint ", codepoint);
        return std::nullopt;
    }
    // (0, 0) is the pen: the outline moves by -pp1, as FreeType moves it.
    for (size_t i = 0; i < outline.tags.size(); ++i) outline.xy[2 * i] -= outline.pp1;
    return outline;
}

std::optional<int> NativeGlyphFace::advance(uint32_t codepoint, float emPixels) const {
    const auto outline = loadOutline(codepoint, emPixels);
    if (!outline) return std::nullopt;
    // The pen advance the original measures with (ui/native_text_metrics.hpp),
    // kerning aside: whole pixels of the scaled advance, plus one.
    return static_cast<int>(((outline->pp2 - outline->pp1) >> 6) + 1);
}

std::optional<NativeGlyphRaster> NativeGlyphFace::rasterize(uint32_t codepoint, float emPixels) const {
    const auto outline = loadOutline(codepoint, emPixels);
    if (!outline) return std::nullopt;
    NativeGlyphRaster glyph;
    glyph.advance = static_cast<int>(((outline->pp2 - outline->pp1) >> 6) + 1);
    const size_t points = outline->tags.size();
    if (points == 0) return glyph;  // a space: advance only

    long xMin = outline->xy[0], xMax = xMin, yMin = outline->xy[1], yMax = yMin;
    for (size_t i = 1; i < points; ++i) {
        xMin = std::min(xMin, outline->xy[2 * i]);
        xMax = std::max(xMax, outline->xy[2 * i]);
        yMin = std::min(yMin, outline->xy[2 * i + 1]);
        yMax = std::max(yMax, outline->xy[2 * i + 1]);
    }
    const long x0 = xMin & ~63L, y0 = yMin & ~63L;
    const long x1 = (xMax + 63) & ~63L, y1 = (yMax + 63) & ~63L;
    const int width = static_cast<int>((x1 - x0) >> 6);
    const int rows = static_cast<int>((y1 - y0) >> 6);
    if (width == 0 || rows == 0) return glyph;

    std::vector<long> xy(outline->xy);
    for (size_t i = 0; i < points; ++i) {
        xy[2 * i] -= x0;
        xy[2 * i + 1] -= y0;
    }
    std::vector<unsigned char> coverage(static_cast<size_t>(width) * rows, 0);
    const int rasterError = wowee_ft2110_render(
        static_cast<short>(outline->ends.size()), static_cast<short>(points), xy.data(), outline->tags.data(),
        outline->ends.data(), 0, coverage.data(), width, rows);
    if (rasterError != 0) {
        LOG_ERROR("Rasterizer error ", rasterError, " on codepoint ", codepoint);
        return std::nullopt;
    }
    // Only the first h rows are drawn: a SKURRI "f" at 16 pixels loses the
    // five rows of its tail below that.
    const int pixelHeight = std::max(1, static_cast<int>(std::floor(emPixels + 0.5f)));
    glyph.rows = std::min(rows, pixelHeight);
    glyph.width = width;
    glyph.left = nativeGlyphLeft(xMin);
    glyph.top = nativeBaselineRow(pixelHeight, ascender_, descender_) - static_cast<int>(y1 >> 6);
    coverage.resize(static_cast<size_t>(width) * glyph.rows);
    for (unsigned char& v : coverage) v = nativeCoverage(v);
    glyph.coverage = std::move(coverage);
    return glyph;
}

} // namespace wowee::ui
