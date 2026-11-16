# Interior minimaps

Authored WMO room tiles use two pixels per local yard. Tile padding extends from
the group's minimum bounds; it must not be stretched across the room bounds.

All authored indoor room tiles participate in the map. Height orders overlapping
tiles, with the closest vertical layer drawn last; it does not remove rooms.
Testing the player's exact Z excludes lower corridor tiles while standing on
entrance stairs. The Stockades entrance is a useful regression location: its
cross corridor and cells must remain visible at wide zoom.

Blizzard FrameXML owns the compass marker and difficulty presentation. Do not hide
`MinimapNorthTag` or draw an additional difficulty label over its minimap.

For validation, compare equal positions and zoom levels, then cross an instance
boundary. Keep room layout, scale, player marker, compass, title, and party portrait
in the review; matching one widget does not validate the whole frame.
