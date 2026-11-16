# Current map zone

World-map opening and updates use Renderer::getCurrentZoneId(). Inside a named WMO, the renderer resolves WMOAreaTable field 10 through AreaTable parent IDs. Groups without an area assignment inherit the root WMO assignment. Outdoors, terrain area resolution supplies the zone.

WMO display names alone do not establish area identity. Keep numeric area resolution alongside containment and use it for GetCurrentAreaId and GetCurrentZoneId. Server world-state zones can remain stale during local movement; do not feed those directly into the world map.

Validated with all 186 Ironforge WMO area assignments in extracted TBC data and live map opening/reopening in Mystic Ward. Artifacts: workspace `_experiments/20261116T085457.758223Z.map_current_wmo_zone/`.
