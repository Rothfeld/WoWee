# Non-combat companion presentation

```sh
./build/bin/test_input_integration '[companion]'
```

Non-combat companions use creature type 12. They show overhead names governed
by `UnitNameCompanionName`, without friendly health nameplates. Tooltip health
bars remain visible, matching the original TBC interface.

Player-controlled companions use the player-controlled selection color rules.
Unowned ambient creatures, including Sixx's moths, retain their NPC reaction
colors. A companion's owner subtitle comes from `UNIT_FIELD_CREATEDBY`.
CMaNGOS `Pet::SetOwnerGuid` supplies `UNIT_FIELD_SUMMONEDBY` only for combat
pets; mini pets omit it. Reading SUMMONEDBY for a Blue Moth produces an empty
owner even when the character name is already cached.

The tooltip contains the name, localized owner companion title, and level.
Inherited player faction metadata does not belong in that tooltip.

Update-field profiles under `Data/expansions` must also reach the runtime data
directory. Updating only source JSON leaves a running development client with
the old field map. The startup log identifies its active expansion directory.
