# Mounted NPCs and detection markers

Remote mounts use the product of CreatureDisplayInfo and CreatureModelData
scale. Their rider seat comes from the model's MountMain attachment, with the
same scale applied. Huntress Leafrunner's mount display 9991 has display scale
1.5. The remote movement callback leaves mounted rider animation transitions
to the per-frame mount/rider state machine; it must not consume the movement
transition by setting the rider's `wasMoving` state first.

The authored `detectstealth_state_head` aura marker is visible while the local
observer has stealth or invisibility. Updating observer concealment must refresh
existing aura visuals even when the observed unit's aura IDs did not change.
Other aura effects remain active across those transitions.

Do not interpret SpellVisualKit type 2 as a visibility condition, or suppress
all visuals whose spells contain detection aura types. Original TBC data assigns
type 2 to ordinary state kits. Naralex's Nightmare includes detection with Sleep
art, and Brain Damage includes detection with Arcane Power art.

Private native TBC comparison established the detection eye's concealment gate.
Its visible placement and glow still differ: native eye sits higher and has
narrower glow. Those differences require separate attachment/material evidence;
the visibility fix does not claim to resolve them.

Regression: `test_animation_ids "[detection-marker]"` checks marker recognition,
observer concealment, and unrelated aura art. Paired evidence is recorded in
workspace experiment `20261116T203400Z.huntress_mount`.
