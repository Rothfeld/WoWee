# TBC flight-point discovery

`build/bin/test_input_integration '[taxi-discovery]'` exercises the request and
response lifecycle without connecting to a server.

Visible, alive, interactable friendly or neutral flight masters receive one
`CMSG_TAXINODE_STATUS_QUERY` per loaded lifetime. The cache distinguishes
unqueried, pending, undiscovered and discovered states. CMaNGOS replies with a
raw GUID and byte: zero means undiscovered, one means discovered. Learning a
node sends one immediately. Missing or pending replies do not create a marker.
Despawn, eligibility loss, world transfer and disconnect clear cached knowledge.

An undiscovered node uses the original `Interface/Buttons/TalkToMeGreen.m2`.
Its measured TBC service placement is the authored name attachment plus half a
yard, with original model scale. This placement was inferred from matched
native camera projections, not executable inspection. This solid mesh has no
billboard bone, so its +X front rotates toward the camera about world Z.
Gold quest-marker
placement remains separate. A visible quest marker has priority when both
states exist; mixed quest/flight-master priority has not been compared live.

Flight knowledge belongs to each character. Two characters at the same NPC may
correctly display different markers. The old `taxiNpcHasRoutes` terminology
misdescribes this packet: it reports discovery, not available destinations.
An unused similarly named cache remains in MovementHandler; it has no runtime
consumers and must not be used as a second source of discovery state.
