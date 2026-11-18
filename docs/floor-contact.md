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

A body already overlapping a triangle passes through it, so an overlap left
by a teleport or an earlier fault cannot trap it. The one exception is an
overlap no deeper than the contact skin (`kContactSkin`, the 0.0014-yard
separation `moveBody` leaves after a contact) on a body moving deeper: that
is contact. In a crease the skin push off one face carries the body that far
into the other; treated as an existing overlap, the second face was ignored
and the body fell through it. At the Alcaz Island dock (Dustwallow Marsh,
`redridgedocks03.m2`, uid 381622) a player walking from the deck onto the
beam at (-2697.7, -4694.8) dropped through the riser between them and was
held inside the deck, sunk to the chest between the boards; native 2.4.3
rests in that fold at z 9.43. Evidence:
`_experiments/20261118T205414.plank_gaps` in the parent workspace.

A grounded step that ends on a face too steep to stand on leaves the body
there, falling from rest. The next frame it may step once more, and keeps
that step only if it lands on a walkable face (`stepFromRest_`). The Alcaz
Island riser above (faces of 57 and 66 degrees, 0.4 yards high): 8606 at 20
and at 60 fps walks over it without losing speed, one frame on the riser
(-2697.70, -4694.68, 9.67) and the next on the beam (-4694.26, 9.83).
Without the second step WoWee stood on the riser, slid back and never
crossed. The Exodar table pedestal (74 degrees) is unchanged: there the
second step lands on the pedestal side again and is refused, and WoWee
peaks 0.45-0.56 yards as capped native did (0.51-0.65).

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
