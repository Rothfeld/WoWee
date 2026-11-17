# TBC minimap markers

`python _experiments/20261117T181649Z.minimap_marker_audit/crop.py o45.png w29.png out.png`
(run in that experiment folder) puts the original's and WoWee's minimap side by side.

Every marker below was observed on the original 2.4.3 client (build 8606) on a
private pair at 1280x720; sizes are in interface units (pixels / 0.9375 there).
Anything not listed is not drawn: ordinary creatures, other players, chests and
battleground positions have no minimap mark.

| Marker | Art | Size | Tooltip line |
|---|---|---|---|
| Player | `MinimapArrow.blp` | 20 square | none |
| Party member, on map | `ObjectIcons.blp` row 1 cell 0 (blue) | 16 square | name |
| Tracked resource (Find Minerals/Herbs) | `ObjectIcons.blp` row 2 cell 0 (gold) | 16 square | object name, e.g. "Copper Vein" |
| Tracked town service (Mailbox, Innkeeper, ...) | same gold cell | 16 square | name, e.g. "Mailbox" |
| Quest giver `!`/`?` | `ObjectIcons.blp` row 2 cells 1-4 | 16 square | NPC name only |
| Corpse, spirit healer (ghost) | `POIIcons.blp` cell 7 tombstone | 16 square | `CORPSE_RED`, `SPIRIT_HEALER_RELEASE_RED` |
| Party member off map | `Rotating-MinimapGroupArrow.blp` | rim arrow | name |
| Corpse off map | `Rotating-MinimapCorpseArrow.blp` | rim arrow | `CORPSE_RED` |
| Spirit healer off map | `Rotating-MinimapGuideArrow.blp` | rim arrow | `SPIRIT_HEALER_RELEASE_RED` |
| Town off map | `Rotating-MinimapArrow.blp` | rim arrow | AreaPOI name |

- The player arrow turns with the character's facing; a free-look camera drag
  leaves it alone.
- Town arrows: `AreaPOI.dbc` rows with flag `0x1` on the current map, within
  700 yards (Stormwind City shown at 690, not at 710), only while off the map.
  The original's `minimapArrowModel` (Minimap.xml) is this arrow. Needs the TBC
  `AreaPOI` entry in `dbc_layouts.json`; the installed data tree's copy does
  not refresh by itself.
- Rim arrows are about 36 units across with the glyph centre 13.5 units inside
  the rim (fitted to the original's model arrows, hub within 1 px).
- One GameTooltip lists every blip within 7 units of the cursor, a line each:
  spirit healer/corpse first, then party members, then the rest. "Spirit
  Healer" above a party member and a party member above "Mailbox" were
  observed; other orderings were not.
- The new-mail envelope (FrameXML `MiniMapMailFrame`) depends on the
  `MSG_QUERY_NEXT_MAIL_TIME` answer, asked on world entry. That answer arrives
  before FrameXML loads, so `UPDATE_PENDING_MAIL` is fired again after loading.

Not verified: native while dead hid the tracked mailbox next to the corpse
(cause unknown); battleground flag carriers; Find Treasure; mixed quest and
flight-master marks; tracked-blip cell (row 1 cell 6 and row 2 cell 0 look
alike at this size).

Evidence: `../_experiments/20261117T181649Z.minimap_marker_audit/`.
