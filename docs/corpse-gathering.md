# Corpse gathering presentation

`build/bin/test_input_integration '[skinning]'`

Skinnable corpses expose a localized gathering label in the unit tooltip.
Creature type flags select Skinning, Herbalism or Mining. Profession rank and
carried tools determine availability. Tool identity comes from Spell.dbc and
TotemCategory.dbc, including compatible multi-purpose tools; item query responses
retain their tool category.

Lootable corpses retain the loot cursor. Without the profession, the cursor stays
Point. With the profession, available harvesting uses Skin and failed rank/tool
requirements use SkinError. Corpse tooltips use localized CORPSE in the level line
and omit living faction and health-bar presentation.

Native TBC reference, untrained Edrik hovering a fully looted Duskbat:
`../_experiments/20261116T205700Z.corpse_skinning/original-untrained-corpse.png`
shows red Skinnable and Point, with no health strip. Trained tool-only and
insufficient-rank visual states have not been independently captured; their
requirements use server checks and original spell/category data.
