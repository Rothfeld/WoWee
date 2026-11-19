#pragma once

// FreeType 2.1.10's anti-aliasing rasterizer (src/smooth/ftgrays.c), built
// standalone from third_party/freetype-2.1.10-grays.
//
// The 2.4.3 client draws interface glyphs with an unhinted FreeType of that
// era. Today's FreeType (2.13) loads the same outlines, but its rasterizer
// splits curves differently: inside a bowl ("O", "6", "e") coverage comes out
// up to one 4-bit level higher. Outlines loaded by the system FreeType and
// rendered here are bit-identical to FreeType 2.1.10 (all 2728 glyphs of
// FRIZQT__, ARIALN, SKURRI and MORPHEUS at 9..24 px).
//
// Not thread-safe: the standalone build keeps one static raster.

#ifdef __cplusplus
extern "C" {
#endif

/// Rasterize an outline into an 8-bit coverage bitmap, row 0 at the top.
/// `xy` holds n_points (x, y) pairs in 26.6 pixels, already moved so the
/// bitmap's bottom-left corner is the origin; `tags` and `contours` are the
/// outline's own. `buffer` is width * rows bytes and must be zeroed.
/// Returns 0 on success, the rasterizer's error code otherwise.
int wowee_ft2110_render(short n_contours, short n_points, const long* xy, const char* tags,
                        const short* contours, int even_odd, unsigned char* buffer, int width,
                        int rows);

#ifdef __cplusplus
}
#endif
