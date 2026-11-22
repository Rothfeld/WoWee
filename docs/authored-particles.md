# Authored M2 particle motion

Run focused checks from the WoWee directory:

```sh
build/bin/test_m2_ribbon_tracks '[m2][particle]'
```

Particle attachment follows flags, independently of emitter shape:

- `NoTrail` (`0x10`) keeps living particles attached to the current emitter bone.
- Without `NoTrail`, particles retain the emitter pose at birth.
- `ModelSpace` (`0x80`) keeps those trailing particles relative to the placed
  model. Otherwise their birth positions remain in world space.
- `0x20` is not a coordinate-space flag; it sizes sprites (below).
- `0x4000` keeps trailing particles in the placed model's frame, like
  `ModelSpace` (285 TBC emitters). Zul'jin's claws (item 33975,
  `Hand_1H_ZulAman_D_01Right.m2`, 0x4429/0x4629) keep their frost wisps on the
  claw through swings and jumps in the original; in world space they left a
  trail and a column behind each jump. Shaman_Frost (0x209) and Widow's
  Remorse's smoke (0x22d) do trail natively. Whether 0x4000 particles also
  follow an animated emitter bone is not observed.

These names are documented in
[wowdev/pywowlib's particle flags](https://github.com/wowdev/pywowlib/blob/master/enums/m2_enums.py).
InstancePortal (`0x39`) follows its rotating emitter. AshenvaleWisps (`0x29`)
and the spline rune (`0x229`) retain separate birth poses.
DR_DRAENEI_HOLORUNES02_BIG.M2 uses zero emission speed and animated parent
bones to draw its traveling floor pattern.
A birth inside a frame starts where the emitter was at that instant, moving
linearly from its previous frame pose: at 8 fps, placing a frame's births at
the current pose drew these floor lines as beads. Shattrath's game-object
runes (DR_Draenei_HoloRunes02/03.m2, GENERIC) are in state READY without
Close/Closed sequences; they keep looping Stand, since only stateful object
types (doors, chests, buttons) show READY through the bind pose.
Detached particles (neither NoTrail nor ModelSpace) take their birth point
from the scaled placement. With flag `0x8` they move at their authored speed
and gravity in yards, rotated only; without it their motion takes the
placement scale too. The quest-object sparkle's flare (LootFX.m2 #0, 0x101)
rises 0.4-0.5 yd over a sign (sparkle scale 1) and 3-4 yd over GnomeBigGear
(3.5) in the original, and Light of Elune (feralaslightblue.m2, 0x1) falls in
longer columns at GO size 5 than at 0.5. With `0x8` and without `0x20`: the Sunwell gate barrier (GO 188118, scale 8.69, 0x409)
otherwise threw its sparks 8.7x too fast as radial streaks past the doorway
(native: an even fire wall; mean-image correlation 0.82 -> 0.89). With `0x8` and `0x20` (`kParticleScaledSprite`), initial velocity scales with placement/particle scale on units and GameObjects as well (Priestess of Delight/Dementia and Mother Shahraz, `Shivan.m2` / `HaremMatron.m2` at display scale 2.5, crown flames #2 `0x29` and #3 `0x229` rise ~4.9 yd as a tapering flame jet instead of collapsing inside their 1.7-2.1 yd birth sprites; Corrupted Windblossom `Windblossom02.m2` at scale 2.0 `0x429`), while `0x8` gravitational acceleration on units and GameObjects stays in world yards/s^2 so Netherspite's Void Zone (`PortalofKathune.m2`, display scale 0.1, `0x229`, horizontal birth velocity `vRange = 0`, `gravity = -1.39`) still lifts its smoke 1.56 yd in 1.5 s rather than a tenth as high. Map doodads (ADT MDDF and WMO MODD placements) and spell visuals
move and accelerate at the placement scale with `0x8` regardless of `0x20`.
Magisters' Terrace's throne orb (`BloodElf_PowerOrb_Green.m2`, MODD 964,
scale 0.358, smoke #5/#6 0x629 at 2.2-2.8 yd/s) lifts its green wisps about
1 yd over the orb in the original, 4-5 yd up the throne in yards; a Wild
Shore bonfire (`OrcBonfire.m2`, -14695.8 503.2, scale 0.47, flames #1 0x29)
keeps its flames inside the log pile, where in yards they stood a pile's
height above it. Whether the rule is the owner kind or a separate object
scale that `0x8` removes is not established. Attached particles live in the scaled bone or
model frame: Brutallus' NoTrail flames (scale 6) rise with it in the original;
moving them in yards left a flat band below his spikes.

Opaque/cutout meshes retain authored blend mode and one-sided materials even
when their model also contains particles. AshenvaleWisps has five back-facing
billboard triangles; drawing them without authored backface culling exposes
green floor triangles absent in the original client.
The existing `WOWEE_M2_NO_FOLIAGE_CULL` diagnostic disables these shared
solid/cutout culling pipelines; leave it unset for native comparisons.

Billboard widths use uniform placement scale measured from the model matrix.
WMO embedded doodads are created with complete matrices; their separate
`M2Instance::scale` value does not contain the authored MODD scale. Particle
centers and widths must use the same placement scale. The Exodar rune at
(-4108.62, -11523.8, -135.668) has scale 0.4504; another rune has scale 6.18096.
Both shrinking and enlargement matter.

Blue braziers use authored emission rates and lifecycle alpha/size without
minimum brightness, size or particle-count floors. BLP rows run top-down;
particle quad UVs invert their vertical coordinate before texture sampling.

Native comparisons require matching camera projection and observing motion.
Independent animation clocks and randomized births do not share exact phases.
Compare trail shape, sprite width, lifecycle and disappearance across a clip,
not the location of a single bright particle in one frame.

Legacy emitter header: versions 256-261 store blend mode and emitter type as
uint16 at +0x28/+0x2A; versions 262 and 263 store them as bytes at
+0x28/+0x29 (263 adds a uint16 particle colour index at +0x2A). Reading 262
with the uint16 layout yields emitter type 0 (no plane or sphere area);
GnomereganVent at the Gnomeregan entrance then collapses its 8-10 yard green
plumes into a point.

Particle sizes are the authored lifecycle scale, times placement scale for
emitters with flag `0x20` (10935 of 16070), with no caps: GnomereganVent smoke
(0x200) grows 2.78 -> 4.44 -> 8.33. In LootFX.m2 the stars (0x29, 0x229) are
about three times as large over GnomeBigGear (sparkle scale 3.5) as over a
sign (1) in the original, while the flare (0x101) stays 0.13-0.18 yd across;
Light of Elune (0x1) drops equally small dots at GO sizes 0.5, 2 and 5, and
Mark of Kael'Thas (GO 185170, size 5, 0x521) burns in flames several yards
wide. NoTrail (`0x10`) emitters, drawn in their bone's frame, grow with the
placement too: Time Rift (display 18877, scale 4) draws its rings (#0/#1,
0x1219) and toon smoke (#3, 0x219) four times their authored size in the
original. Unobserved: model-frame emitters (0x80/0x4000) without `0x20`. An all-zero lifecycle
alpha is authored (14 TBC emitters) and draws nothing.
An item visual (ItemVisuals.dbc model on a weapon's attachment 0-4) is an
exception for `0x20` emitters (Fear_State_Head #0 is 0x19 and so authored
size by the flag rule alone): its particles are drawn at the authored size, whatever the scale of
the weapon, of the weapon's attachment bone (Axe_1H_OutlandRaid_D_06 1.8,
Knife_1H_Hunting_B_02 1.39) and of the unit wielding it. That chain still
places them. Bonechewer Blade Fury (23235: display 2 x model 1.3) carries
Fear_State_Head on both axes; at the chain's 4.68 its purple wisps merged into
one bright cloud every 2.6 s (its enabled track is on 3333-3600 and 3800-4100 ms),
native draws thin separate wisps. A player's `.modify scale 2` leaves the
wisps on its dagger unchanged in the original client.

Head/tail selector: pre-264 byte `+0x2C` (0 head, 1 tail, 2 both; 12548 / 3124 /
398 TBC emitters). `+0x2D` is zero in all 16070 TBC emitters; reading it drew
every tail as a dot. A tail quad runs from the particle back along
`-velocity * tailLength` (`+0x17C`), half width = particle scale.
Corrosive Acid Breath (16359, `AcidLiquidBreath.m2` #1/#2) shows green
tadpole streaks, and the trainer learn light (kit 362, `Learn_Impact_Base.m2`
#0, tail 2.4 s at 2.78 yd/s) vertical beams.
The tail texture's U axis runs along the tail, U = 0 at the particle: 333 of
3118 tail emitters use wide textures against 3 of 12548 head emitters, e.g.
`SlimeBlob.blp` (64x16), a drop whose head is its left edge. With V along the
tail the acid breath drew thin hard-edged lines instead of drops.

Spline emitters (type 3, 120 TBC emitters): pre-264 `+0x1D4` count,
`+0x1D8` offset of cubic Bezier points (knot, handle, handle, knot; always
3k + 1, 4 to 37), equal parameter per segment, added to the emitter position.
Width held at one value: a birth takes a path parameter uniformly between the
length and width tracks. Shadowmoon_Rune2.m2 (length 0, width 1; the Witch's
Sanctum floor ring in Sunwell, Ground Rune 183036) draws its #0 streaks as a
fence all round the ring in the original; standing at the path end they made
one purple column. Width animated: a birth stands at its current value.
FlameCircle.m2 (display 410: "Molten Core Circle" traps 178187-178193, linked
trap of each Molten Core rune button) is four quarter circles of radius 2.78
whose width runs 0 -> 1 over each 1 s Stand. Native top-down 30 fps capture
shows a flame front running round the ring once a second, the brightest arc
about 0.6 s behind it (alpha peak at 0.6 of the 1 s life), in the direction
of increasing path parameter. Births spread uniformly between the length and
width tracks keep the ring brightest at its start instead; that was rejected.
Unobserved: emitters whose observations would separate this from a flag rule
(animated width with flag 0x8: ArcaneSpirit/BurningSpirit_Impact, SummonPet;
held width without 0x8: ValentineFirework). Velocity stays the
emitter's vertical cone; native FlameCircle flames rise straight up.
Wrath (264+) spline offsets are not read.

A multi-key emission rate without interpolation (type 0) emits nothing once
its sequence is past the last key. Holy Ground (29512,
`consecration_impact_base.m2`: rate 0 at 0 ms, 200 at 67 ms, 12.4 s Stand)
shows only its lava cracks in the original; holding 200/s drew a saturated
10 yd disc of DUST1_A. All 429 pre-264 rate ranges that end before their
sequence are type 0; linear ones always run to the end.

Spin (pre-264 `+0x198`, 8191 of 16070 TBC emitters) turns a sprite by
spin x age radians. Christmas Tree (Medium), GO 178557: spin 0.5, 3 s life,
alpha peak at 0.6 of life; SPARKLE.BLP is an X, and the original shows the
star as a + (rays near 0/90 degrees in 3 s mean images); unturned, WoWee drew
a fixed X. Turning direction is not established.

Particle colour index (version 263 emitter `+0x2A`, 0 or 11-13; 190 TBC
emitters): such an emitter takes start/mid/end ARGB of slot index - 11 from
the ParticleColor row its creature display names (CreatureDisplayInfo
column 13, `ParticleColorID`; 606 TBC displays). FelReaver.m2's flames,
smoke and LAVADRIP drool (#4-#7, index 12) are green on Fel Reavers (row 332)
and blue-violet on the Void Reaver (display 18951, row 306); WoWee drew the
model's own green on both. Rows 355/356 carry real alpha, so alpha is
replaced too.

Geometry particles: a pre-264 emitter naming a model at `+0x18` (28 TBC
emitters: WhirlwindGeoWhite, ConeofCold_Geo, CycloneGeo, Skull,
BoneArmor_Recursive, DarkIronNodeParticleRock) draws that model's bind-pose
triangles at each particle instead of a sprite: birth-pose orientation,
lifecycle scale (including twinkle), colour and opacity, the geometry model's
own texture and M2Blend mode (falling back to the emitter's when the geometry
model has no material, supporting all 7 M2Blend modes 0..6 via
`m2_particle_mesh.frag.glsl` and `particleBlendPipeline`), turning at an
angular velocity drawn from the tumble range (`+0x19C` min xyz, `+0x1A8` max
xyz; Whirlwind 0..8 rad/s about Z). SandVortex (`em_blend=1` AlphaKey with
`SMOKETOON01.BLP` -> `CycloneGeo.mdx` with `Clouds4.blp`, `blend=2` Alpha)
renders translucent feathered sand streaks matching native 2.4.3 once the
geometry model's own texture and blend mode are used (applying `em_blend=1`
AlphaKey `224/255` against soft `SMOKETOON01.BLP` discarded the entire funnel).

Twinkle percent (`+0x184`, 72 TBC emitters `< 1.0`): each twinkle period
(`floor(age * twinkleSpeed)`) is visible with probability `twinklePercent` and
blinks off (`0.0`) otherwise, even when `twinkleScaleMin == twinkleScaleMax`
(`Archimonde_Fire #1` authors `speed 11, percent 0.9, scale 1.0-1.0`).

Inherited emitter velocity (`0x40` + `burstMultiplier` at `+0x190`, 240 TBC
`0x40` emitters: `Gyrocopter`, `RocketMount`, `PhoenixPet`, `WaterElemental
#2/#3` `2.0`, `WellOfSoulsPain` `0.8`, `Firework_RomanCandle` `5.0`): detached
particles inherit `((currPos - prevPos) / dt) * burstMultiplier` on birth
(guarded against > 10 yd teleport steps).

Static wind (`windVector` at `+0x1B4` in yards/s, `windTime` at `+0x1C0` in
seconds; 14 TBC emitters: `Blacksmith_Smoke` / `Smokestack` `6.3886 s`,
`ShadowFangFog01` / `Creature_EtherealStorm` `20 s`, `WaterElemental #10` /
`Waterfall-long` `1.0 s`): accelerates particles by `windVector / windTime`
during `age in [0, windTime]` (total impulse `windVector` over `windTime`),
oriented by the particle's velocity basis.

Sphere emitters (type 2): radius uniform between the area length and width
tracks, elevation `U(-v, v)` above the model's horizontal plane, azimuth
`U(-h, h)` about +Z; particles travel along the offset (outward), or up +Z
with flag 0x100. Shadow Nova 30852 (`shadow_nova_area.m2` #0, vertical 0)
spreads a flat smoke band in the original; tilting from +Z instead made it
a rising column. A negative radius is no radius: Etherial.m2 Death #20
(-1.11 to -0.83, gravity 69) falls as thin lines in the original, not a
4 yd ring of streaks. Track 10 (`+0x130`, "zSource" on wowdev, loaded as
`deceleration`) is still unused: making particles travel away from
`(0, 0, zSource)` showed no difference on the Ethereal death.

The green columns over Illidari Defiler corpses in Black Temple are the
state kit of Fiery Aura 39839 (`Fel_Immolate_State_Base.mdx`, from
`creature_template_addon`; death persistent, AttributesEx3 0x100000, so it
stays on corpses). The original draws it: a Stand intro (1.5 s, emitting
throughout), then the Hold loop (4.5 s, emitting its first 1.5 s). Green
pixels after `.aura 39839` on a player: lit 4.4 s, dark 1.7 s, lit 2.0 s.
The model has no bone animation, so WoWee's clock looped Stand and never
reached Hold: the flames never went out. On the Defiler (Satyr, display
scale 2, Base bone scale 1.45) WoWee's flames still spread wider round the
body than the original's.

Channel effects take the same Stand-then-Hold course (27 of 65 TBC ChannelKit
models author both, e.g. Food_HealEffect_Base, Lifetap_State_Chest).
Magister Kalendris (Dire Maul, Dark Channeling 21157,
`Shadow_Snare_High_base.m2`) emits #0's black streaks only in Stand (enabled
track 1/0) and fades #1's dark rings out and back in over each 4 s Hold
(rate 25 -> 0 -> 0 -> 25). The original shows a green glow with faint rings
coming and going; looping Stand drew a steady burst of black spikes.
Still different: WoWee's #1 rings (tail emitter, `Shockwave_Invert.blp`)
read as sharp radial spikes where the original shows soft ring arcs.
