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
| Chest GeosetGroup2 | 1001 + value | Keldric Boucher 1431 (doublet flap 1002) |
| Tabard GeosetGroup1 | 1201 + value, none under a robe | Scholomance Adept 11150 (flap 1202) |
| CharacterFacialHairStyles | 100/200/300 + value, 0 draws nothing | Gnome Pit Crewman 5428 (no goatee), 5435 (no earrings) |

Chest robe selection takes priority over leg kilts. Nessa and Handiir use zero
values; their original models contain no bare 901 or 1101 mesh. Peacekeeper legs
37026 select robe 1302. Group 13 must not use Legs GeosetGroup1.

Group 10 and the tabard flap: the TBC character models carry only 1002 and
1202 in those groups (naga female and skeleton female have neither), so 1001
and 1201 mean none. 1002 used to be
left on every NPC (a skirt ring round the hips) and the tabard asked for 1201,
which no model has, so no tabard flap was ever drawn. Native draws no tabard
flap over a robe skirt from chest or legs (Father Gavin 1616, Scarlet Augur
2495, Magus Zabraxis 16372). A zero facial variant is none; substituting the
group's lowest member put a goatee (102) on clean-shaven gnome men and earrings
(202) on gnome women.

Native draws a character NPC whose CreatureDisplayInfoExtra has no BakeName
entirely white, hair and all (Ironforge Brigade Mortarman 15444, Sunhawk
Ambusher 17116, Male Ghost 15331); WoWee composites those from CharSections and
the items. With a bake, native and WoWee both draw the bake. Comparison:
`_experiments/20261119T130500Z.npc_clothes/` in the workspace.

Private native comparisons and original asset inspection live in the workspace
experiment `20261116T224412Z.npc_appearance_audit`. Nessa feet, Handiir waist and
Peacekeeper robe were compared at matched camera coordinates. These checks do
not validate mount seats, aura attachments or ranged weapon sheath rules.
