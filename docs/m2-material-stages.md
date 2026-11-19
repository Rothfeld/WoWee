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

Models with global flag 0x08 name each stage's mode in their combiner combos,
read at the batch's shader id (0 Opaque, 1 Mod, 2 Decal, 3 Add, 4 Mod2x), the
same rule as the character renderer: an Opaque first stage drops texture alpha;
a second stage multiplies (Opaque/Mod/Decal), adds weighted by its alpha (Add)
or doubles both colour and alpha (Mod2x). Sunwell Plateau's fire barrier
around the Brutallus arena (GameObject 188075,
`spells/sunwell_fire_barrier_ext.m2`) is Mod then Mod2x over
`FIREWALL COPY 3.BLP` and `T_VFX_BORDER6/7.BLP`; plain modulation burned it at
half brightness, dark brown against native's yellow-orange. Paired captures:
`_experiments/20261118T093139Z.brutallus_fire_wall` in the workspace. Shader ids
with bit 0x8000 (0xFFFE, 0xFFFD: Tempest Keep crystals, the Caverns of Time
hourglass) keep modulating both stages; their rule is not established. UV
rotation/scaling tracks are not implemented.

## Multiply blend modes and batch order

M2 material blend modes 5 (Mod) and 6 (Mod2x) multiply the framebuffer by the
unlit texture (`dst * src`, `dst * src * 2`), fogged and faded toward the
factor that changes nothing (1, 0.5), as in the character renderer. 135 TBC
world M2s use them: Magisters' Terrace's scrying orb (`BE_ScryingOrb.m2`, MODD
955 by Kael'thas's throne: ball and both `ORBREFLECT.BLP` layers Mod2x), the
corrupted Bloodmyst crystals, Hellfire, Ogri'la and Shadowmoon crystals,
Hellfire Citadel floor braziers. Drawn additively the scrying orb was a
pink-white ball on a pale stand; 2.4.3 shows a deep red ball on a dark gold
stand.

A model's blended batches are drawn far to near by their submesh sort point
(TBC submesh `+0x20` sortCenter, carried by its center bone `+0x12`), stable for
batches of one submesh. The scrying orb's additive `SHOCKWAVE10.BLP` halo sits at
its billboard's origin, 0.112 behind the Mod2x dome's sort point: native
multiplies the halo into the red ball; in skin order it lay on top as a white
ring. Whether native ranks priority plane above this order is not observed.
Paired captures (pair 23):
`_experiments/20261119T1400Z.red_orb/` in the workspace.
