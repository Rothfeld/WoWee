# World object tooltips

Hover a labelled sign in game to show its name in `GameTooltip` above the cursor.

Generic object templates use `data[0]` for the floating label and `data[1]` for
model highlighting. These flags are independent. Chairs do not show name labels.
Object names pass through the tooltip API as escaped text; they must never be
interpreted as Lua source. Unit mouseover remains separate from object hover.

`ANCHOR_CURSOR` places the tooltip bottom-center at the cursor each rendered
frame. UI scale and frame scale both affect its anchor coordinates. The standard
FrameXML tooltip supplies the font, gold title, background, border and screen
clamping. A new owner clears cursor tracking unless it requests `ANCHOR_PRESERVE`.
World tooltip updates only hide tooltips owned by `WorldFrame`.

For original TBC build 8606, the game-object branch at `0x4A1C40` selects cursor
anchor 6 for floating labels and calls the object tooltip builder at `0x54FB00`.
The title color at `0xB9F648` is packed `0xFFFFD200`. Cursor placement at
`0x54BE20` anchors point 7 (BOTTOM) to root point 6 (BOTTOMLEFT) at cursor
coordinates. These are executable virtual addresses for this build only.

Hover highlight strength is currently 0.25 of pressed-object strength. Exact
brightness parity still requires matching native and WoWee captures.
