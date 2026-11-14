# TBC interface compatibility

The 2.4.3 interface uses the original extracted FrameXML and DBC data.
Keep the expansion's dbc_layouts.json and update_fields.json synchronized with
Data/expansions/tbc when changing data readers; extraction copies these files.

## Fonts and tooltips

Original build 8606 renders requested font sizes above 32 at size 32, while
GetFont returns the requested size. Text measurement and rendering share the
limit. Tooltip header and body regions retain their separate font heights.

Spell tooltip values come from Spell.dbc, SpellRange.dbc and current unit fields.
Ranged attack time is integer milliseconds on the wire. CMaNGOS stores it as a
float internally but converts it in Object.cpp before sending update fields.
Tooltip FontString text and colors refer to the same lines that the renderer draws.
SetPlayerBuff uses Spell.dbc's Tooltip field, which describes the active aura;
Description describes the castable spell and can contain different text.

## Aura packets

TBC has 40 positive and 16 negative visible aura slots. Its aura flags carry
effect bits; bit 0x02 does not classify a debuff. Classification uses the slot.
GetPlayerBuff and its related TBC UI methods expose one-based slots.

SMSG_INIT_EXTRA_AURA_INFO contains a packed GUID followed by zero or more
13-byte records. SMSG_SET_EXTRA_AURA_INFO and its NEED_UPDATE variant contain
one record. Each record is uint8 slot, uint32 spell ID, int32 maximum duration,
uint32 remaining duration. There is no count, effect index or flag byte.

Extra-aura metadata augments known object-field auras. It does not erase auras
omitted from the metadata packet. Duration can arrive before the spell ID;
empty-slot timing is preserved until the corresponding object fields arrive.
Unrelated field updates preserve a continuing aura's timer.
Server-only helper spells absent from Spell.dbc stay out of UI aura lists.
For example, CMaNGOS applies 20612 with Stoneform 20594; only 20594 has a
client display entry.

Food and drink restoration are aura-driven animation and sound states. They
do not emit spell-channel events or appear in UnitCastingInfo/UnitChannelInfo.
Moving still cancels restoration; actual timed casts and channels retain their
separate state. Animation classification reads the active restoration spell
identity independently of the current cast ID, which remains zero while eating.
EmoteEat runs on SpineLow and its descendants with an independent timeline;
Sitting supplies the pelvis and legs. Food ending removes the upper-body layer.

## FrameXML behavior

Missing optional API globals remain nil. WOWEE_LUA_API_FALLBACK=1 explicitly
enables diagnostic placeholders; it is not the normal execution mode.
Saved variables registered through RegisterForSave and RegisterForSavePerCharacter
are restored before login events and persisted at shutdown.

The world map uses ToggleWorldMap and the binding manager's actual command
lookup. FrameXML supplies map controls and labels. Zone bounds drive player
marker hit testing. Overlay hit rectangles are top/left/bottom/right coordinates
in the visible 1002x668 map, rather than the padded 1024x768 texture.
Unexplored overlay labels remain hidden.

Spellbook button names do not encode their slot IDs: SpellButton1 through
SpellButton4 have XML IDs 1, 7, 2, 8. Tab refresh checks must use GetID().
The General tab uses Ability_Kick. Internal/item-only/profession recipe entries
are filtered from the player spellbook while remaining available to gameplay.

## Settings and verification

Status Text is off by default in TBC; values appear on hover. Buff duration text
below icons is controlled by buffDurations and defaults off in stock FrameXML.
The tooltip countdown does not depend on that setting. Saved preferences win.
Automatic idle camera orbit is opt-in.

For UI comparisons, retain original-client screenshots as a baseline and capture
changed WoWee behavior. Match resolution, UI scale and settings, and account for
character inventory, progression and addons. Use separate live checks for hover,
casting and timer expiration; successful Lua loading alone does not verify pixels.

Local cast checks cover passive spells, death, combat restrictions, stealth,
combo points and available power. The server remains authoritative for complete
spell eligibility. Other expansion interfaces require their own comparison runs.

## Trade and depleted action items

TBC trade update headers contain client/server offer revisions. Item records run
to packet end; revisions are not counts. CMSG_ACCEPT_TRADE echoes the peer's
server revision. GetTradeTargetItemInfo returns quality before isUsable;
GetTradePlayerItemInfo does not. During trade, the NPC unit token resolves to
the trading player even when a vendor remains selected.

Item actions retain their identity at zero inventory count. Metadata is requested
independently of bag contents and matching actions refresh when it arrives.
Original FrameXML supplies the dim icon and zero count for depleted consumables.
