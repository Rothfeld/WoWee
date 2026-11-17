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
- `0x20` is `UnLit`, not a coordinate-space flag.

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
Gravity uses the same birth-space basis as position and velocity. Its sampled
magnitude remains an authored emitter track; transforming the direction once
preserves placement scale and orientation for detached particles. A rotated,
scaled ballistic regression compares all three attachment modes in world space.

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

Particle sizes are the authored lifecycle scale times placement scale, with no
caps: GnomereganVent smoke grows 2.78 -> 4.44 -> 8.33. An all-zero lifecycle
alpha is authored (14 TBC emitters) and draws nothing.

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
