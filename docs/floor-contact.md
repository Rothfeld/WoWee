# Floor contact

Walking and falling use collision-sampled heights from terrain, WMO groups and
M2 props. Footprint and movement-segment probes recover narrow supported seams.
Ground contact then requires a reachable step, a crossed floor while falling,
or the existing short slope-following range.

Do not replace a lower sampled floor with a height derived from lastGroundZ.
That height has no supporting triangle. Repeatedly snapping to it resets
vertical speed, retains grounded animation, and makes descent depend on frame
rate. The Exodar balcony at server coordinates (-4199.94,-11608.7,-98.6475)
is a recorded regression route: walk toward heading1.21757, then release
forward while airborne. Horizontal momentum must continue until landing.

Paired native/client recordings and supported-ramp checks are retained in the
parent workspace under `_experiments/20261117T000028Z.balcony_contact`.
The movement helper regressions run with:

```sh
cmake --build build --target test_hill_climbing
build/bin/test_hill_climbing
```
