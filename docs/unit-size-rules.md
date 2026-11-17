# Unit size: selection circle and cursor picking (build 8606)

Rules recovered by static disassembly of `Wow.exe` 2.4.3.8606 (image base
0x400000) and the TBC DBCs in `client/wowee-data/expansions/tbc/dbfilesclient`.
Scripts and notes: `_experiments/20261117T140000Z.unit_size_rules/` in the
workspace. Print the radius the client uses for a model's displays:

```sh
python _experiments/20261117T140000Z.unit_size_rules/radius_table.py ElementalEarth Moonkin
```

Implementation: `include/core/unit_size.hpp` (radius),
`include/rendering/visual_mesh_pick.hpp` (model picking),
`EntitySpawner::getSelectionRadiusForGuid`, `CharacterRenderer::intersectInstance`,
`ui::pickScene`. Tests: `tests/test_unit_size.cpp`, `tests/test_scene_pick.cpp`.

## 1. Selection circle

Drawn by the CGUnit_C vtable slot +0x48 (`0x614070`, unit and player vtables
0x8c5580 / 0x8c32b8), called by the world frame (`0x4acab0`) for the selected
object. It projects `Textures\UnitSelectTexture.blp` (handle created in
`0x626a40` as "ObjectSelectionCircle") onto the ground inside the box
`position +- r` on all three axes; `r` is the float at CGUnit_C+0xfb8.

`r` is computed in `0x60e560` (called on display, scale and mount changes):

| step | value | address |
|------|-------|---------|
| box | CreatureModelData GeoBox, fields 17-22 (record +0x44..+0x58) | `0x60ce50` |
| mounted | rider box moved so its vertical middle is at the mount's MountHeight (field 16, +0x40), then unioned with the mount's GeoBox | `0x60cf04`, `0x604bd0`, `0x604b70` |
| degenerate | `|dx| < 2.4e-7 and |dy| < 2.4e-7` -> r = 1.2 (no scale) | `0x8907e8`, `0x8c5450` |
| d | `0.5 * sqrt(dx^2 + dy^2)` (horizontal half diagonal) | `0x60e5e5` |
| x | `d * scale` (vtable +0x58) | `0x60e611` |
| r | `sqrt(x)`; if `x > 5`: `r += 0.06 * (x - 5)^2` | `0x897374`, `0x892da8`, `0x8c5448` |
| clamp | `r = min(r, 10)` | `0x88d7c0` |

Scale (vtable +0x58, `0x5ec9b0`) = OBJECT_FIELD_SCALE_X x CGUnit_C+0xa0 (1 for
units) x CGUnit_C+0xa4 (only while a mount model exists: the mount display's
CreatureModelScale, `0x6216b0`) x CGUnit_C+0xfe8. +0xfe8 comes from `0x60bc70`:
CreatureModelData.ModelScale x CreatureDisplayInfo.CreatureModelScale x the
native race display's CreatureModelScale when the display has an extra
(humanoid) record (`0x60bbc0`); 1 when the product is not positive; then the
CreatureFamily level scale (pets take it, others take the larger of the two).

WoWee does not yet apply the race display factor of `0x60bbc0` (it changes
render size as well). Expansions whose DBC layout names no GeoBox draw no ring
and log it once per unit.

Examples (scale 1): HumanMale 0.84, NightElfFemale 0.84, TaurenMale display 59
1.35, ElementalEarth (Magma Elemental, display 2075 family) 1.70.

The auto-attack pulse (`0x61418a`, unit flag CGUnit_C+0xf18 & 0x10) draws a
second circle whose size and alpha follow the swing timer; WoWee pulses colour
only.

## 2. Picking units and corpses

Hover and click go through CGWorldFrame `0x4aede0` -> `0x4aeb10`:

1. The camera ray is cut at the first world surface (`0x681940`, flags
   0x1000124 / 0x10f0124).
2. `0x4ae760` adds every candidate object's models to the M2 scene query.
   Candidates by type (`0x4acce0`): units and players (unless
   UNIT_FLAG_NOT_SELECTABLE-like checks in `0x4acba0` reject them), every game
   object (when no spell is being targeted), corpses. Attached models
   (weapons, mount/rider) carry the owner's guid (`0x4abf30` recursion).
3. A dead unit the player can neither loot nor skin (`0x60e870`: health 0,
   not lootable+lootable-by-me, not UNIT_FLAG_SKINNABLE+skinnable) is held
   back and only queried when the first query hit nothing.
4. CM2Scene `0x70b2b0`:
   - broad phase `0x70a1a0`: ray against the sphere of the model's *current
     animation* bounds (sequence +0x24 box middle, +0x3c radius); when that
     radius is below 9.5e-7 (`0x896724`), the M2 header vertex box/radius
     (header +0xb4/+0xcc). Candidates are sorted by entry distance.
   - narrow phase `0x70ae50`: every skin batch (texture unit) that is drawn -
     geoset enabled, texture-unit flag 0x08 clear, model alpha x colour alpha
     x texture weight > 0 - is skinned to the current pose and tested
     triangle by triangle (`0x70a7b0`).
   - the nearest triangle hit wins. (The mode value stored per model,
     3 living unit / 2 lootable corpse / 1 usable object, is only a tie
     breaker in a second pass that runs when the first found nothing.)

So the clickable area of a unit is exactly its drawn silhouette at the current
pose; the oversized animation boxes of models like ElementalEarth (Stand
11.5 yd, Death 13.5 yd) only decide whether the triangles are tested.

## 3. Picking game objects

Same scene query and narrow phase as units: drawn batches, current pose. No
type filter for hover outside spell targeting; whether a click uses the object
is decided after picking (cursor by type). WoWee keeps its own candidate filter
(disabled/spell-focus objects skipped) and its visible-geometry raycast, which
additionally skips additive/alpha-blended batches - a difference from 8606
that is not yet resolved.
