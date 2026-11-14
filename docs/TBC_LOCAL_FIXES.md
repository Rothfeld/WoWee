# Local TBC compatibility work

Run the targeted checks from the WoWee directory:

```sh
build/bin/test_update_field_table && build/bin/test_lua_handler_globals && build/bin/test_addon_lua_snippets
```

Branch: `fix/tbc-client-compatibility`, based on `ad3f84b`.

Implemented fixes:
- TBC character creation offers its nine classes; death knight is excluded.
- TBC pings use a 30-second minimum interval, including transfer pings.
- Warden reset drains its worker before releasing the module it reads.
- Relative mouse capture reports SDL failures.
- TBC update fields match CMaNGOS build 8606. Stats, appearance, aura flags,
  money and other fields use their server offsets. Unsupported packed
  game-object bytes are absent; TBC publishes separate state fields.
- TBC dropdown calls and gossip records use their original signatures.
- Lua OnLoad errors restore legacy handler globals before propagating.
- TBC string replacement and coin texture formatting are available.
- Player health changes received from the server are logged for measurement.

The FrameXML runner selects the requested expansion before loading Lua and
reads actual spell icons and DBC layouts. Its player fixture includes gossip
quests for record-width checks.

Validation: 21 update-field assertions, 65 handler-global assertions, and
10 snippet assertions passed. Actual TBC FrameXML passed assertions for build
8606, three-value gossip records, literal string replacement, coin formatting,
and a visible spell icon using the original asset path. Two existing load
errors remain in dropdown handlers. Missing inventory icons and user-reported
panel alignment are not yet verified fixed. The live client still runs the
previous executable until restarted.

Workspace evidence is in
`../_experiments/20261114T134647Z.tbc_ui_compatibility/`.
The server health rate is 1. At level 1, dwarf rogue base spirit 19 and original
TBC regeneration data predict four HP per two-second standing, out-of-combat
tick. Live health logging has not yet verified the observed rate.

Build gotchas:
- The workspace launcher sets preview MSAA to 1 because the NVIDIA driver
  loses its Vulkan device during the 4x preview. Scene MSAA is independent.
- Do not disable XInput2 for normal mouse look.
- Shader compilation overwrites tracked `assets/shaders/*.spv` binaries.
  Keep generated binaries out of source-fix commits. The diagnostic copies
  were archived as `rebuilt-shaders.tar.gz` in the experiment directory.
- The Cauldron submodule had an interrupted checkout: empty index/worktree
  caused 415 staged deletion reports. Restoring its recorded HEAD repaired it.
