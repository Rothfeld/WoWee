# Party identity and map positions

Run `./build/bin/test_input_integration '[party-reconnect]'`.

The group roster carries names, online state and subgroup membership. Those
answers are valid before member-stat packets arrive. `findPartyMember` therefore
looks up roster membership without requiring `hasPartyStats`; callers needing
health or coordinates validate those fields separately.

Roster updates retain each surviving GUID's independent stat snapshot. Name-query
responses notify every matching unit token, including party and raid slots.

Positions have two sources. Nearby entities contain precise canonical coordinates.
Out-of-range party stats contain coarse signed server coordinates; swap their
horizontal axes through `serverToCanonical` before using map projection. Never
project an absent stat snapshot as a real position.

Minimap party markers prefer live entities. Offscreen members use the authored
`Interface/Minimap/Rotating-MinimapGroupArrow.blp`, rotated toward the member and
centered on its alpha bounds. Both rotating and fixed minimaps use the same
projection basis for dots and arrows.
Nearby members use the original blue ObjectIcons marker regardless of class or
leadership. Marker tooltips use GameTooltipTemplate with a yellow member name.

AreaTable supplies each out-of-range member's physical map ID. Minimap requires
that map to match the player's map. World-map projection instead checks the
selected map, using an explicit source-map callback; viewing another continent
must not reject its members merely because the player stands elsewhere.

Validation gotcha: this CMaNGOS build removes nonraid members on normal logout
(`mangos-tbc/src/game/Server/WorldSession.cpp`). A disappearing group after that
operation does not exercise roster reconnect. Use packet replay to test retained
membership independently of server logout policy. A console kick also disbanded
the dedicated two-player group during live validation.

Portraits retain appearance after a previously visible member moves out of range.
A cold session which has never received that member's appearance still has no
portrait data; roster names and party-stat packets do not carry character looks.
