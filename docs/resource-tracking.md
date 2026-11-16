# TBC resource tracking

`build/bin/test_scene_pick '[resource-tracking]'`

Find Minerals and Find Herbs use server-owned `PLAYER_TRACK_RESOURCES`, wire
index `0x523` in TBC. Learning Mining or Herbalism alone does not enable tracking.
Each `Spell.dbc` aura45 effect sets bit `(EffectMiscValue - 1)`; Find Minerals2580
uses value3 and Find Herbs2383 uses value2. These values are the skill-lock IDs in
`Lock.dbc`: Copper Vein lock38 uses key type2, lock type3. Item-key slots do not
identify a resource category.

The minimap matches this mask against resource chest lock slots. Conditional
quest objects still require their per-player activation flag. Icons use the gold
ring in original `Interface/Minimap/ObjectIcons.blp`, second row, first column.
Markers share terrain projection and rotation. Hover uses the original tooltip
frame and is suppressed when another window covers the minimap.

Tracking is independent of optional custom NPC/chest trackers. The resource field
is currently mapped only for TBC; other expansion indices must be verified before
this path is enabled for them. Find Treasure involves chest classification beyond
Mining/Herbalism skill locks and is not covered by this node path.

Evidence and dedicated-client checks live in
`../_experiments/20261116T220500Z.resource_tracking/`. Original field oracle is
`mangos-tbc/src/game/Entities/UpdateFields.cpp`; aura-bit rule is
`mangos-tbc/src/game/Spells/SpellAuras.cpp`, `HandleAuraTrackResources`.

Deployment gotcha: extracted assets use their own `expansions/tbc/update_fields.json`.
Updating the executable or build/bin/Data does not update that asset directory.
Deploy the repository metadata to the configured asset expansion directory and
restart; stale metadata otherwise cannot expose the tracking or object flags.
Required-field checks fail explicitly even in release builds.
