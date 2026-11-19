#include "ui/native_glyph_loader.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_SIZES_H

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <new>
#include <vector>

#include "core/logger.hpp"
#include "ui/ft2110_grays.h"

namespace wowee::ui {

namespace {

/// One FreeType library for every interface face. ImGui does not call
/// LoaderInit for a per-source loader, so it lives here.
FT_Library library() {
    static FT_Library lib = [] {
        FT_Library l = nullptr;
        const FT_Error err = FT_Init_FreeType(&l);
        if (err != 0) LOG_ERROR("FT_Init_FreeType failed: ", err);
        assert(err == 0 && l);
        return l;
    }();
    return lib;
}

struct SourceData {
    FT_Face face = nullptr;
    const ImFontBaked* activeBaked = nullptr;
};

struct BakedData {
    FT_Size size = nullptr;
    int pixelHeight = 0;
    int baseline = 0;
};

bool fontSrcInit(ImFontAtlas*, ImFontConfig* src) {
    assert(src->FontLoaderData == nullptr);
    auto* data = IM_NEW(SourceData)();
    const FT_Error err = FT_New_Memory_Face(library(), static_cast<const FT_Byte*>(src->FontData),
                                            static_cast<FT_Long>(src->FontDataSize),
                                            static_cast<FT_Long>(src->FontNo), &data->face);
    if (err != 0) {
        LOG_ERROR("FreeType cannot open interface face ", src->Name, ": error ", err);
        IM_DELETE(data);
        return false;
    }
    assert(FT_IS_SCALABLE(data->face) && data->face->ascender - data->face->descender > 0);
    src->FontLoaderData = data;
    return true;
}

void fontSrcDestroy(ImFontAtlas*, ImFontConfig* src) {
    auto* data = static_cast<SourceData*>(src->FontLoaderData);
    if (!data) return;
    FT_Done_Face(data->face);
    IM_DELETE(data);
    src->FontLoaderData = nullptr;
}

bool fontSrcContainsGlyph(ImFontAtlas*, ImFontConfig* src, ImWchar codepoint) {
    const auto* data = static_cast<SourceData*>(src->FontLoaderData);
    return FT_Get_Char_Index(data->face, codepoint) != 0;
}

void activate(SourceData* data, BakedData* bakedData, const ImFontBaked* baked) {
    if (data->activeBaked == baked) return;
    FT_Activate_Size(bakedData->size);
    data->activeBaked = baked;
}

bool fontBakedInit(ImFontAtlas*, ImFontConfig* src, ImFontBaked* baked, void* loaderData) {
    auto* data = static_cast<SourceData*>(src->FontLoaderData);
    auto* bakedData = new (loaderData) BakedData();
    // A size is an em size here (see nativeGlyphLoader). Interface text is
    // drawn at whole pixel heights; other callers may ask for a fraction, and
    // the outline is scaled to it exactly while the row rules use the
    // nearest whole height.
    assert(src->ExtraSizeScale == 1.0f && "interface faces are added at their em size");
    float size = baked->Size;
    if (src->MergeMode && src->SizePixels != 0.0f)
        size *= src->SizePixels / baked->OwnerFont->Sources[0]->SizePixels;
    FT_Error err = FT_New_Size(data->face, &bakedData->size);
    if (err == 0) err = FT_Activate_Size(bakedData->size);
    if (err == 0) {
        err = FT_Set_Char_Size(data->face, 0, static_cast<FT_F26Dot6>(std::lround(size * 64.0f)), 72, 72);
    }
    if (err != 0) {
        LOG_ERROR("FreeType cannot size interface face ", src->Name, " to ", size, ": error ", err);
        return false;
    }
    data->activeBaked = baked;
    bakedData->pixelHeight = std::max(1, static_cast<int>(std::floor(size + 0.5f)));
    bakedData->baseline = nativeBaselineRow(bakedData->pixelHeight, data->face->ascender,
                                            data->face->descender);
    if (!src->MergeMode) {
        baked->Ascent = static_cast<float>(bakedData->baseline);
        baked->Descent = static_cast<float>(bakedData->baseline - bakedData->pixelHeight);
    }
    return true;
}

void fontBakedDestroy(ImFontAtlas*, ImFontConfig* src, ImFontBaked* baked, void* loaderData) {
    auto* data = static_cast<SourceData*>(src->FontLoaderData);
    auto* bakedData = static_cast<BakedData*>(loaderData);
    if (data && data->activeBaked == baked) data->activeBaked = nullptr;
    if (bakedData->size) FT_Done_Size(bakedData->size);
    bakedData->~BakedData();
}

bool fontBakedLoadGlyph(ImFontAtlas* atlas, ImFontConfig* src, ImFontBaked* baked, void* loaderData,
                        ImWchar codepoint, ImFontGlyph* outGlyph, float* outAdvanceX) {
    auto* data = static_cast<SourceData*>(src->FontLoaderData);
    auto* bakedData = static_cast<BakedData*>(loaderData);
    const FT_UInt index = FT_Get_Char_Index(data->face, codepoint);
    if (index == 0) return false;
    activate(data, bakedData, baked);
    const FT_Error err = FT_Load_Glyph(data->face, index, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP);
    if (err != 0) {
        LOG_ERROR("FreeType cannot load glyph ", static_cast<unsigned>(codepoint), " of ", src->Name,
                  ": error ", err);
        return false;
    }
    const FT_GlyphSlot slot = data->face->glyph;
    // The pen advance the original measures with (ui/native_text_metrics.hpp),
    // kerning aside: whole pixels of the scaled advance, plus one.
    const float advance = static_cast<float>((slot->advance.x >> 6) + 1);
    if (outAdvanceX) {
        assert(outGlyph == nullptr);
        *outAdvanceX = advance;
        return true;
    }
    assert(slot->format == FT_GLYPH_FORMAT_OUTLINE);
    outGlyph->Codepoint = codepoint;
    outGlyph->AdvanceX = advance;

    const FT_Outline& outline = slot->outline;
    FT_BBox box;
    FT_Outline_Get_CBox(&outline, &box);
    const long x0 = box.xMin & ~63L, y0 = box.yMin & ~63L;
    const long x1 = (box.xMax + 63) & ~63L, y1 = (box.yMax + 63) & ~63L;
    const int width = static_cast<int>((x1 - x0) >> 6);
    const int rows = static_cast<int>((y1 - y0) >> 6);
    if (outline.n_points == 0 || width == 0 || rows == 0) return true;  // a space: advance only

    std::vector<long> xy(static_cast<size_t>(outline.n_points) * 2);
    for (int i = 0; i < outline.n_points; ++i) {
        xy[2 * i] = outline.points[i].x - x0;
        xy[2 * i + 1] = outline.points[i].y - y0;
    }
    std::vector<unsigned char> coverage(static_cast<size_t>(width) * rows, 0);
    const int rasterError = wowee_ft2110_render(
        outline.n_contours, outline.n_points, xy.data(), reinterpret_cast<const char*>(outline.tags),
        outline.contours, (outline.flags & FT_OUTLINE_EVEN_ODD_FILL) != 0, coverage.data(), width, rows);
    if (rasterError != 0) {
        LOG_ERROR("Rasterizer error ", rasterError, " on glyph ", static_cast<unsigned>(codepoint), " of ",
                  src->Name);
        return false;
    }
    // Only the first h rows are drawn: a SKURRI "f" at 16 pixels loses the
    // five rows of its tail below that.
    const int shownRows = std::min(rows, bakedData->pixelHeight);
    for (unsigned char& v : coverage) v = nativeCoverage(v);

    const ImFontAtlasRectId pack = ImFontAtlasPackAddRect(atlas, width, shownRows);
    if (pack == ImFontAtlasRectId_Invalid) {
        LOG_ERROR("Font atlas full at glyph ", static_cast<unsigned>(codepoint), " of ", src->Name);
        return false;
    }
    ImTextureRect* rect = ImFontAtlasPackGetRect(atlas, pack);
    const float left = static_cast<float>(nativeGlyphLeft(box.xMin));
    const float top = static_cast<float>(bakedData->baseline - static_cast<int>(y1 >> 6));
    outGlyph->X0 = left;
    outGlyph->Y0 = top;
    outGlyph->X1 = left + static_cast<float>(width);
    outGlyph->Y1 = top + static_cast<float>(shownRows);
    outGlyph->Visible = true;
    outGlyph->Colored = false;
    outGlyph->PackId = pack;
    ImFontAtlasBakedSetFontGlyphBitmap(atlas, baked, src, outGlyph, rect, coverage.data(),
                                       ImTextureFormat_Alpha8, width);
    return true;
}

} // namespace

const ImFontLoader* nativeGlyphLoader() {
    static ImFontLoader loader = [] {
        ImFontLoader l;
        l.Name = "WoW 2.4.3 interface glyphs";
        l.FontSrcInit = fontSrcInit;
        l.FontSrcDestroy = fontSrcDestroy;
        l.FontSrcContainsGlyph = fontSrcContainsGlyph;
        l.FontBakedInit = fontBakedInit;
        l.FontBakedDestroy = fontBakedDestroy;
        l.FontBakedLoadGlyph = fontBakedLoadGlyph;
        l.FontBakedSrcLoaderDataSize = sizeof(BakedData);
        return l;
    }();
    return &loader;
}

} // namespace wowee::ui
