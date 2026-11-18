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
GetLFGTypeEntries returns name and texture pairs. The texture is LFGDungeons.dbc
field 22 (a string: `ScarletMonastery` for all four wings, empty for most
zones), which FrameXML appends to `Interface\LFGFrame\LFGIcon-`. The 2.4.3
file has 24 fields: ID, 16 name locales, locale mask, min/max level, type,
faction, texture, expansion. Level filter is inclusive (level 40 lists
Maraudon 40-52 but not Scarlet Monastery - Library 31-39); entries sort by name.

Answers measured on the 2.4.3 client with `/script` and pcall:

- GetLookingForGroup returns 13 values with real booleans. An empty LFG slot
  reports type 2 (Dungeon) and index 0, also after ClearLookingForGroup.
  The LFM type starts at 2 and becomes 1 (None) after ClearLookingForMore.
- SetLFGType/SetLFMType ignore out-of-range slots and types.
- CanSendLFGQuery(type, index) reports only the query throttle (`(1, 0)` is
  true); with fewer than two numbers it returns nothing.
- GetLFGResults for a missing row returns nil, 0, six nils, 0, false, nil;
  GetLFGPartyResults returns nil, "", nil.
- Called with no arguments, SetLFGType, SetLookingForGroup, LFGQuery,
  GetNumLFGResults, GetLFGResults, GetLFGPartyResults, CanSendLFGQuery and
  SortLFG raise their `Usage:` text (GetNumLFGResults says
  `Usage: GetLFGResults(type, lfgNdx)`). Partial or nil arguments do not raise.
  GetLFGTypeEntries raises for any non-number. SetLFMType, SetLookingForMore
  and SetLFGComment never raise.
- Each SMSG_LFG_UPDATE raises MEETINGSTONE_CHANGED then LFG_UPDATE. The login
  update arrives before FrameXML loads; WoWee announces it after
  VARIABLES_LOADED, so `LFM_AUTO_ADD` ends as the reported autofill state.

LFMFrame relies on two widget answers: an empty FontString (a cleared
dropdown's text) reads back as nil, and a disabled CheckButton without a
DisabledTexture still draws its NormalTexture box.

`TbcLfgState` belongs to the game session and resets on logout. Server updates
supply listing and queue state. MSG_LOOKING_FOR_GROUP responses contain packed
player GUIDs, levels, zones, three criteria, comments and packed party members.
Malformed result packets fail before replacing the last complete result list.
Name-query completion updates the displayed results.

TBC SMSG_NAME_QUERY_RESPONSE contains an uncompressed GUID, name and realm
strings, uint32 race/gender/class, and declined-name data. Even a local realm
occupies one terminating zero byte. Skipping that field corrupts class/race and
breaks result colors and class text.

Probe transcripts and paired captures for the answers above:
`../_experiments/20261118T193600Z.lfm_dialog_diff/`.

Not established: whether 2.4.3 SetLFGType/SetLFMType send packets themselves
(native slot types sometimes revert to 2 between scripts, and the queued flag
toggles, without a SetLookingForGroup call).

Live verification used original 2.4.3 Graulberto queued for Stormwind Stockades
and WoWee Graulberon searching LFM. Category/dungeon selection, real server
results, row hover, refresh, sorting and invitation were exercised. Original
client received and accepted the invitation. Evidence is in
`../_experiments/20261116T123630.246731Z.lfm_dialog/`.
