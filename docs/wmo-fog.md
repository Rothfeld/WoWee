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
Record zero supplies default WMO fog. Referenced local spheres contribute between
their inner and outer radii; duplicate references do not multiply their weight.
The infinite-radius flag bypasses the distance test. The shared frame UBO receives
the resulting fog, so buildings and characters use the same haze.

Do not use the first group reference unconditionally. Exodar group references
include blue sphere1 even where native displays default purple sphere0; a paired
capture at Tolo exposed that shortcut. Also establish runtime ffxGlow parity before
comparing distant brightness. Exact multi-sphere overlap weighting still needs
native boundary captures; parser tests alone cannot establish visual parity.

Format references:
- https://github.com/wowemulation-dev/warcraft-rs/blob/main/docs/src/formats/graphics/wmo.md
- https://skarndev.github.io/wowlib/python/wmo/root-chunks/
