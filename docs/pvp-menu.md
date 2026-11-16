# PvP preference

The portrait menu uses `GetPVPDesired` and `SetPVP`. Desired state comes from
`PLAYER_FLAGS` bit `0x200`, resolved through the expansion's update-field table.
`SetPVP` sends `CMSG_TOGGLE_PVP` with one byte: 1 enables, 0 disables. The empty
packet remains the toggle operation used by `TogglePVP`.

The active shield uses `UNIT_FIELD_FLAGS` bit `0x1000`. Disabling the preference
does not immediately clear that flag: CMaNGOS counts down 300 seconds when PvP
combat and enforced-area conditions allow it. Server updates remain authoritative.
Unit flag changes emit `UNIT_FACTION` for Blizzard's portrait listeners.

Regression checks: `build/bin/test_game_replay '[pvp]'` from the repository root.
Live validation uses the portrait menu, then reads the server-updated preference
and active flag separately.
