# WMO lighting and face orientation

WMO group visibility and lighting are separate. MOGP flag `0x2000` marks an
interior group; `0x40` disables local lighting for that group. Stormwind street
and canal groups combine both flags. `pipeline::wmoUsesLocalLighting` applies
this override to WMO materials, doodad placement colors, and liquid lighting.
Interior portal visibility continues to use the visibility flags.

Examples from original TBC assets:

| Group | Flags | Lighting |
| --- | --- | --- |
| Stormwind thief01 | 0x2a41 | Exterior |
| Stormwind canal505 | 0xb841 | Exterior |
| Stormwind AuctionHouse | 0x2a05 | Local |
| Ironforge interior groups | 0x2805 | Local |

Flag definitions are also recorded in
[pywowlib's MOGP flags](https://github.com/wowdev/pywowlib/blob/master/file_formats/wmo_format_group.py).

The main WMO, M2 and character pipelines declare counter-clockwise front faces
under the camera's Vulkan projection. This matters even when culling is off:
fragment shaders use `gl_FrontFacing` to orient two-sided lighting normals.

WMO materials without MOMT flag `0x04` (F_UNCULLED) draw front faces only.
The water reflection pass, whose mirrored view flips winding, draws every WMO
surface two-sided.

From outside a building (camera in no interior-only group box, or also in an
exterior group box) WoWee draws every exterior group, interior groups whose box
holds the camera or the character, and interior groups reached through portals
that are in the view frustum and face the camera. A portal faces the camera when
`dot(MOPT normal, camera) + MOPT distance` has the sign of the MOPR `side` of the
group it is entered from. This walk runs without `WOWEE_WMO_CULL`; inside a
building the portal walk stays behind that switch. WMO doodads listed only by
interior groups (MODR) are drawn while one of those groups is drawn.

Units are not drawn when their room is not reached by the portal walk, which
runs indoors too for this (`WMORenderer::roomHides`). A unit's room is the
interior-only group whose box holds it and whose surface is first below it.
Inside a building the walk starts from the best-fit box groups of the camera
and the character and from their rooms by that same surface rule. The two
differ in Dire Maul West's Court of the Highborne (`KL_Diremaul_Instance.wmo`,
around 26, 507, -3.57): the walkway's best-fit box is group 80, but its floor
belongs to group 72. Starting from group 80 alone, the Eldreth units on that
floor vanished once the camera pitched down past the 80 -> 72 portal.
Evidence: `../_experiments/20261118T1800Z.tiltcull/`.

GM Island (map 1, around 16240 16290) shows all three: `guildhousea.wmo`'s
ground-floor rooms and their furniture behind the lodge are not drawn from the
courtyard, and the exterior group's courtyard-facing wall backs are see-through.
Evidence: `../_experiments/20261117T2215Z.gmisland_selection_lua/` (pair-gm_d*.png).

A WMO doodad is lit by its MODD colour only when every group whose MODR lists
it uses local lighting (`pipeline::wmoDoodadUsesLocalLighting`); one listing
group without local lighting gives it the outdoor light. 547 TBC doodads are
listed by both kinds. Booty Bay's north entrance (`bootybay.wmo` MODD 3,
`BootyBayEntrance_02.m2`: beams, Jolly Roger, shark jaw, lantern) has MODD
colour (0,0,0) and groups 22 (0xb805, the tunnel) and 42 (0x8849). 2.4.3 draws
it with the cape's night light, seen from the cape and from inside the tunnel;
taking the first local group's colour made it a black silhouette with only the
unlit lantern showing. Torch MODD 853 (colour (255,161,140), groups 29 0x849
and 30 0x2805) is dark green at night in 2.4.3, not orange-red. Interior-only
doodads keep the colour: the sacks of group 32 (MODD 943, 946, 950: colours
near (0,0,0)) are black in 2.4.3. Evidence:
`../_experiments/20261118T223141Z.black_mast/`.

Units and game objects standing in a locally lit interior group take the
floor's MOCV under their feet (the closest WMO surface below, the group's box
holding them), floored by the MOHD ambient, in place of the outdoor light.
Units use it as their light; game objects use it as interior doodads use their
MODD colour. Shattrath's Terrace of Light braziers (`ANCIENT_D_STANDING_LIGHT.m2`,
group 1, MOGP 0xb805) are purple there, not the green of outdoor Light 458; a
unit beside one measures 0.9x the MOCV (178,153,231) under it. Not modelled:
native lit sides of units reach about 1.8x MOCV, and game objects are about
0.45x MOCV natively (WoWee about 1.3x brighter, as for interior doodads).
Groups with their own lights (MOGP 0x200) do not light units this way: units
there keep the outdoor light. Tempest Keep's hall01 (tk_raid group 2, 0x2a05,
MOCV red 0.6-0.9/0.3-0.5/0.3-0.6): native skin 134/93/75, WoWee with the MOCV
78/26/22, without 121/97/82; the Warglaive of Azzinoth's additive glow green
214 native, 151 then 175. Two groups support the rule (this and Shattrath's
0xb805), no more. Evidence: `../_experiments/20261118T121312Z.glaive_glow/`.

Native unit light in an interior is not the floor MOCV times a gain (open; WoWee
still uses the MOCV). Measured on Shattrath's Terrace of Light (group 1,
0xb805) by fitting native/texel = A + D*max(N.l,0) per pixel, l the TBC sun
direction, texel and normal from debug-shader renders at the held native camera:

| unit at (-1834, y) | floor MOCV | A (ambient) | D (sun) |
|---|---|---|---|
| y 5486 | (52,61,105) | 0.32/0.40/0.59 | 0.02/0.01/0.10 |
| y 5492 | (70,74,123) | 0.36/0.40/0.70 | 0.20/0.23/0.28 |
| y 5498 | (149,132,205) | 0.42/0.39/0.62 | 0.20/0.15/0.12 |
| y 5504 | (188,159,239) | 0.42/0.36/0.54 | 0.39/0.34/0.41 |
| (-1831, 5500), two facings | (170,148,225) | 0.46/0.39/0.63 | 0.44/0.34/0.25 |

- The ambient stays near (0.38,0.39,0.6) while the MOCV under the feet triples;
  it is not the floor MOCV, its alpha (0 along the ramp), a group boundary (all
  points in groups 0/1/2, one floor) or a higher sample point (flat floor).
- A sun term appears and grows toward the brazier end of the room; it is warm,
  like the map-wide Light row (374, params 379) diffuse, not the local Light 458.
  The Spectral Realm's map-wide row 1400 uses the same params, which fits
  Sathrovarr's warm lit side there.
- What a unit's camera sees decides which side shows (lit or ambient); native
  does not switch light by camera position (36-step orbit, fixed unit).
- The authored M2/MOLT lights are not the source: shattrathcity.wmo has no
  MOLT, its group 1 doodads and the naaru carry no M2 lights.
- Fits explain only 0.0-0.3 of the variance (one spot is unusable); raw native
  pixel tracks along the ramp agree on the trend: hair 1.4/1.09/0.9x while MOCV
  rises 3.6/2.6/2.3x.

Not done: a second building to confirm the sun colour source, and a creature
next to the player in one frame (in the Spectral Realm native creatures look
sun-lit while the native player looks MOCV-dark). Evidence:
`../_experiments/20261118T1050.interior_unit_light`, `..1120.camera_light_selection`,
`..1130.mocv_sample`, `..1140.ramp_fit`, `..1110.m2_unit_lights`.

TBC uses authored textures, vertex colors and environment lighting without
the extra point lights synthesized from fixture names, glow meshes or lava.
Those synthetic lights are not authored light records.

TBC window surfaces also retain their authored MOMT blend modes and normal
material shading. Texture names do not enable the synthetic glass pipeline,
its Fresnel reflections, or its view-dependent opacity. For example,
`MM_STRMWND_WINDOWS_01` is painted window artwork, not a request for transparency.
The ray-tracing geometry uses the same rule: opaque windows remain occluders.

TBC specular highlights follow the MOMT shader variant. Diffuse stone, paving,
wood, and cloth do not receive the shared pipeline's specular term. Batch merging
keeps diffuse and specular materials separate even when they share a texture.
The Specular variant (shader 1) uses texture alpha as its highlight mask.
Its TBC highlight is evaluated at vertices with exponent 8 and the authored
sun color (LightIntBand channel 9). Primary vertex lighting is saturated before
interpolation. The diffuse direction and color remain separate from the
specular color. M2 exterior primary lighting uses the same vertex evaluation;
TBC omits synthetic foliage color variation, canopy darkening and replacement
lamp halo sprites. Authored glow meshes remain visible.
Alpha-blended M2 cards retain texture alpha even when texture classification
detects dark texels. Color-key opacity replacement applies only to opaque and
cutout materials. `torch.m2` uses `gloworange32.blp` with blend mode 2.
For example, `MM_STREET_02.BLP` has mean alpha 19.75/255; treating its mask as
fully white produces broad orange highlights across the street.
Name-based lamp/clock emission boosts and flicker are disabled for TBC as well;
these surfaces use their authored material shading.

TBC scene glow uses LightParams field 4, blended with the active light volumes.
It composites `scene + glow * blurredScene * blurredScene` before UI drawing.
The Gaussian footprint is an approximation measured against native captures;
the material and glow terms are independent. `/console ffxGlow 0` disables it,
and `/console ffxGlow 1` enables it. The CVar persists between runs and defaults
to enabled. Glow does not implicitly enable FXAA. For Stormwind profile 79,
the authored glow amount is 0.3.

Matched-camera captures at two zoom distances, native glow-on/off isolation,
shader diagnostics and build logs are in the parent workspace experiment
`_experiments/20261116T101556Z.stormwind_lighting_parity`.
Native command injection must use physical base keys plus Shift for punctuation;
X11's alternate parenthesis keycodes are ignored by Wine.

Comparison artifacts and asset inspection script:
`_experiments/20261116T094629Z.stormwind_shading` in the parent workspace.
Run `python _experiments/20261116T094629Z.stormwind_shading/inspect_assets.py`
there to inspect group flags, doodad references and authored triangle winding.
