#pragma once

#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>

namespace wowee::rendering {

/// Effective horizontal clearance radius of an M2 header collision box around
/// model origin (0, 0) for GameObject LootFX scaling.
///
/// Solid boxy containers that enclose model origin on all four horizontal sides
/// (e.g. G_ChestTitan.m2 3.14 x 4.11 x 2.34, boundRadius 2.84; TitanChest.m2
/// 3.02 x 4.55 x 2.11, boundRadius 2.93) return `headerBoundRadius` so LootFX
/// particles clear the chest walls instead of spawning occluded inside the lid.
///
/// Thin/offset props (GnomeBigGear.m2 dx=0.46, AllianceVeteranBanner02.m2 dx=0.55,
/// WoodSignPointerWorn01.m2 origin outside box) and wide flat floor-skirt props
/// whose bounding sphere is dominated by ground chains/plinths rather than an
/// enclosing container (NOX_ANVIL.m2 8.19 x 6.67 x 1.98, boundRadius 5.37 =
/// 2.71x height) return 0 so LootFX scales by `collisionBoxHeight` (1.98 on
/// Icebellow Anvil, matching native TBC 2.4.3) instead of blowing up 4x across
/// the room.
[[nodiscard]] inline float m2CollisionSparkleRadius(const glm::vec3& headerBoxMin,
                                                    const glm::vec3& headerBoxMax,
                                                    float headerBoundRadius) {
    if (!std::isfinite(headerBoundRadius) || headerBoundRadius <= 0.0f) return 0.0f;
    const glm::vec3 bmin = glm::min(headerBoxMin, headerBoxMax);
    const glm::vec3 bmax = glm::max(headerBoxMin, headerBoxMax);
    const float boxHeight = bmax.z - bmin.z;
    if (boxHeight <= 0.0f) return 0.0f;
    const float minOriginClearanceXY = std::min({-bmin.x, bmax.x, -bmin.y, bmax.y});
    if (minOriginClearanceXY < 0.5f * boxHeight) return 0.0f;
    if (headerBoundRadius > 1.5f * boxHeight) return 0.0f;
    return headerBoundRadius;
}

}  // namespace wowee::rendering
