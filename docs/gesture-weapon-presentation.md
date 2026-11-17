# Gesture weapon presentation

Run `build/bin/test_animation_ids '[gesture-weapons]'` for the presentation-policy tests.

Emotes.dbc selects the gesture and its NOSHEATHE override. AnimationData.dbc
supplies the weapon-presentation flags. Ordinary Wave and WAVE_NOSHEATHE share
animation 67, so network callbacks retain the original emote ID: animation ID
alone cannot distinguish their weapon policy.

While a sheathing gesture is active, equipped weapons use their authored sheath
attachment and orientation. Held tools without a sheath attachment are hidden.
The renderer restores their ordinary attachment when the full-body or upper-body
gesture finishes or is interrupted. This temporary presentation never writes
the server's persistent sheath state.

Remote players, NPC virtual equipment, and local equipment all provide alternate
sheath metadata when attaching their weapon models. Armor attachments are not
affected. Normal Wave uses AnimationData Weaponflags 0x10; the Emotes 0x4
NOSHEATHE override keeps weapons in hand even for that animation.

Original TBC DBC rows and native empty-hand seated-wave reference are recorded
in the workspace experiments `20261117T000550Z.gesture_weapon_visibility` and
`20261116T234700Z.seated_emotes`.
