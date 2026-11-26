#include "ui/native_glyph_loader.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <new>

#include "core/logger.hpp"

namespace wowee::ui {

namespace {

struct BakedData {
    float size = 0.0f;
    int pixelHeight = 0;
    int baseline = 0;
};

NativeGlyphFace* faceOf(ImFontConfig* src) { return static_cast<NativeGlyphFace*>(src->FontLoaderData); }

template <int OutlinePixels>
bool fontSrcInitT(ImFontAtlas* atlas, ImFontConfig* src) {
    assert(src->FontLoaderData == nullptr);
    auto face = NativeGlyphFace::open(src->FontData, static_cast<size_t>(src->FontDataSize), src->FontNo);
    if (!face) {
        LOG_ERROR("Cannot open interface face ", src->Name);
        return false;
    }
    if constexpr (OutlinePixels > 0) {
        if (atlas) {
            atlas->TexDesiredFormat = ImTextureFormat_RGBA32;
            atlas->TexPixelsUseColors = true;
            if (atlas->TexData) atlas->TexData->UseColors = true;
        }
    }
    src->FontLoaderData = face.release();
    return true;
}

void fontSrcDestroy(ImFontAtlas*, ImFontConfig* src) {
    delete faceOf(src);
    src->FontLoaderData = nullptr;
}

bool fontSrcContainsGlyph(ImFontAtlas*, ImFontConfig* src, ImWchar codepoint) {
    return faceOf(src)->contains(codepoint);
}

bool fontBakedInit(ImFontAtlas*, ImFontConfig* src, ImFontBaked* baked, void* loaderData) {
    const NativeGlyphFace* face = faceOf(src);
    auto* bakedData = new (loaderData) BakedData();
    assert(src->ExtraSizeScale == 1.0f && "interface faces are added at their em size");
    float size = baked->Size;
    if (src->MergeMode && src->SizePixels != 0.0f)
        size *= src->SizePixels / baked->OwnerFont->Sources[0]->SizePixels;
    bakedData->size = size;
    bakedData->pixelHeight = std::max(1, static_cast<int>(std::floor(size + 0.5f)));
    bakedData->baseline = nativeBaselineRow(bakedData->pixelHeight, face->ascender(), face->descender());
    if (!src->MergeMode) {
        baked->Ascent = static_cast<float>(bakedData->baseline);
        baked->Descent = static_cast<float>(bakedData->baseline - bakedData->pixelHeight);
    }
    return true;
}

void fontBakedDestroy(ImFontAtlas*, ImFontConfig*, ImFontBaked*, void* loaderData) {
    static_cast<BakedData*>(loaderData)->~BakedData();
}

template <int OutlinePixels>
bool fontBakedLoadGlyphT(ImFontAtlas* atlas, ImFontConfig* src, ImFontBaked* baked, void* loaderData,
                         ImWchar codepoint, ImFontGlyph* outGlyph, float* outAdvanceX) {
    const NativeGlyphFace* face = faceOf(src);
    const auto* bakedData = static_cast<const BakedData*>(loaderData);
    if (outAdvanceX) {
        assert(outGlyph == nullptr);
        const auto advance = face->advance(codepoint, bakedData->size);
        if (!advance) return false;
        const int extra = (OutlinePixels > 1 && codepoint != ' ') ? (OutlinePixels - 1) : 0;
        *outAdvanceX = static_cast<float>(*advance + extra);
        return true;
    }
    if constexpr (OutlinePixels == 0) {
        const auto glyph = face->rasterize(codepoint, bakedData->size);
        if (!glyph) return false;
        outGlyph->Codepoint = codepoint;
        outGlyph->AdvanceX = static_cast<float>(glyph->advance);
        if (glyph->coverage.empty()) return true;  // a space: advance only

        const ImFontAtlasRectId pack = ImFontAtlasPackAddRect(atlas, glyph->width, glyph->rows);
        if (pack == ImFontAtlasRectId_Invalid) {
            LOG_ERROR("Font atlas full at glyph ", static_cast<unsigned>(codepoint), " of ", src->Name);
            return false;
        }
        ImTextureRect* rect = ImFontAtlasPackGetRect(atlas, pack);
        outGlyph->X0 = static_cast<float>(glyph->left);
        outGlyph->Y0 = static_cast<float>(glyph->top);
        outGlyph->X1 = static_cast<float>(glyph->left + glyph->width);
        outGlyph->Y1 = static_cast<float>(glyph->top + glyph->rows);
        outGlyph->Visible = true;
        outGlyph->Colored = false;
        outGlyph->PackId = pack;
        ImFontAtlasBakedSetFontGlyphBitmap(atlas, baked, src, outGlyph, rect, glyph->coverage.data(),
                                           ImTextureFormat_Alpha8, glyph->width);
        return true;
    } else {
        const auto glyph = face->rasterizeOutlined(codepoint, bakedData->size, OutlinePixels);
        if (!glyph) return false;
        outGlyph->Codepoint = codepoint;
        outGlyph->AdvanceX = static_cast<float>(glyph->advance);
        if (glyph->rgba.empty()) return true;  // a space: advance only

        const ImFontAtlasRectId pack = ImFontAtlasPackAddRect(atlas, glyph->width, glyph->rows);
        if (pack == ImFontAtlasRectId_Invalid) {
            LOG_ERROR("Font atlas full at outlined glyph ", static_cast<unsigned>(codepoint), " of ", src->Name);
            return false;
        }
        ImTextureRect* rect = ImFontAtlasPackGetRect(atlas, pack);
        outGlyph->X0 = static_cast<float>(glyph->left);
        outGlyph->Y0 = static_cast<float>(glyph->top);
        outGlyph->X1 = static_cast<float>(glyph->left + glyph->width);
        outGlyph->Y1 = static_cast<float>(glyph->top + glyph->rows);
        outGlyph->Visible = true;
        outGlyph->Colored = false;
        outGlyph->PackId = pack;
        ImFontAtlasBakedSetFontGlyphBitmap(atlas, baked, src, outGlyph, rect, glyph->rgba.data(),
                                           ImTextureFormat_RGBA32, glyph->width * 4);
        return true;
    }
}

template <int OutlinePixels>
const ImFontLoader* makeLoader(const char* name) {
    static ImFontLoader loader = [name] {
        ImFontLoader l;
        l.Name = name;
        l.FontSrcInit = fontSrcInitT<OutlinePixels>;
        l.FontSrcDestroy = fontSrcDestroy;
        l.FontSrcContainsGlyph = fontSrcContainsGlyph;
        l.FontBakedInit = fontBakedInit;
        l.FontBakedDestroy = fontBakedDestroy;
        l.FontBakedLoadGlyph = fontBakedLoadGlyphT<OutlinePixels>;
        l.FontBakedSrcLoaderDataSize = sizeof(BakedData);
        return l;
    }();
    return &loader;
}

} // namespace

const ImFontLoader* nativeGlyphLoader(int outlinePixels) {
    if (outlinePixels >= 2) return makeLoader<2>("WoW 2.4.3 interface glyphs (THICKOUTLINE)");
    if (outlinePixels == 1) return makeLoader<1>("WoW 2.4.3 interface glyphs (OUTLINE)");
    return makeLoader<0>("WoW 2.4.3 interface glyphs");
}

} // namespace wowee::ui
