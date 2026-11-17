# TBC moving transport timing

Run focused route tests: `build/bin/test_transport_path_repo '[taxi-schedule]'`.

TBC moving transports carry their route clock in the `UPDATEFLAG_TRANSPORT`
movement payload. Preserve the unsigned millisecond value, including zero and
wraparound. Cache receipt time even when model loading or the game-object query
has not registered the transport yet. Advance from monotonic elapsed time since receipt;
per-frame integer millisecond accumulation loses time.

For taxi-path transports such as Moonspray (entry 176244, path 293), use
`TaxiPathNode.dbc` control points, teleport flags, stop flags and delays together
with the game-object template's move speed and acceleration. The schedule omits
endpoint control nodes, splits at teleport boundaries, and derives segment
lengths from three Catmull-Rom chord samples. Stop-to-stop travel accelerates and
brakes. A route with no explicit stop uses its origin as the implicit stop.
Moonspray's original TBC data yields a 312178 millisecond generated period.

CMaNGOS then overrides the period and final departure time from its realm's
`transports` table. Moonspray's configured period is 312734 milliseconds. Small
differences accumulate each cycle; they must not be approximated by a phase
offset or by scaling the intermediate motion segments.

TBC does not transmit this period in GAMEOBJECT_LEVEL/DYNAMIC (Wrath does).
Set `WOWEE_TRANSPORT_TIMETABLE` to a realm-specific JSON export with schema
`wowee.realm-transports.v1`, expansion `tbc`, nonempty `realm` and `source`
strings, and `transports` rows containing integer `entry` and `period_ms`.
Configured files reject missing transport entries, duplicate entries, zero or
invalid periods. The client does not connect to a world database. Without a
timetable, startup warns that generated periods cannot reproduce realm
overrides; native-derived period portability remains unresolved.

Map ownership comes from the evaluated route segment. Do not render a hull on
its previous map while its route is on another map. Passenger map transfers
remain server-owned.

An early-looking boat can be a route phase mismatch. Compare the same boat's
server position and native view before changing visibility distance. Native
`.debug transports` reports server positions when issued by a GM client. Do not
run that command through the server console: its implementation accesses the
player session.

The private ride in `_experiments/20261116T234100Z.boat_ride_visibility` exercises
boarding, remote passenger movement and local movement while underway. Original
TBC passenger movement uses an unpacked 64-bit transport GUID. Parsing it as a
packed GUID shifts the remaining movement fields and can produce enormous yaw
values. Angle normalization must remain bounded for malformed input.

The raw GUID layout applies to all three TBC MovementInfo paths: outgoing
movement, remote movement opcodes, and living-object CREATE updates. A correct
remote reader alone does not make boarding valid: a packed outgoing GUID shifts
the server's offsets and transport timestamp. The `[transport]` integration
tests check the complete outgoing payload and a living CREATE packet.

WMO passenger offsets use model-local axes, not world server/canonical axes.
Do not swap local X/Y on outgoing movement. Derive relative facing from the
current hull transform; the hull's initial server yaw is stale while turning.
The integration fixture verifies the server's passenger-to-world equation.

Distinguish a normal teleport off a ship from the ship crossing maps using
`SMSG_TRANSFER_PENDING`. Its optional transport entry and origin map identify
the latter. Being aboard when `SMSG_NEW_WORLD` arrives is insufficient: retaining
passenger ownership after a portal or GM teleport waits for a ship that will
never arrive and leaves local controls frozen. Normal transfers clear pending
passenger state; transport transfers retain model-local offsets.

Private live proof includes both clients boarding, on-deck strafing observed
by native, arrival at Auberdine, then a normal teleport to Exodar and seven yards
of local movement in the same process. Exact moving-hull silhouette timing is
not established by those independent-camera captures. The native camera probe
reports boat-relative directions aboard; its usual matched-camera hold must
not be used as boat geometry evidence.
