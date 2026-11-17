# Equipment proficiencies

Run merchant packet replay checks with `build/bin/test_input_integration '[merchant]'`.

Equipment usability combines learned proficiency spells and server-granted
proficiency masks. Spell.dbc effect 60 grants the `EquippedItemSubClassMask` for
`EquippedItemClass`. An ordinary ability with equipment requirements does not
grant proficiency. Removing a learned proficiency removes that contribution.

CMaNGOS TBC can send no `SMSG_SET_PROFICIENCY` packet during login: passive
spells are applied while the player is loading, before normal session packet
delivery is enabled. `SMSG_INITIAL_SPELLS` still supplies learned spells.
Treating packet masks alone as the complete state makes usable armor red in
merchant windows after every login. No class-specific whitelist is needed.

Required level, class/race restrictions, skills and reputation remain separate
merchant checks. Affordability does not determine equipment usability.
