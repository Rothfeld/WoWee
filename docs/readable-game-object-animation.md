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
