# Legacy WMO water

TBC indoor WMO water uses `XTextures/river/lake_a.1.blp` through
`lake_a.30.blp`, with one repeat per 4.1666665-yard liquid grid tile.
The sequence lasts 1.25 seconds. Surface RGB is MOMT color2 plus lake RGB
plus one quarter of lake alpha. Opacity interpolates LightParams river shallow
and deep alpha using the authored MLIQ vertex depth byte.

Exterior TBC WMO water uses LightIntBand channel 17 (deep river color),
multiplied by clamped flat-surface vertex lighting. Texture RGB and texture-alpha
highlights add to that body color; the alpha highlight includes the same 0.25
term as indoor water plus a vertex sun-specular term. Lighting volumes blend the
river color spatially and temporally. Both WMO paths preserve the flat authored
mesh and use texture animation and contact decals for surface motion.

TBC terrain water uses an authored shallow/deep color ramp and opacity ramp.
Its MCLQ liquid kind selects river `lake_a` or ocean `ocean_h` animation frames
and the matching LightIntBand colors. Both sequences have 30 frames. Terrain
depth bytes interpolate the ramp; zone names do not determine liquid kind.
Cliffspring River, for example, contains ocean liquid. Geometry retains its
authored heights. Ocean terrain does not use procedural Gerstner waves or foam.
See [water lighting fields](water-lighting.md) for table layout and blending.
Cliffspring comparison evidence is in workspace experiment
`_experiments/20261116T223600Z.cliffspring_water/`. Six-frame captures verify
gray translucent water and authored detail. Open-water luminance remains about
10 byte levels below native in that scene; exact lighting parity is unverified.

Stormwind canal comparison: six matched-camera native/WoWee captures, including
water-patch measurements, live in workspace experiment
`_experiments/20261116T105955.187294Z.stormwind_water/`. This validates that WMO
exterior path; ADT oceans and rivers use their own depth-color ramp.

These rules come from build 8606 shader assets `psLiquidWater.bls` and
`vsLiquidWater.bls`, WMO setup at 0x695e90, UV generation at 0x6A77F0,
texture animation at 0x69EAA0, and depth lookup construction at 0x69B6F0.
Disassembly and comparisons live in workspace experiment
`_experiments/20261116T090238.015332Z.native_water_contact`.

Contact effects use original additive splash and wake textures. Stationary
actors emit rings; translating actors emit directional wakes. The wake texture
tip at V=0 points along travel, with its arms trailing behind. Effects remain
at their emission positions, grow, and expire. Current timing uses central
values from native ranges: 0.65-second lifetime and 0.425-second idle interval.
Moving emission interval is `0.25 * depthScale * 2.5 / min(speed, 20)` seconds,
with speed in yards per second. At shallow depth this gives 0.625-yard spacing
up to the speed cap. Overdue emissions retain their age and interpolated
position, so frame delays do not discard wakes or stack them at one point.
Set `WOWEE_WATER_CONTACT_TRACE=1` when launching the client to log each
emission's actor, speed, interval, depth, position, age, and active count.
Exact contact size and visual parity still require paired footage review.
CPU checks cover rings, wakes, multiple actors, expiry, dry/submerged suppression,
and despawn. Surface captures show fine ripples and visible submerged legs.

MLIQ material indices address MOMT, not LiquidType. Preserve authored vertex
heights and depths. Group flag 0x2000 identifies interiors. Missing liquid data
or failed texture uploads fail explicitly. Animation descriptors are immutable
and shared across surfaces; frame changes select another descriptor.

## Draw order

Water drawn in the scene pass goes after the opaque world (terrain, WMOs,
opaque M2 batches, solid characters) and before the blended world models
(M2 pass 2, blended character layers, particles, ribbons). Blended batches
write no depth, so water drawn after them covered whatever stood in it.
Magisters' Terrace's energy cubes (`FactoryEnergyCube.m2`, MODD 837/838 of
`sunwell_5man.wmo`, two-sided alpha glass without depth write) stand in the
Observation Grounds' WMO liquid (`sunwell_5man_003.wmo`, height -20.10, half a
yard above the cube base). With the liquid laid over the faces behind it the
lower cube was pale see-through pink; 2.4.3 shows it uniformly vivid magenta.
Paired captures: `_experiments/20261119T1454Z.purple_cubes/` in the workspace.
Water drawn in the refraction continuation pass (`waterDrawsInContinuePass`:
MSAA without an off-screen scene target, or a single-sampled continuation pass)
still comes after the blended models.
