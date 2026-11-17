#pragma once

// The size of a unit as build 8606 measures it for the ground selection circle.
//
// Wow.exe 2.4.3.8606 keeps the radius in CGUnit_C+0xfb8 and recomputes it in
// 0x60e560 whenever the display, scale or mount changes. The box it measures
// is CreatureModelData's GeoBox (fields 17-22, copied by 0x60ce50), not the
// M2 bounds and not the collision width; a mounted unit's box is moved up to
// the mount's MountHeight and joined with the mount's own GeoBox (0x60ce50 ->
// 0x604bd0). The circle itself (0x614070) is Textures\UnitSelectTexture.blp
// projected over the square position +- radius.
//
// Full evidence: docs/unit-size-rules.md.

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>

namespace wowee::core {

/// CreatureModelData GeoBox, model space, yards.
struct ModelGeoBox {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
};

/// 0x60ce50: the rider's box lifted so its vertical middle sits at the
/// mount's MountHeight, then the union with the mount's box (0x604b70).
inline ModelGeoBox mountedGeoBox(const ModelGeoBox& rider, const ModelGeoBox& mount,
                                 float mountHeight) {
    const float lift = mountHeight - (rider.max.z - rider.min.z) * 0.5f;
    ModelGeoBox raised = rider;
    raised.min.z += lift;
    raised.max.z += lift;
    return {glm::min(raised.min, mount.min), glm::max(raised.max, mount.max)};
}

/// 0x60e560. `scale` is the unit's object scale (CGUnit_C vtable +0x58:
/// OBJECT_FIELD_SCALE_X x display/model/family scale x mount scale).
///
/// Half the horizontal diagonal of the box, times scale, compressed by a
/// square root; past 5 yards it grows again by 0.06 per square yard; never
/// more than 10. A box with no horizontal extent (|dx|, |dy| < 2.4e-7) gets
/// a flat 1.2 that the scale does not touch.
inline float selectionCircleRadius(const ModelGeoBox& box, float scale) {
    constexpr float kEpsilon = 2.384185791015625e-07f;  // 0x8907e8
    constexpr float kDegenerate = 1.2f;                 // 0x8c5450
    constexpr float kKnee = 5.0f;                       // 0x897374, 0x892da8
    constexpr float kGrowth = 0.06f;                    // 0x8c5448
    constexpr float kMax = 10.0f;                       // 0x88d7c0
    const float dx = box.max.x - box.min.x;
    const float dy = box.max.y - box.min.y;
    if (std::abs(dx) < kEpsilon && std::abs(dy) < kEpsilon) return kDegenerate;
    const float halfDiagonal = std::sqrt(dx * dx + dy * dy) * 0.5f;
    const float scaled = halfDiagonal * scale;
    float radius = std::sqrt(scaled);
    if (scaled > kKnee) {
        const float over = scaled - kKnee;
        radius += over * over * kGrowth;
    }
    return std::min(radius, kMax);
}

} // namespace wowee::core
