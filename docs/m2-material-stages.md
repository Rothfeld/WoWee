# M2 texture stages

`build/bin/test_m2_ribbon_tracks '[m2]'` checks texture-coordinate lookup decoding
alongside M2 track and particle-layout tests.

The doodad renderer supports two-texture modulation. Each stage resolves its
texture and coordinate selector through the model's combo tables. Selectors 0
and 1 use the respective mesh UV sets; -1 uses view-space normal coordinates.
Each stage samples its own UV translation track. Static models can carry a zero
transform combo without any transform tracks; this represents identity mapping.

`narru_crystal_pure.m2` combines `ENERGY_SCROLL_ALPHA.BLP` with
`ENERGY_SCROLL2.BLP`. Sampling the mask alone produces white Exodar pillars.
The second texture contains their purple pattern. Native and WoWee captures at
map 530, (-4099.21, -11522.9, -135.623) verify the resulting pillar surface.

Texture-stage support here applies to M2 doodads. It does not add a general
implementation of every M2 shader combiner or UV rotation/scaling track.
