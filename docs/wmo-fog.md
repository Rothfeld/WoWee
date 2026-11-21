# Authored WMO fog

Run `./build/bin/test_open_formats '[wmo-fog]'` for parser and volume sampling checks.

MFOG records contain a local-space sphere, BGRA regular/underwater colors,
fog end distances and start multipliers. Fog starts at end times multiplier;
negative underwater multipliers are valid authored values. MOGP references up to
four records at header offset48. Offset52 is the liquid type.

WMORenderer retains the records and group references. The camera's containing
indoor group selects candidate volumes, using the instance inverse transform.
A containing group with MOGP 0x40 (exterior lit) takes Light.dbc fog instead:
Black Temple's open courtyard (blacktemple.wmo group 8, 0x43a41) shows the
map's dark green Light 592 fog natively, not its teal MFOG record 0.
Record zero supplies default WMO fog when authored (`largeRadius > 0` or the
`0x1` infinite-radius flag, present in 170 of TBC's 1,204 root WMOs). In the
remaining 1,034 root WMOs where no global WMO fog was placed, the WMO compiler
writes an identical zero-radius white stub in record zero (`flags = 0`,
`smallRadius = largeRadius = 0`, `endDist = 444.44`, `color1 = 0xFFFFFFFF`),
while 37 of those WMOs (`wailingcaverns.wmo`, `md_mushroomcave02`..`10`,
`md_icecave02`..`04`, `md_junglecave07`, `md_goldmine_variantb`/`c`/`f`,
`mini-hive_a`..`f`, `kl_diremaul.wmo`, `monestary_cathedral`/`cemetery`,
`stormwind.wmo`, `darnassis.wmo`) author local fog spheres in records `1..N`.
Referenced local spheres contribute between their inner and outer radii even
when record zero is that zero-radius stub, blending from the outdoor `Light.dbc`
fog across their falloff shell and returning `nullopt` outside their outer
radii; duplicate references do not multiply their weight. The infinite-radius
flag bypasses the distance test.

The sampled fog is the fog of interior surfaces only. The frame UBO's
`fogColor`/`fogParams` carry it while the camera is in such a group, and
`outdoorFogColor`/`outdoorFogParams` keep the Light.dbc fog. A surface takes the
camera's fog when it is lit as an interior: WMO groups with `isInterior`, M2s
with an interior light (WMO doodads, game objects standing in such a group),
units with `interiorLit`, and liquids of interior groups (`push.interiorFog`).
Terrain, grass, outdoor doodads, exterior groups and outdoor liquids take the
outdoor fog. Particles, ribbons and the sky still read `fogColor`/`fogParams`.

An outdoor M2 (one without an interior light) is not drawn past the outdoor
fog's end (`m2.frag.glsl` discards there); the sky shows instead. Terrain, WMOs
and units are still drawn past it, fully fogged.

Native evidence, build 8606, `_experiments/20261119T084535Z.bt_cyan_planes/` in
the workspace (pair 24, `farclip` raised to 1277 unless noted):
- Black Temple, stairs up to Illidan's door (blacktemple.wmo group 23, teal
  MFOG record 0, end 417; open the gate with `.gobject activate 5640021 185905`):
  sky through the roof openings, where WoWee drew the floating rocks
  (shadowmoon_overhangrock_large_01, 509 yd) as flat teal shapes, and with
  the outdoor fog as flat dark olive ones (outdoor fog end 389).
- Black Temple, Illidan's terrace: sky where WoWee drew the same rocks as flat
  dark olive shapes.
- Echo Ridge Mine entrance (md_goldmine.wmo, MFOG ends 69 and 200): Northshire's
  trees outside keep their colours through the entrance.
- Garadar, Nagrand: at `farclip` 177 the floating island over the town is gone
  while the mountains behind it stay; at 1277 both are drawn. Clipping terrain at
  WoWee's outdoor fog end as well removed those mountains, so only M2s are cut.

Gotchas: `DistCull` is clamped at 500 and neither it (1 to 500) nor `SmallCull` 0
changed what 8606 drew in these views. Typing into the native chat right after
another line can drop the first character, so `/console ...` becomes a /say.

Do not use the first group reference unconditionally. Exodar group references
include blue sphere1 even where native displays default purple sphere0; a paired
capture at Tolo exposed that shortcut. Also establish runtime ffxGlow parity before
comparing distant brightness. Exact multi-sphere overlap weighting still needs
native boundary captures; parser tests alone cannot establish visual parity.

Format references:
- https://github.com/wowemulation-dev/warcraft-rs/blob/main/docs/src/formats/graphics/wmo.md
- https://skarndev.github.io/wowlib/python/wmo/root-chunks/
