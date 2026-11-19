# Readable game object animation

Run the policy regression from the repository root:

```sh
build/bin/test_animation_ids '[game-object-pose]'
```

Readable books use the TEXT game object type (9). Their authored Open/Close
sequences follow server state and hold their final frame. They share this
policy with doors, buttons, chests, traps, goobers, destructible buildings,
and trapdoors. Animated scenery keeps its own continuous playback.

Without state sequences (146-149) a door, button or chest shows READY through
its bind pose. A trap keeps Stand running instead: the Molten Core rune rings
(TRAP 178187-178193, `FlameCircle.m2`, Stand and Hold only) are READY, and the
original client draws them as a flame front running round the rune once a
second. Their type often arrives after their state; the template response then
releases such traps (`gameObjectBindPoseShowsReady`).

A stateful object whose model has Destroy (150) but neither Open (148) nor
Opened (149) shows ACTIVE with Destroy, held at its last frame
(`gameObjectActiveShowsDestroy`), and READY with Stand. Original 2.4.3:
Zul'Gurub's Brazier of Madness (BUTTON 180327, `G_BrazierTroll.m2`: Destroy,
Stand, Custom0) goes out when the Gurubashi Mojo Madness extinguishes it and
is dark when it comes into view again; Blackrock Depths' Thunderbrew Lager
Keg (GOOBER 164911, `G_Keg.m2`) bursts. Open wins where the model has it: the
Supply Crate (GOOBER 166872, `G_Crate01.m2`, which also has Destroy) lifts its
lid and the Bar Door (DOOR 170571) swings open. GENERIC objects keep Stand
(Troll Hero Fire/Air Portal, `FireRift.m2`/`AirRift.m2`). Captures:
`_experiments/20261118T230000Z.brazier_state/` in the workspace root. Its
state can arrive before its type; the template response applies the pose then.

READY loops Closed (147) where the model has it, except on GENERIC (5) and
CAMERA (13) objects, which loop Stand (`gameObjectReadyShowsStand`).
`ScholomanceCrystalBall01.m2` (display 4891) shows it: Stand is constant,
Closed fades its additive `ORBREFLECT.BLP` layer from 1 to 0 over 1 s. Original
2.4.3 pulses once a second as Orb of Command (QUESTGIVER 179879), Archmage
Vargoth's Orb (SPELL_FOCUS 183507) and Orb of Domination (GOOBER 177808), and
glows steadily as Orb of the Blue Flight (GENERIC 188415) and Magisters'
Terrace's Scrying Orb (CAMERA 187578). Spawned GENERIC models with Closed and no
Stand send Stand to Closed through their playable lookup, so they are unchanged.
The type can arrive after the state; the template response then swaps Closed
for Stand. Paired 20 Hz recordings:
`_experiments/20261119T1445Z.orb_pulse/` in the workspace root.

Every M2 instance starts on its Stand sequence, resolved through the model's
pre-WotLK playable animation lookup when it has no Stand
(`pipeline::standSequenceIndex`). Doodad cage doors in Zul'Gurub
(`g_cage02.m2`, `holdingpen.m2`: Open, Opened, Close, Closed; Stand -> Closed)
stand shut, and the brazier, which lists Destroy first, starts lit.

Both immediate and delayed template responses use
`core/game_object_animation_policy.hpp`. Do not unfreeze a state-driven object
when its template arrives: `setInstanceAnimationFrozen(false)` clears
`holdAtEnd`, which would loop the selected transition.

TBC publishes `GAMEOBJECT_STATE` at absolute field 14. It does not publish
Wrath's packed `GAMEOBJECT_BYTES_1`. Creation must apply state even when zero
is omitted from the update mask. Keep runtime `update_fields.json` synchronized
with the packaged table before launching a binary that consumes this field.

Reading a TEXT object temporarily plays its Open sequence for the reader.
Closing the reader restores the object's current server state. Changing books
restores the previous object first. Leaving TEXT interaction range closes the
reader and restores its pose.

The World Tree and the Emerald Dream (entry 175730, display 559) uses
`World/Goober/G_BookOpenMediumGreen.m2`. Its authored sequence IDs are
Close 146, Closed 147, Open 148, and Opened 149. These IDs describe model
animations; the server state still determines which transition is held.
