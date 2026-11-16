# Zone announcements

World entry and UI reload invalidate the renderer's announced area snapshot.
The next renderer update resolves WMO containment, zone, and subzone before
sending one zone event to FrameXML. A zone change takes precedence over a
subzone change within the same update.

This initialization matters because initial network events can arrive before
FrameXML loads. `ZoneTextFrame.zoneText` must be initialized before the first
ordinary subzone crossing. Otherwise that crossing can display the full zone
title and territory line.

The original `zonetext.lua` owns text, color, territory lines, and fading. No
Canals-specific rule is needed. `UI area event` logs include packet zone,
resolved zone/area, and WMO names to diagnose event ordering.

Validation: paired Stockades doorway captures show the short green `The Canals`
title in both clients. A fresh launch after the lifecycle change emits
`ZONE_CHANGED_NEW_AREA` with resolved Stormwind City / The Canals immediately
after UI initialization. Artifacts: workspace experiment
`20261116T142240Z.canals_title`.
