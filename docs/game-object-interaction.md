# Game-object interaction

`build/bin/test_scene_pick '[quest-object]'` checks conditional activation and
large-object surface picking.

`GAMEOBJECT_FLAGS` bit4 requires the per-player `GAMEOBJECT_DYNAMIC` activation
bit1. Quest3524 activates Beached Sea Creature175207: flags remain4, dynamic
changes from0 to9 (activation plus sparkle). Picker and direct use share this
eligibility check. Runtime expansion metadata must match the packaged table.

Renderable M2 objects use their visible triangles for pick distance. The bound
sphere only rejects rays outside the object. Its far exit is not a valid surface
when the camera is inside the sphere; terrain behind the actual mesh must not
occlude the object. `ScenePickParams::trace` logs intersected object candidates,
actual server fields, mesh distance, and any occlusion rejection on clicks.

Lock.dbc slots describe alternative opening methods. The first skill method
chooses the cursor. An empty LockType cursor uses the normal opening/gear cursor;
it does not request the icon from a later alternative. Lock259 uses Open13 first
and Herbalism2 later. Native quest-active sea creature shows the gear cursor.
