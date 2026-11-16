# Terrain liquid coverage

Run `./build/bin/test_water_mask`.

MCLQ and MH2O coverage is normalized into a chunk-wide 8x8 bitmap. Bit order is
row * 8 + column, least-significant bit first. Every liquid type respects that
bitmap, including sea-level ocean. The absent bitmap case fills only the layer's
declared rectangle.

Mesh merging changes storage and draw count, not which cells contain liquid.
Only vertices used by authored wet cells contribute heights. Dry MCLQ vertices
may contain FLT_MAX and must not influence adjacent wet vertices.

The original TBC `kalimdor_30_24.adt` ocean chunk (3,6), at Zoram Strand, has
bitmap bytes `00 00 00 0f 7f ff ff ff`: 35 wet cells. Its northern beach rows are
dry even where terrain lies near sea level. Drawing all 64 cells and relying on
depth testing creates triangular water patches on that beach. The regression
uses those original bytes and checks merged and individual coverage agreement.

If an ocean hole appears, inspect the original mask, axis conversion and vertex
ownership. Do not fill omitted cells to conceal an unrelated projection defect.
