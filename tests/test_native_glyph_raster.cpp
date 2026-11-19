// Interface glyph raster rules of the 2.4.3 client (ui/native_glyph_loader.hpp).
//
// Baselines and placements were read off native captures (GlyphProbe addon,
// _experiments/20261119T112856Z.freetype_glyphs/ in the workspace) on a
// 1280x720 window. The faces themselves are not in the repository: the
// bitmap check reads them from WOWEE_TBC_DATA_DIR/fonts
// (WOWEE_TBC_DATA_DIR=client/wowee-data/expansions/tbc).
#include <catch_amalgamated.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "ui/ft2110_grays.h"
#include "ui/native_glyph_loader.hpp"

using namespace wowee::ui;

TEST_CASE("baseline row per face and pixel height, as 2.4.3 draws it", "[glyph_raster]") {
    struct Face { int ascender, descender; std::array<int, 11> baseline; };
    // hhea ascender/descender; rows from line top to baseline at 9, 10, 11, 12, 13, 14, 15, 16, 18, 20, 24 px
    const Face frizqt{917, -270, {8, 9, 9, 10, 11, 12, 13, 13, 15, 16, 20}};
    const Face arialn{1916, -434, {8, 9, 10, 11, 12, 12, 13, 14, 16, 17, 21}};
    const Face skurri{760, -299, {7, 8, 9, 10, 10, 11, 12, 12, 14, 15, 18}};
    const Face morpheus{2425, -512, {8, 9, 10, 11, 12, 13, 13, 14, 16, 18, 21}};
    const std::array<int, 11> heights{9, 10, 11, 12, 13, 14, 15, 16, 18, 20, 24};
    for (const Face& f : {frizqt, arialn, skurri, morpheus}) {
        for (size_t i = 0; i < heights.size(); ++i) {
            INFO("ascender " << f.ascender << " height " << heights[i]);
            CHECK(nativeBaselineRow(heights[i], f.ascender, f.descender) == f.baseline[i]);
        }
    }
}

TEST_CASE("glyph bitmap starts at ceil(xMin) - 1 from the pen", "[glyph_raster]") {
    CHECK(nativeGlyphLeft(33) == 0);    // FRIZQT "B" at 12 px
    CHECK(nativeGlyphLeft(-38) == -1);  // "A"
    CHECK(nativeGlyphLeft(127) == 1);   // "1"
    CHECK(nativeGlyphLeft(0) == -1);    // SKURRI "T": on a pixel edge, one pixel left
    CHECK(nativeGlyphLeft(-64) == -2);  // FRIZQT "A" at 20 px
    CHECK(nativeGlyphLeft(64) == 0);    // ARIALN "O" at 24 px
}

TEST_CASE("coverage is kept to 4 bits", "[glyph_raster]") {
    CHECK(nativeCoverage(0) == 0);
    CHECK(nativeCoverage(15) == 0);
    CHECK(nativeCoverage(16) == 17);
    CHECK(nativeCoverage(229) == 14 * 17);
    CHECK(nativeCoverage(255) == 255);
}

TEST_CASE("FreeType 2.1.10 gray raster renders an outline", "[glyph_raster]") {
    // A square from (0.5, 0.5) to (2.5, 2.5) px in a 3x3 bitmap: quarter
    // pixels at the corners, half pixels on the edges, a full one inside.
    const std::vector<long> xy{32, 32, 32, 160, 160, 160, 160, 32};
    const std::vector<char> tags{1, 1, 1, 1};
    const std::vector<short> contours{3};
    std::vector<unsigned char> coverage(9, 0);
    REQUIRE(wowee_ft2110_render(1, 4, xy.data(), tags.data(), contours.data(), 0, coverage.data(), 3, 3) == 0);
    const std::vector<unsigned char> expected{64, 128, 64, 128, 255, 128, 64, 128, 64};
    CHECK(coverage == expected);
}

namespace {

/// FNV-1a over everything the loader hands ImGui for one face at one size.
struct Fnv {
    uint64_t h = 1469598103934665603ull;
    void bytes(const void* p, size_t n) {
        const auto* c = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i) h = (h ^ c[i]) * 1099511628211ull;
    }
    void i32(int32_t v) { bytes(&v, sizeof v); }  // little-endian, as recorded
};

} // namespace

TEST_CASE("interface face glyph bitmaps at 9..24 px are unchanged", "[glyph_raster][assets]") {
    // Recorded from the loader that read the outlines through FreeType 2.13.2
    // (FT_LOAD_NO_HINTING) into the same 2.1.10 rasterizer: every codepoint
    // of the face's cmap, its advance, placement, rows and 4-bit coverage.
    // Recorder: _experiments/20261119T122614Z.nofreetype_glyphs/ref_dump.cpp.
    struct Face { const char* file; std::array<uint64_t, 16> hash; };
    const std::array<Face, 4> faces{{
        {"frizqt__.ttf", {
        0xbb5344380ffb25eeull, 0x45da1150c55a8ebeull, 0x87446a006e7769b2ull, 0x0f5893bfcb4aeedcull,
        0xe74af7d2df7cc51dull, 0x0e2a1c30fcd32196ull, 0x7062b2dfdbf8f730ull, 0x5baeec9b5fcf3bccull,
        0x4df2c18c9fb6cf5full, 0xf775e875edaf9362ull, 0xf2dbb2899b4f186eull, 0xad8c7f0259eaab64ull,
        0x7c79c1a3e1c2a0c8ull, 0x801373dac4f51502ull, 0x786a1c89b3e25c18ull, 0x91b47478c8c9293aull,
        }},
        {"arialn.ttf", {
        0x5524c4fe5021464aull, 0x6bf414cbcf6ff8a3ull, 0xf7c5034848c69ec5ull, 0x307c929718602890ull,
        0x04c1571d51987198ull, 0x9ac8fb7f77440db4ull, 0x4dcfd6ee547cb495ull, 0xf47c1f9645ca78c7ull,
        0x760f26634e7ccf33ull, 0x954f3004414c31dfull, 0xc71df1c0a21821edull, 0xf29bb1c00d8af6c7ull,
        0xe750036c147f69feull, 0x6af03269463da874ull, 0x7963100ec4aa021bull, 0xa873bcb328c42482ull,
        }},
        {"skurri.ttf", {
        0x5e84ffc10b8bd246ull, 0x34ea21de76431e34ull, 0x98018e6ce4279081ull, 0xe9216c56766fa411ull,
        0x0d23ea0e534c55ebull, 0xbce394064b7a3d02ull, 0xaf34deda2940c429ull, 0x1c7485e56c4c7f47ull,
        0x59517f94b8e98a0aull, 0xeda1cb530a3ff29cull, 0x46c237807b18a2c9ull, 0x9f94125f3b28ec99ull,
        0x48c88b091c8e34aaull, 0x8fc24bddffe8a69eull, 0xf9ffd775c29c51b3ull, 0x02a9c676f53e8670ull,
        }},
        {"morpheus.ttf", {
        0x55227e76cbd3d19cull, 0xc42411da05dc30ebull, 0x50523d33a0b45831ull, 0x3936e51b2f88ac07ull,
        0x57b278f057ecfb21ull, 0x0a30b5ee03107490ull, 0x78eb9ae691ee1ffcull, 0x7eb285c403328f37ull,
        0x0cc3b02c87e36eadull, 0x7fb6ba8835bf4d12ull, 0xecede6f3cbd11e25ull, 0xa75c6fd0565aae3dull,
        0xd3bc35fede3fb8feull, 0x003f4b2c6b81651eull, 0x33cbf5c24e820be4ull, 0xc8f1cf9ef78d59b0ull,
        }},
    }};
    const char* root = std::getenv("WOWEE_TBC_DATA_DIR");
    if (!root || !*root) SKIP("Set WOWEE_TBC_DATA_DIR to client/wowee-data/expansions/tbc");
    for (const Face& f : faces) {
        std::ifstream in(std::filesystem::path(root) / "fonts" / f.file, std::ios::binary);
        const std::vector<unsigned char> font{std::istreambuf_iterator<char>(in), {}};
        REQUIRE_FALSE(font.empty());
        const auto face = NativeGlyphFace::open(font.data(), font.size(), 0);
        REQUIRE(face);
        for (int px = 9; px <= 24; ++px) {
            Fnv fnv;
            fnv.i32(px);
            fnv.i32(nativeBaselineRow(px, face->ascender(), face->descender()));
            for (uint32_t cp = 0; cp <= 0xFFFF; ++cp) {
                if (!face->contains(cp)) continue;
                fnv.i32(static_cast<int32_t>(cp));
                const auto glyph = face->rasterize(cp, static_cast<float>(px));
                REQUIRE(glyph);
                fnv.i32(glyph->advance);
                if (glyph->coverage.empty()) {
                    fnv.i32(0);
                    continue;
                }
                fnv.i32(1);
                fnv.i32(glyph->left);
                fnv.i32(glyph->top);
                fnv.i32(glyph->width);
                fnv.i32(glyph->rows);
                fnv.bytes(glyph->coverage.data(), glyph->coverage.size());
            }
            INFO(f.file << " at " << px << " px");
            CHECK(fnv.h == f.hash[static_cast<size_t>(px - 9)]);
        }
    }
}
