#pragma once

#include "core/geoset_rules.hpp"

namespace wowee::core {

struct NpcLowerBodyEquipment {
    uint32_t boots = 0;       // Boots GeosetGroup1.
    uint32_t pants = 0;       // Legs GeosetGroup1: waist cuff.
    uint32_t kneepads = 0;    // Legs GeosetGroup2.
    uint32_t robe = 0;        // Chest/legs GeosetGroup3, chest takes priority.
};

inline void selectNpcLowerBodyGeosets(std::unordered_set<uint16_t>& selected,
                                     const std::unordered_set<uint16_t>& available,
                                     const NpcLowerBodyEquipment& equipment) {
    for (auto it = selected.begin(); it != selected.end();) {
        const auto group = geosetGroup(*it);
        if (group == 5 || group == 9 || group == 11 || group == 13)
            it = selected.erase(it);
        else
            ++it;
    }
    const uint16_t requested[] = {
        equippedGeoset(501, equipment.boots),
        equippedGeoset(901, equipment.kneepads),
        equippedGeoset(1101, equipment.pants),
        equippedGeoset(1301, equipment.robe),
    };
    for (const auto id : requested) {
        const auto resolved = resolveGeoset(id, available);
        if (resolved != 0) selected.insert(resolved);
    }
}

} // namespace wowee::core
