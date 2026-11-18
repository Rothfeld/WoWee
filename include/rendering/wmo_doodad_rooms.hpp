#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace wowee::rendering {

/// When an M2 instance placed as WMO doodads is drawn. M2Renderer hands back
/// the instance already standing at a spot (createInstanceWithMatrix's
/// dedup), so one instance can stand for several MODD entries.
struct WmoDoodadRooms {
    bool placed = false;          // some MODD entry has been placed as it
    uint32_t wmoInstance = 0;     // 0: drawn whenever in view
    std::vector<uint16_t> rooms;  // groups of wmoInstance; drawn while one is
};

/// What the instance becomes once another MODD entry of WMO instance
/// `wmoInstance`, listed by `rooms` (pipeline::wmoDoodadRooms; empty: drawn
/// with the building), is placed as it: drawn while any of its entries would
/// be. Two buildings' entries leave it drawn whenever in view, since no single
/// room list can say when.
///
/// Black Temple's MODD lists the courtyard's trees, braziers and golden
/// statues twice at one spot: for the courtyard (group 8) and for
/// BossSummit_facade (48), a backdrop seen only from Illidan's summit. Both
/// become one instance; when the second entry's rooms replaced the first's,
/// the courtyard's own tree went with the facade.
inline WmoDoodadRooms addWmoDoodad(WmoDoodadRooms current, uint32_t wmoInstance,
                                   const std::vector<uint16_t>& rooms) {
    const bool first = !current.placed;
    current.placed = true;
    if (rooms.empty() || (!first && (current.wmoInstance == 0 || current.wmoInstance != wmoInstance))) {
        current.wmoInstance = 0;
        current.rooms.clear();
        return current;
    }
    current.wmoInstance = wmoInstance;
    for (uint16_t room : rooms) {
        if (std::find(current.rooms.begin(), current.rooms.end(), room) == current.rooms.end())
            current.rooms.push_back(room);
    }
    return current;
}

}  // namespace wowee::rendering
