# Teleport movement and entity visibility

CMaNGOS TBC sends two different teleport packet layouts:

* `MSG_MOVE_TELEPORT`: packed GUID followed immediately by `MovementInfo`.
  This observer broadcast updates a remote entity and its render position
  immediately, without interpolation across the teleport distance.
* `MSG_MOVE_TELEPORT_ACK`: packed GUID, order counter, then `MovementInfo`.
  This request updates the local player and requires an acknowledgement.

In TBC `MovementInfo`, the one-byte secondary flags field precedes the
four-byte movement timestamp. See `Unit::SendTeleportPacket` and
`MovementInfo::Write` in CMaNGOS `src/game/Entities/Unit.cpp`.

Same-map teleports stream destination terrain without resetting entity render
instances. The server may retain nearby entities and send only field updates;
discarding their models would require CREATE packets that will not arrive.
Cross-map entry still uses the full world loader.

Reset the camera controller with `teleportTo`, including floor and fall state.
Assigning only its follow target leaves the previous ground height active and
can trigger void recovery back to the departure location during streaming.
