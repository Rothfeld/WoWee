# FreeType 2.1.10 gray rasterizer

`ftgrays.c`, `ftgrays.h` and `ftimage.h` from FreeType 2.1.10 (tag VER-2-1-10,
commit 765cb0fa70cff3829f7902169a1e3eb65d390000, https://github.com/freetype/freetype),
unmodified. License: FreeType Project License, `FTL.TXT`.

Built standalone (`_STANDALONE_`, `ft_grays_raster` renamed to
`wowee_ft2110_grays_raster`) into `wowee_ft2110_grays`; `src/ui/ft2110_grays.c`
feeds it outlines loaded by the system FreeType. The 2.4.3 client's interface
glyphs match this rasterizer's coverage, not current FreeType's (curves are
flattened differently). See README.md, "Interface glyph raster".
