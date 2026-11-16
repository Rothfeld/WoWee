# TBC Looking For Group / Looking For More

Run packet regressions from the WoWee directory:

```sh
cmake --build build --target test_lfg_join_packet -j2
build/bin/test_lfg_join_packet
```

TBC FrameXML uses the LFG/LFM bulletin board and meeting-stone queue APIs.
`lua_lfg_api.cpp` exposes its category selection, three LFG slots, LFM listing,
comments, query cooldown, result rows, party details and sorting. Invitations
use the existing party invite path. These APIs use TBC opcodes, not Wrath's
role-based dungeon-finder join packet.

Dropdown selections are one-based indices into the level/faction-filtered,
alphabetically ordered LFGDungeons.dbc catalogue. Network selections contain
the DBC ID in the low 16 bits and type in the high byte. Read level from the
live player entity; the XP-update level cache starts at 1 during login.
FrameXML treats the second GetLFGTypeEntries return per entry as an icon suffix,
not a DBC ID. Empty suffix uses FrameXML's category icon.

`TbcLfgState` belongs to the game session and resets on logout. Server updates
supply listing and queue state. MSG_LOOKING_FOR_GROUP responses contain packed
player GUIDs, levels, zones, three criteria, comments and packed party members.
Malformed result packets fail before replacing the last complete result list.
Name-query completion updates the displayed results.

TBC SMSG_NAME_QUERY_RESPONSE contains an uncompressed GUID, name and realm
strings, uint32 race/gender/class, and declined-name data. Even a local realm
occupies one terminating zero byte. Skipping that field corrupts class/race and
breaks result colors and class text.

Live verification used original 2.4.3 Graulberto queued for Stormwind Stockades
and WoWee Graulberon searching LFM. Category/dungeon selection, real server
results, row hover, refresh, sorting and invitation were exercised. Original
client received and accepted the invitation. Evidence is in
`../_experiments/20261116T123630.246731Z.lfm_dialog/`.
