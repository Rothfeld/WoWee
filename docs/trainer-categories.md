# Profession trainer groups

`shownTrainerServices` in `src/addons/lua_quest_api.cpp` groups profession
services by their authored `SkillLineAbility.dbc` membership. Abilities with a
row belong to Recipes, including Mining Apprentice (2575), which neither creates
an item nor consumes reagents. Rank-teaching spells without a row, such as
Journeyman Miner (2582), belong to Development Skills.

The player's learned skills do not determine the heading. Filters, selection,
costs and training calls all resolve the same grouped rows, including headings.
The `[trainer]` integration regression covers initial training, subsequent ranks,
recipes and collapsed groups.

Native references: Muaat's trainer API rows in
`../_experiments/20261116T202100Z.trainer_recipes/`, and Kurdram Stonehammer's
Mining Apprentice view plus authored DBC probe in
`../_experiments/20261116T214820.890029Z.mining_trainer_category/`.
