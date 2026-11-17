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

Only authored M2 collision triangles (header boundingTriangles) collide - as
floor, wall or camera blocker - for ADT doodads, WMO children and game objects
alike; the CMaNGOS vmap extractor reads the same triangles for server-side
height and line of sight. A model without them
is walked through; its bounds never become a box wall or a floor. The Blackrock
Depths rune glyphs (`darkirondwarfrune_*.m2`, GO 170578-170584) are the
regression: no collision triangles, a 2.7-yard tall vertex box, and a player
lifted onto that box and left floating. Evaluate the policy against the
renderer's cached model. Tile preparation intentionally uses empty model
placeholders for repeated or already uploaded assets; an empty placeholder says
nothing about collision in the cached model.

Airborne lateral contact against authored M2 geometry cannot raise the body as
a grounded step. A vertical side collision may block penetration but ends a
fall only when a walkable M2 floor reaches the feet. This keeps the next lateral
sweep from gaining a grounded step allowance while hanging against a prop.

Grounded movement that runs into authored M2 collision steps: the body rises
by at most `kMaxStepUp`, moves across, and descends onto the first face it
meets, whatever that face's slope. Ground-follow then decides support: a
walkable face keeps the walker grounded, a steep one starts a fall from rest.
Native TBC places the beveled body at exactly that resting height (within a
few millimetres, measured against the exported placed meshes) on the Exodar
table's 74-degree pedestal sides and on the hammer handles beside it, then
falls or walks on. Without the step, WoWee slid along the pedestal at floor
height. Native rose up to 0.92 yards in one frame there, more than the 0.6
budget, so the native step height is not established by this evidence.
Native outcomes depend on frame rate: uncapped on a busy host it climbed the
table in three of four identical walks; capped at 20 fps it never did.
Compare at a matched frame rate. Evidence:
`_experiments/20261117T171500Z.table_collision_data_rule`.

M2 support probes use the moving body's radius and lower beveled envelope.
Each offset sample carries its height above the sole; subtract that height from
the mesh hit to obtain supported sole height. A prop touching the body's side
does not place the sole on top of the prop. Probe extent must follow body size,
including diagonal corners, rather than a fixed world-space square.

Paired native/client recordings and supported-ramp checks are retained in the
parent workspace under `_experiments/20261117T000028Z.balcony_contact`.
Cross-map test setup waits for a fresh `World terrain ready: map=N` log entry
before sending movement. The application may already be IN_GAME during a map
transfer, so an application-state transition alone cannot prove terrain loaded.
`WOWEE_BODY_CONTACT_TRACE=1` logs horizontal M2 sweep requests/results and the
subsequent ground snap. Movement contact counters include horizontal and
vertical sweeps. Use these together to distinguish collision rejection from
a later floor snap erasing the resolved height.
`WOWEE_TRACE_M2_FLOOR=1` logs each winning M2 candidate's model, source mesh,
height and query position. This identifies the supporting object when
a floor probe promotes a nearby decoration over the surface under the feet.
`WOWEE_TRACE_M2_COLLISION=<model name substring>` (for example
`WOWEE_TRACE_M2_COLLISION=DR_TABLE_MEDIUM_01`, the Exodar table; the match is
case-sensitive against the model path as loaded) logs every wall query that reaches a
matching model: query position in model space, body radius and height,
instance scale, collision triangle count and whether the instance skips
collision.

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

Gravity waits only for terrain that can still stream in: streaming enabled,
tile neither loaded nor failed, and its ADT present. WMO-only maps (WDT names
a root WMO; Blackrock Depths, map 230) never load a tile. Treating "not
loaded" as "still arriving" there disabled gravity and jumps, and left a
lifted body in the air (ledger #70). The pair probe in
`_experiments/20261117T171500Z.table_collision_data_rule/brd_probe.py`
teleports by console because view sync rejects instance maps.
