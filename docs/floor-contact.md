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

Walking off an edge also publishes FALLING movement state once airborne for
0.15 seconds, with zero vertical launch speed and the retained horizontal
takeoff velocity. Heartbeats carry the physics elapsed fall time and position.
Releasing movement input must not erase the airborne trajectory; FALL_LAND
clears it. Native TBC captures on this route use zero launch speed and
7 yards/second horizontal travel without a JUMP opcode.

WMO child props require authored collision geometry for wall blocking. Evaluate
this policy against the renderer's cached model. Tile preparation intentionally
uses empty model placeholders for repeated or already uploaded assets; an empty
placeholder says nothing about collision in the cached model. Both initial and
reused instances must block against the same authored triangles. Decorative
children without collision meshes must not gain bounding-box walls.

Airborne lateral contact against authored M2 geometry cannot raise the body as
a grounded step. A vertical side collision may block penetration but ends a
fall only when a walkable M2 floor reaches the feet. This keeps the next lateral
sweep from gaining a grounded step allowance while hanging against a prop.

Paired native/client recordings and supported-ramp checks are retained in the
parent workspace under `_experiments/20261117T000028Z.balcony_contact`.
Cross-map test setup waits for a fresh `World terrain ready: map=N` log entry
before sending movement. The application may already be IN_GAME during a map
transfer, so an application-state transition alone cannot prove terrain loaded.
`WOWEE_BODY_CONTACT_TRACE=1` logs horizontal M2 sweep requests/results and the
subsequent ground snap. Movement contact counters include horizontal and
vertical sweeps. Use these together to distinguish collision rejection from
a later floor snap erasing the resolved height.

The Exodar table route at server (-4191,-11477.700195,-131.451859), heading
0.299362, exercises more than wall blocking. Original TBC emits repeated
FALL_LAND packets while walking along its base; feet briefly rise and descend.
Matching its silhouette path while keeping the actor grounded does not verify
that contact response. Geometry reconstruction and wire samples are retained in
`_experiments/20261117T005000Z.table_geometry_replay` in the parent workspace.
The movement helper regressions run with:

```sh
cmake --build build --target test_hill_climbing
build/bin/test_hill_climbing
```
