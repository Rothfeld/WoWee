# Authored M2 particle motion

Run focused checks from the WoWee directory:

```sh
build/bin/test_m2_ribbon_tracks '[m2][particle]'
```

Spline emitters retain each particle's animated emitter pose at birth. Their
positions remain in model space, so moving the placed model moves the trail
without applying the current emitter bone to every existing particle.
DR_DRAENEI_HOLORUNES02_BIG.M2 uses zero emission speed and animated parent
bones to draw its traveling floor pattern.

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
