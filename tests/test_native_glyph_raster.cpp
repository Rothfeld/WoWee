// Interface glyph raster rules of the 2.4.3 client (ui/native_glyph_loader.hpp).
//
// Baselines and placements were read off native captures (GlyphProbe addon,
// _experiments/20261119T112856Z.freetype_glyphs/ in the workspace) on a
// 1280x720 window. The faces themselves are not in the repository, so the
// raster check uses an outline written out here.
#include <catch_amalgamated.hpp>

#include <array>
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
