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
