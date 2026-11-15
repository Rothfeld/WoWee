# TBC overhead labels and hover

Overhead health plates and plain world names use different presentation rules.
TBC plates use `Interface/Tooltips/Nameplate-Border.blp` and
`Interface/TargetingFrame/UI-TargetingFrame-BarFill.blp`, with white names,
yellow hovered names, a separate level field, and reaction-colored health.
Friendly unflagged player plates use blue health. NPC subtitles, percentage
text, and custom aura dots are omitted from legacy plates.

Plate anchors use M2 attachment 18 (29 when mounted), plus the native
2/3-yard plate offset. The frame's top aligns to that point; its health bar
sits below it according to the original artwork geometry. Plain names use
perspective-scaled text with a raised baseline, calibrated against build 8606.
They sample scene depth per pixel, so tent fabric and other model geometry
can hide part of a name. The final water pass must preserve resolved depth
with STORE for this to work. Character hover also illuminates attached weapons.

Plate range is measured from the player, independently of camera zoom. Units
outside plate range can still show a plain world name according to name settings.
Legacy world names follow visible units without an additional 40-yard camera
cutoff or five-yard label fade. Scene depth still occludes their pixels.
Neutral attackable creatures use the enemy-plate toggle. A visible plate retains
its name even when the ordinary NPC-name setting is disabled. Plain names scale
with perspective and have no clickable plate rectangle.

World picking and visible plate hit tests produce one mouseover GUID. It drives
NPC name visibility, mouseover macros, and character highlight. FrameXML and
ImGui mouse ownership suppress underlying world input. Character hover adds
neutral texture illumination before fog, without modifying texture alpha or
preview rendering. Moving off the unit or plate clears the highlighted instance.

Build 8606's anonymous WorldFrame children expose the original textures, text
fonts, region geometry, and health colors. Geometry was queried at a 768-unit
logical UI height with 1280x720 presentation. Native and WoWee captures cover
Adlin Pridedrift, Balir Frosthammer, Coldridge Mountaineers, and another player.
They verify plate artwork, name colors, level placement, model/plate hover,
hover-only NPC names, and range at the comparison camera distance. They do not
establish pixel-exact anchor placement, highlight intensity under every lighting
condition, or boss/elite artwork parity.

In TBC, `ShowFriendNameplates()` / `HideFriendNameplates()` and the saved
`FRIENDNAMEPLATES_ON` variable control friendly plates. Stock FrameXML restores
this variable through `UpdateNameplates()`. A comparison restart must close the
client normally: SIGTERM bypasses saved-variable shutdown and resets this state.

NPC equipment uses the three server update slots. In TBC these contain
ItemDisplayInfo IDs, with inventory type in UNIT_VIRTUAL_ITEM_INFO; in Wrath
these contain item entries, resolved through Item.dbc. Main hand, off hand,
and ranged weapons use the same model attachments and sheath transforms as
player equipment. Changes to slot metadata or sheath state rebuild attachments.
Non-humanoid creature models can carry weapons when their M2 defines the required
attachment; CreatureDisplayInfoExtra is not a prerequisite.

NPC corner tooltips use GameTooltip:SetUnit and the default FrameXML anchor.
The engine supplies trade title, level, faction, PvP flag, and a named child
health StatusBar. Non-unit tooltip setters clear the unit identity so item and
spell tooltips cannot retain an NPC health bar. TBC Faction.dbc's English name
is field 19; Wrath uses field 23. Always consult the active DBC layout.

Shader-only builds must refresh `build/bin/assets/shaders`, where the running
executable loads SPIR-V. Copying assets only on executable POST_BUILD leaves
shader-only edits stale. The shader build target copies changed outputs on each
build. For validation, compare source and runtime shader hashes.

Raid targets use `Interface/TargetingFrame/UI-RaidTargetingIcons.blp`, a 4-column
atlas with 64-pixel cells. Build 8606 uses a one-yard camera-facing quad above
the name attachment when the unit has no visible plate. A visible legacy plate
uses an icon anchored to its left edge. Own markers use the player's renderer
instance and remain visible when `unitNameOwn` is disabled. The native quad
vertices are at executable VA 0xbbf124; atlas UVs start at 0xbbf154, and drawing
starts at 0x6d46b0. Nameplate icon setup is at 0x7bdd52.

Group rosters exclude the local character. Leader-name notifications must resolve
the active character separately, and roster leader changes must emit
`PARTY_LEADER_CHANGED`: stock party crowns do not refresh on roster events alone.
Joining or reconnecting to a group requests the authoritative raid target list
with MSG_RAID_TARGET_UPDATE payload 0xff; the roster itself contains no marks.
