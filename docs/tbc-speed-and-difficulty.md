# TBC speed and level difficulty

Run protocol and difficulty checks from the repository root:

```sh
build/bin/test_game_replay '[speed]'
build/bin/test_nameplate_stacking '[difficulty]'
```

TBC forced speed changes carry a packed GUID, uint32 counter and float speed.
Forced run speed adds a tracking byte before the float; walk and swim do not.
The decoded run speed drives both local movement and stride playback. ACKs
retain the full GUID required by the client-to-server packet layout.

Enemy nameplate level colors use the CMaNGOS creature XP difficulty thresholds,
including gray at and below the no-XP boundary. At player level 40, level 31 is
gray, 32 green, 38 yellow, 43 orange, and 45 red.

Validation: 17 speed packet assertions and 10 difficulty assertions pass.
Workspace evidence: `_experiments/20261116T134800Z.daze_speed/` and
`_experiments/20261116T134557Z.enemy_level_color/`.
