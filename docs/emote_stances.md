# Emote stances

EmotesText.dbc links each command token to Emotes.dbc. EmoteSpecProc 1
changes the stand state using EmoteSpecProcParam. This covers LAYDOWN,
SLEEP, SIT, STAND and KNEEL without command-name special cases.
Original FrameXML maps `/lie`, `/lay`, `/liedown` and `/laydown` to LAYDOWN.

EmoteFlags bit 1 requires standing. Bit 0x200 permits sleeping/dead
stances. Reject incompatible gestures before sending text or playing an
animation. Wave permits sitting; bow and dance require standing.

Compatible seated one-shot gestures use the renderer's upper-body layer.
The lower-body stand-state animation remains active through the gesture,
including ground and chair sitting. Apply this to local and remote players.
Restart the layer for repeated gestures, and clear it when the gesture ends
or the seated base pose changes.

Reference: original TBC Emotes.dbc, EmotesText.dbc and FrameXML; field
documentation: <https://trinitycore.info/files/DBC/335/emotes>.
Native comparisons and regression results are retained under
`_experiments/20261116T234700Z.seated_emotes` in the parent workspace.
