# Meeting stone summoning effects

## Dungeon entrance swirl

`InstancePortal.m2` uses two spherical emitters with flags `0x39`. Bit `0x20`
keeps living particles in emitter space: simulation contracts their radius,
and the current model and bone matrices rotate their positions and velocities
at draw time. Applying the bone matrix only at birth loses the swirl.
`Summon_Ritual.m2` uses `0x19`; its particles remain in world space after birth.
Static skeletons also need initialized bone matrices before particle drawing.

The dungeon portal's mesh has a permanently zero texture-weight track. Resolve
the batch's transparency index through `textureWeightLookup`, and preserve zero
when every authored sequence/key is zero. Treating all zero first keys as full
opacity exposes solid white ring segments from inside the dungeon. An animated
fade-in or sequence-dependent visibility must not be classified as permanently
hidden. Particle emitters remain visible independently of mesh opacity.

The regression fixture checks a contracting portal particle under a rotated
bone and verifies that detached particles receive no second transform.
Paired four-second Stockades entrance recordings are stored in
`../_experiments/20261116T114901.087557Z.portal_swirl/verified/`.
These contain 120 frames per client with at most 1.07 ms capture skew.
Optical flow on the translucent effect is noisy and does not establish exact
angular-speed parity; authored bone attachment is covered by the fixture.

WMO-only dungeon loading must preserve `WMOModel::sourcePath` from the WDT.
The interior minimap uses that filename to resolve textures; an empty filename
causes a substring exception when entering Stockades.

## Summoning channel and portal

Run the asset-layout regression tests from the repository root:

```sh
build/bin/test_m2_ribbon_tracks
build/bin/test_m2_classifier_foliage
```

TBC spell 23598 uses SpellVisual 1523. Its channel kit is 729, with character
animation 124 and Magic_PreCast_Hand effects on attachment 2 (left hand) and
attachment 1 (right hand). Channel messages carry packed caster GUIDs in
CMaNGOS TBC. Duration updates to zero remove the attached effects.

The server-created portal uses display 1327 and
`World/Generic/ActiveDoodads/InstancePortal/Summon_Ritual.m2`. Its four vertices
are a visible textured card, not an emitter volume to suppress. The model's
billboard bone supplies its orientation; no additional whole-model spin is
needed. Portal effects do not participate in solid model collision.

The version-260 asset contains four spherical emitters. Equal minimum and
maximum radii produce rings. Zero horizontal spread places those rings in
the YZ plane. The black-center and green-flare emitters have tail style 1 at
offset 0x2c and tail multipliers 2 and 3 at offset 0x17c. They need velocity-
aligned quads, not point sprites. Head particles retain square billboards;
style 2 draws both head and tail. Wrath stores style at 0x2d and tail length
at 0x15c. Both layouts have regression fixtures.

The renderer uploads one instance per head/tail quad and generates its six
vertices in the vertex shader. Buffer capacity includes both quads for
style 2. Authored material blends, effect meshes and particle emitters must
remain active together. Empty spell visual stages do not substitute other
stages.

Validation used paired 1280x720 captures of the same summon from the original
2.4.3 client and WoWee, with matching camera matrices. Checked channel pose,
both hand effects, black center, radial green tails, and cancellation. The
original retains a faint portal fade briefly after cancellation; WoWee removes
the server object immediately. Particle phase and exact fade timing are not
pixel-equivalent. No executable decompilation was used.

Evidence remains outside the source repository:
`../_experiments/20261116T111737.987799Z.meeting_stone_effects/`.
The `final-*` images show the fresh summon; `cancel-*` show cancellation.
Tests passed: 58 particle/ribbon assertions and 213 classification assertions.

Other visible discrepancies in these captures: player selection-ring size,
minimap content, and nameplate opacity. Regrouping also exposed existing Lua
errors for CanSendLFGQuery, IsRealPartyLeader and ArenaFrame arena-team values.
These are independent of the summon visual path.
