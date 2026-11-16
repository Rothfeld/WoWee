# NPC lower-body equipment meshes

Run `build/bin/test_geoset_rules '[npc-equipment]'` from the WoWee directory.

Humanoid NPC skins contain baked equipment textures. Mesh selection still uses
CreatureDisplayInfoExtra equipment display IDs and ItemDisplayInfo geoset fields.
Each lower-body group selects one authored variant; an absent bare variant draws
nothing for that group.

| Equipment field | Mesh group | Example |
| --- | --- | --- |
| Boots GeosetGroup1 | 501 + value | Nessa Shadowsong boots 7029 |
| Legs GeosetGroup1 | 1101 + value | Handiir legs 36828 |
| Legs GeosetGroup2 | 901 + value | Knee overlays |
| Chest/legs GeosetGroup3 | 1301 + value | Peacekeeper legs 37026 |

Chest robe selection takes priority over leg kilts. Nessa and Handiir use zero
values; their original models contain no bare 901 or 1101 mesh. Peacekeeper legs
37026 select robe 1302. Group 13 must not use Legs GeosetGroup1.

Private native comparisons and original asset inspection live in the workspace
experiment `20261116T224412Z.npc_appearance_audit`. Nessa feet, Handiir waist and
Peacekeeper robe were compared at matched camera coordinates. These checks do
not validate mount seats, aura attachments or ranged weapon sheath rules.
