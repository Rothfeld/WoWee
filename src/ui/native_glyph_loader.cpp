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

bool fontSrcInit(ImFontAtlas*, ImFontConfig* src) {
    assert(src->FontLoaderData == nullptr);
    auto face = NativeGlyphFace::open(src->FontData, static_cast<size_t>(src->FontDataSize), src->FontNo);
    if (!face) {
        LOG_ERROR("Cannot open interface face ", src->Name);
        return false;
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
    // A size is an em size here (see nativeGlyphLoader). Interface text is
    // drawn at whole pixel heights; other callers may ask for a fraction, and
    // the outline is scaled to it exactly while the row rules use the
    // nearest whole height.
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

bool fontBakedLoadGlyph(ImFontAtlas* atlas, ImFontConfig* src, ImFontBaked* baked, void* loaderData,
                        ImWchar codepoint, ImFontGlyph* outGlyph, float* outAdvanceX) {
    const NativeGlyphFace* face = faceOf(src);
    const auto* bakedData = static_cast<const BakedData*>(loaderData);
    if (outAdvanceX) {
        assert(outGlyph == nullptr);
        const auto advance = face->advance(codepoint, bakedData->size);
        if (!advance) return false;
        *outAdvanceX = static_cast<float>(*advance);
        return true;
    }
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
