#pragma once

#include "pipeline/m2_loader.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>

namespace wowee::core {

struct CreatureGroundAlignment {
    glm::vec3 rotation{0.0f};
    float zLift = 0.0f;
};

// M2 TiltX/TiltY describe the slope along model axes. Model +X is forward;
// a forward rise therefore rotates around -Y, not around X.
// A missing support sample means the footprint is not on a known floor.
// A corpse lies along the ground on both axes whatever its model's flags:
// native 2.4.3 tilts a dead Blood Elf (BloodElfMale, no tilt flags) with a
// Desolace hillside and The Eye's Crucible ramp, where WoWee's level corpse
// sank head or feet into the slope
// (_experiments/20261118T123600Z.corpse_ramp).
// Wide flat corpses (e.g. Creature\Slime\Slime.m2, whose boundingBox is only
// 0.69 yd wide while its melted Death puddle Sub[4] spans ~5x4 yd) use the
// wider clamped vertexBox footprint when dead and lift by the upward terrain
// concavity (chord average minus center height, 0 on flat/ramp/convex ground)
// so outer puddle lobes do not sink into concave terrain hollows.
template<class SampleFloor>
CreatureGroundAlignment creatureGroundAlignment(const pipeline::M2Model& model, float scale,
                                                const glm::vec3& position, float yaw,
                                                SampleFloor&& sampleFloor, bool corpse = false) {
    assert(std::isfinite(scale) && scale > 0.0f);
    CreatureGroundAlignment out;
    out.rotation = glm::vec3(0.0f, 0.0f, yaw);
    const glm::vec3 forward(std::cos(yaw), std::sin(yaw), 0.0f);
    const glm::vec3 left(-forward.y, forward.x, 0.0f);

    glm::vec3 halfSpan = (model.boundMax - model.boundMin) * (0.25f * scale);
    if (corpse) {
        const glm::vec3 vMin = glm::min(model.vertexBoxMin, model.vertexBoxMax);
        const glm::vec3 vMax = glm::max(model.vertexBoxMin, model.vertexBoxMax);
        const glm::vec3 vertexHalf = glm::clamp((vMax - vMin) * (0.25f * scale),
                                                glm::vec3(0.0f), glm::vec3(2.0f));
        halfSpan.x = std::max(halfSpan.x, vertexHalf.x);
        halfSpan.y = std::max(halfSpan.y, vertexHalf.y);
    }

    float chordSum = 0.0f;
    int chordCount = 0;
    if ((corpse || (model.globalFlags & 0x1u)) && halfSpan.x > 0.0f) {
        const auto front = sampleFloor(position + forward * halfSpan.x);
        const auto back = sampleFloor(position - forward * halfSpan.x);
        if (front && back) {
            out.rotation.y = -std::atan2(*front - *back, 2.0f * halfSpan.x);
            chordSum += 0.5f * (*front + *back);
            ++chordCount;
        }
    }
    if ((corpse || (model.globalFlags & 0x2u)) && halfSpan.y > 0.0f) {
        const auto leftHeight = sampleFloor(position + left * halfSpan.y);
        const auto rightHeight = sampleFloor(position - left * halfSpan.y);
        if (leftHeight && rightHeight) {
            out.rotation.x = std::atan2(*leftHeight - *rightHeight, 2.0f * halfSpan.y);
            chordSum += 0.5f * (*leftHeight + *rightHeight);
            ++chordCount;
        }
    }
    if (corpse && chordCount > 0) {
        const float chordAvg = chordSum / static_cast<float>(chordCount);
        out.zLift = std::clamp(chordAvg - position.z, 0.0f, 0.15f);
    }
    return out;
}

template<class SampleFloor>
glm::vec3 creatureGroundRotation(const pipeline::M2Model& model, float scale,
                                 const glm::vec3& position, float yaw,
                                 SampleFloor&& sampleFloor, bool corpse = false) {
    return creatureGroundAlignment(model, scale, position, yaw,
                                   std::forward<SampleFloor>(sampleFloor), corpse).rotation;
}

} // namespace wowee::core
