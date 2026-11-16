# World object tooltips

Hover a labelled sign in game to show its name in `GameTooltip` above the cursor.

Generic object templates use `data[0]` for the floating label and `data[1]` for
model highlighting. These flags are independent. Chairs do not show name labels.
Meeting stones (type 23) highlight while hovered and show their destination
from template `data[2]` / AreaTable below the object name. Using an object
preserves the selected unit: meeting stones need that selection as the summon
target. Template `data[0]` and `data[1]` bound the summoner's level; an ineligible
player receives the TBC level-requirement UI error before sending object use.
CMaNGOS checks group membership and both players' levels in `GameObject::Use`.
Object names pass through the tooltip API as escaped text; they must never be
interpreted as Lua source. Unit mouseover remains separate from object hover.

`ANCHOR_CURSOR` places the tooltip bottom-center at the cursor each rendered
frame. UI scale and frame scale both affect its anchor coordinates. The standard
FrameXML tooltip supplies the font, gold title, background, border and screen
clamping. A new owner clears cursor tracking unless it requests `ANCHOR_PRESERVE`.
World tooltip updates only hide tooltips owned by `WorldFrame`.

Cursor-anchored tooltips enter the renderer's sizing pass when their shown
ancestor chain is visible, even before they have an anchor. `SetOwner` clears
the previous anchor; the sizing pass assigns the cursor anchor. Requiring an
already drawable frame here prevents the first tooltip from ever appearing.

TBC object-query responses contain icon, opening-text and closing-text strings
before the 24 template fields. All three must be consumed to preserve label
and highlight flags. This layout matches CMaNGOS TBC's `QueryHandler.cpp`.

For original TBC build 8606, the game-object branch at `0x4A1C40` selects cursor
anchor 6 for floating labels and calls the object tooltip builder at `0x54FB00`.
The title color at `0xB9F648` is packed `0xFFFFD200`. Cursor placement at
`0x54BE20` anchors point 7 (BOTTOM) to root point 6 (BOTTOMLEFT) at cursor
coordinates. These are executable virtual addresses for this build only.

Sign hover strength is 0.25 of pressed-object strength; meeting stones use 0.7.
Matched Stockades captures confirm hover illumination, destination text, and
the ineligible-player error. At level 21, using the stone preserves the party
target and starts server spell 23598, creating the summoning portal. Highlight
tint is approximate; the renderer's warm lift differs from native illumination.
