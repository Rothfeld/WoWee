# TBC flight-point discovery

`build/bin/test_input_integration '[taxi-discovery]'` exercises the request and
response lifecycle without connecting to a server.

Visible, alive, interactable friendly or neutral flight masters receive one
`CMSG_TAXINODE_STATUS_QUERY` per loaded lifetime. The cache distinguishes
unqueried, pending, undiscovered and discovered states. CMaNGOS replies with a
raw GUID and byte: zero means undiscovered, one means discovered. Learning a
node sends one immediately. Missing or pending replies do not create a marker.
Despawn, eligibility loss, world transfer and disconnect clear cached knowledge.

An undiscovered node uses the original `Interface/Buttons/TalkToMeGreen.m2` at
original model scale, based on the authored name attachment (29 when mounted).
While a plain world name is drawn for the NPC, the marker is raised half a yard
so it sits above the two-line name/subtitle block. With a health plate, or with
no visible name, it stays on the attachment and the plate covers its lower part.
`GameScreen::unitLabelVisibility` supplies the plate/name decision, so the
marker follows the same settings, target and hover rules as the label.

Measured on build 8606 at Astranaar (Daelyshia), matched cameras at 11.4 and
14.7 yards: the native lift is 41.5 and 33 pixels, i.e. constant in world units
(0.47-0.55 yards within the marker's bob). All 102 TBC flight masters carry a
subtitle, so the name block is always two lines. Targeting a nameless NPC shows
its name and raises the marker in both clients.

Native caches this placement per unit. Turning names on raises the marker at
once; turning them off leaves it raised until the unit is created again, e.g.
after relogging. Compare native captures only after re-creation. WoWee applies
the current rule every frame. Measurements use matched camera projections, not
executable inspection. Evidence:
`_experiments/20261117T102218Z.flight_marker_name_offset/`.

This solid mesh has no billboard bone, so its +X front rotates toward the camera
about world Z.
Gold quest-marker
placement remains separate. A visible quest marker has priority when both
states exist; mixed quest/flight-master priority has not been compared live.

Flight knowledge belongs to each character. Two characters at the same NPC may
correctly display different markers. The old `taxiNpcHasRoutes` terminology
misdescribes this packet: it reports discovery, not available destinations.
An unused similarly named cache remains in MovementHandler; it has no runtime
consumers and must not be used as a second source of discovery state.
