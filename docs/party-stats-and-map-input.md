# Party stats and map input

`cmake --build build --target test_game_replay && build/bin/test_game_replay '[party][tbc]'`

TBC party stat packets, both full and incremental, start with a packed GUID and
32-bit update mask. Health fields are 16-bit. The full packet has no prefix byte.
Protocol reference: CMaNGOS TBC `src/game/Groups/GroupHandler.cpp`,
`BuildPartyMemberStatsChangedPacket` and `HandleRequestPartyMemberStatsOpcode`.
Roster packets omit health; the client requests full stats for remote members.
Object health/power updates notify party/raid unit tokens as well as target tokens.

`PromoteToLeader` accepts both unit tokens and character names. Blizzard's party
portrait menu passes `party1`; its raid menu passes a name. Resolve the recipient
through the roster before sending `CMSG_GROUP_SET_LEADER` with the 64-bit GUID.
The `[promote]` replay test covers both forms with a TBC roster and no visible
world entity for the recipient.

Loot feedback follows changes in the server roster's method, threshold, and
master-looter GUID. Unchanged rosters produce no repeated message. Each change
also notifies `PARTY_LOOT_METHOD_CHANGED`, including threshold-only and master-only
updates. Personal pass-on-loot feedback follows its outgoing request because TBC
does not acknowledge that setting. Message wording follows Blizzard's
`ERR_SET_LOOT_*`, `ERR_NEW_LOOT_MASTER_S`, and `OPT_OUT_LOOT_TOGGLE_*` strings.

Map loading pumps SDL events outside the application's normal input dispatch.
`WorldLoader::loadOnlineWorldTerrain` releases camera capture and held Lua bindings
before entering that loop. Otherwise releasing right mouse during loading leaves
the camera's held-button state set after the transfer.

Live regression: hold right mouse, transfer out of an instance, release during
loading, then move the pointer without buttons. Camera must stay fixed. Repeat
entering the instance. Check the alive party member's frame after both transfers.

Party portrait models retain their subject GUID. When that member leaves object
visibility, the portrait continues rendering its last known appearance. A different
GUID cannot reuse that image, and shutdown invalidates the retained subject.
