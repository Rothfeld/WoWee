#include "ui/interface_fonts.hpp"

#include <imgui.h>
#include <cfloat>
#include <cassert>
#include <cmath>

#include "ui/text_markup.hpp"
#include "ui/text_wrap.hpp"
#include "core/logger.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <vector>

namespace wowee::ui {

namespace {
float maximumFontSize = 0.0f;

/// The lower-case stem of a path, with any directory and extension removed.
/// "Fonts\\MORPHEUS.ttf" and "morpheus.ttf" both come out as "morpheus", which
/// is what lets a font object written one way find a file named the other.
std::string faceKey(const std::string& pathOrName) {
    const size_t slash = pathOrName.find_last_of("\\/");
    std::string stem = (slash == std::string::npos) ? pathOrName
                                                    : pathOrName.substr(slash + 1);
    const size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos) stem = stem.substr(0, dot);
    std::transform(stem.begin(), stem.end(), stem.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return stem;
}

std::unordered_map<std::string, ImFont*>& faces() {
    static std::unordered_map<std::string, ImFont*> map;
    return map;
}

struct NativeEntry {
    std::shared_ptr<NativeFace> face;
    ImFont* font = nullptr;
};

std::unordered_map<std::string, NativeEntry>& nativeFaces() {
    static std::unordered_map<std::string, NativeEntry> map;
    return map;
}

const NativeEntry* nativeEntry(const std::string& fontFace) {
    for (const std::string& name : {fontFace, std::string("frizqt__")}) {
        if (name.empty()) continue;
        const auto it = nativeFaces().find(faceKey(name));
        if (it != nativeFaces().end() && it->second.face) return &it->second;
    }
    return nullptr;
}

/// How wide the merged fallback face draws a codepoint the interface face
/// lacks, in pixels at `pixelHeight`.
float fallbackAdvance(uint32_t codepoint, int pixelHeight, const void* ctx) {
    auto* font = static_cast<ImFont*>(const_cast<void*>(ctx));
    if (!font || ImGui::GetCurrentContext() == nullptr) return 0.0f;
    char utf8[5] = {};
    if (codepoint < 0x80) {
        utf8[0] = static_cast<char>(codepoint);
    } else if (codepoint < 0x800) {
        utf8[0] = static_cast<char>(0xC0 | (codepoint >> 6));
        utf8[1] = static_cast<char>(0x80 | (codepoint & 0x3F));
    } else if (codepoint < 0x10000) {
        utf8[0] = static_cast<char>(0xE0 | (codepoint >> 12));
        utf8[1] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        utf8[2] = static_cast<char>(0x80 | (codepoint & 0x3F));
    } else {
        utf8[0] = static_cast<char>(0xF0 | (codepoint >> 18));
        utf8[1] = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
        utf8[2] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        utf8[3] = static_cast<char>(0x80 | (codepoint & 0x3F));
    }
    return font->CalcTextSizeA(static_cast<float>(pixelHeight), FLT_MAX, 0.0f, utf8).x;
}

/// A big-endian field out of a font file, or zero past the end.
///
/// Bounds-checked on every read rather than once at the top: a truncated font
/// is a file this client did not write, and the tables it names are at offsets
/// the file itself supplies.
uint32_t beAt(const uint8_t* d, size_t n, size_t at, int bytes) {
    if (at + static_cast<size_t>(bytes) > n) return 0;
    uint32_t v = 0;
    for (int i = 0; i < bytes; ++i) v = (v << 8) | d[at + static_cast<size_t>(i)];
    return v;
}

} // namespace

void registerInterfaceFace(const std::string& pathOrName, ImFont* font) {
    if (!font) return;
    faces()[faceKey(pathOrName)] = font;
    // The metric tables come from the same bytes ImGui rasterizes: the first
    // source is the face itself, later ones are merged fallbacks.
    NativeEntry entry;
    entry.font = font;
    if (font->Sources.Size > 0 && font->Sources[0]->FontData && font->Sources[0]->FontDataSize > 0) {
        entry.face = NativeFace::fromTrueType(font->Sources[0]->FontData,
                                              static_cast<size_t>(font->Sources[0]->FontDataSize));
    }
    if (!entry.face) LOG_WARNING("Interface face ", pathOrName, " has no readable metric tables");
    nativeFaces()[faceKey(pathOrName)] = std::move(entry);
}

const NativeFace* interfaceNativeFace(const std::string& fontFace) {
    const NativeEntry* e = nativeEntry(fontFace);
    return e ? e->face.get() : nullptr;
}

NativeTextMeasure interfaceTextMeasure(const std::string& fontFace, int pixelHeight) {
    NativeTextMeasure m;
    m.pixelHeight = pixelHeight;
    if (const NativeEntry* e = nativeEntry(fontFace)) {
        m.face = e->face.get();
        m.missingAdvance = &fallbackAdvance;
        m.missingCtx = e->font;
    }
    return m;
}

ImFont* interfaceFace(const std::string& pathOrName) {
    if (pathOrName.empty()) return nullptr;
    const auto it = faces().find(faceKey(pathOrName));
    return (it == faces().end()) ? nullptr : it->second;
}

ImFont* interfaceFaceOrDefault(const std::string& fontFace) {
    // The order the renderer uses. The interface's own default rather than this
    // client's, because ImGui draws with whatever face was added first and that
    // is deliberately the built-in one.
    if (ImFont* f = interfaceFace(fontFace)) return f;
    if (ImFont* f = interfaceFace("frizqt__")) return f;
    return (ImGui::GetCurrentContext() != nullptr) ? ImGui::GetFont() : nullptr;
}

void setInterfaceFontSizeLimit(float maximum) { maximumFontSize = maximum; }

float interfaceFontSize(float fontHeight, float pixelScale) {
    assert(std::isfinite(pixelScale) && pixelScale > 0.0f);
    if (fontHeight > 0.0f) return maximumFontSize > 0.0f ? std::min(fontHeight, maximumFontSize / pixelScale) : fontHeight;
    // What the renderer falls back to: the current size, not a flat twelve.
    return (ImGui::GetCurrentContext() != nullptr) ? ImGui::GetFontSize() : 12.0f;
}

float interfaceTextWidth(const std::string& text, const std::string& fontFace,
                         float fontHeight, float pixelScale) {
    if (text.empty()) return 0.0f;
    const float size = interfaceFontSize(fontHeight, pixelScale);
    if (ImFont* font = interfaceFaceOrDefault(fontFace)) {
        return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x;
    }
    // No context yet - during the FrameXML load there may be no frame in
    // flight to ask. An estimate is far better than nothing: the alternative
    // is answering zero, and MoneyFrame does SetWidth(GetTextWidth() +
    // iconWidth), which then places three buttons on top of each other.
    return static_cast<float>(text.size()) * size * 0.5f;
}

int interfaceTextLines(const std::string& text, const std::string& fontFace,
                       float fontHeight, float wrapWidth, bool nonSpaceWrap, float pixelScale) {
    if (text.empty()) return 0;
    const float size = interfaceFontSize(fontHeight, pixelScale);
    ImFont* font = interfaceFaceOrDefault(fontFace);
    const auto measure = [&](const std::string& piece) {
        if (font) return font->CalcTextSizeA(size, FLT_MAX, 0.0f, piece.c_str()).x;
        // The same estimate interfaceTextWidth falls back on when there is no
        // context yet, so a measure taken during the load does not answer that
        // everything fits on one line.
        return static_cast<float>(piece.size()) * size * 0.5f;
    };
    const auto lines = wrapText(parseMarkup(text), wrapWidth, nonSpaceWrap, measure);
    return lines.empty() ? 1 : static_cast<int>(lines.size());
}

float fontEmSizeScale(const void* ttfData, size_t byteCount) {
    const auto* d = static_cast<const uint8_t*>(ttfData);
    if (!d || byteCount < 12) return 1.0f;

    // A collection points at its first face; a plain font is already there.
    size_t base = 0;
    if (beAt(d, byteCount, 0, 4) == 0x74746366u /* 'ttcf' */) {
        base = beAt(d, byteCount, 12, 4);
        if (base + 12 > byteCount) return 1.0f;
    }

    const uint32_t tables = beAt(d, byteCount, base + 4, 2);
    size_t head = 0, hhea = 0;
    for (uint32_t i = 0; i < tables; ++i) {
        const size_t rec = base + 12 + static_cast<size_t>(i) * 16;
        const uint32_t tag = beAt(d, byteCount, rec, 4);
        if (tag == 0x68656164u /* 'head' */) head = beAt(d, byteCount, rec + 8, 4);
        else if (tag == 0x68686561u /* 'hhea' */) hhea = beAt(d, byteCount, rec + 8, 4);
    }
    if (head == 0 || hhea == 0) return 1.0f;

    const float upem = static_cast<float>(beAt(d, byteCount, head + 18, 2));
    // Signed, and the descender is negative in every font that has one.
    const auto s16 = [&](size_t at) {
        return static_cast<float>(static_cast<int16_t>(beAt(d, byteCount, at, 2)));
    };
    const float span = s16(hhea + 4) - s16(hhea + 6);
    if (upem <= 0.0f || span <= 0.0f) return 1.0f;

    // A face whose span is wildly different from its em is more likely a
    // misread than a real design, and scaling by it would ruin the text rather
    // than improve it. FRIZQT__ is 1.215 and the other four are near it.
    const float scale = span / upem;
    return (scale >= 0.5f && scale <= 3.0f) ? scale : 1.0f;
}

float fontEmSizeScaleOfFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return 1.0f;
    // The tables this reads are in the first few hundred bytes of every font,
    // but the directory can name them in any order, so the file is read whole
    // rather than guessed at. These are a few hundred kilobytes each.
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    return fontEmSizeScale(bytes.data(), bytes.size());
}

} // namespace wowee::ui
