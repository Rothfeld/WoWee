#pragma once

#include <glm/glm.hpp>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <span>
#include <cstdint>
#include <vector>

#include "pipeline/m2_loader.hpp"
#include "rendering/m2_track_sampler.hpp"

namespace wowee::rendering {

// Unnormalized local direction preserves the world-space ray parameter under
// instance scaling. Two-sided triangles match cursor occlusion on solid props.
inline float pickVisualTriangles(std::span<const glm::vec3> vertices,
                                 std::span<const uint16_t> indices,
                                 const glm::vec3& origin, const glm::vec3& direction,
                                 float limit) {
    assert(indices.size() % 3 == 0);
    for (size_t i = 0; i < indices.size(); i += 3) {
        assert(indices[i] < vertices.size() && indices[i + 1] < vertices.size() && indices[i + 2] < vertices.size());
        const auto& a = vertices[indices[i]];
        const glm::vec3 edge1 = vertices[indices[i + 1]] - a;
        const glm::vec3 edge2 = vertices[indices[i + 2]] - a;
        const glm::vec3 p = glm::cross(direction, edge2);
        const float determinant = glm::dot(edge1, p);
        if (std::abs(determinant) < 1e-8f) continue;
        const glm::vec3 offset = origin - a;
        const float u = glm::dot(offset, p) / determinant;
        if (u < 0.0f || u > 1.0f) continue;
        const glm::vec3 q = glm::cross(offset, edge1);
        const float v = glm::dot(direction, q) / determinant;
        if (v < 0.0f || u + v > 1.0f) continue;
        const float t = glm::dot(edge2, q) / determinant;
        if (t > 0.0f && t < limit) limit = t;
    }
    return limit;
}

/// MOPY material of a WMO triangle no batch draws: collision only, invisible.
inline constexpr uint8_t kWmoCollisionOnlyMaterial = 0xFF;

/// M2 skin batch (texture-unit) flag 0x08 excludes the batch from CM2Scene
/// narrow-phase ray picking (build 8606 0x70ae50; docs/unit-size-rules.md).
/// Authored on aura/light-shaft columns (AuraBlueVeryTall.m2, AuraPurpleShort.m2,
/// UtherShrineLightBeam.m2) and Lightwell's vertical beam (G_HolyLightWell.m2
/// batch 0, while its solid bowl batch 1 has flag 0x00 and remains pickable).
inline constexpr uint8_t kM2BatchFlagNonPickable = 0x08u;

[[nodiscard]] constexpr bool m2BatchIsPickable(uint8_t batchFlags, uint16_t submeshLevel,
                                               uint8_t blendMode, float batchOpacity,
                                               bool starLayer, bool ownSurface) {
    if ((batchFlags & kM2BatchFlagNonPickable) != 0) return false;
    if (submeshLevel != 0 || batchOpacity < 0.01f || starLayer) return false;
    return ownSurface || blendMode < 2;
}

/// World-surface M2 occlusion rule for unit/corpse/object ray picking
/// (build 8606 CGWorldFrame 0x4aeb10 -> 0x681940 world ray cut; verified live
/// on WOWEE_PAIR=7):
/// - Doodads without authored collision triangles (elwynnbush09, tombstones,
///   ground clutter, tree leaf cards) never block unit clicks.
/// - Colliding doodads (elwynntreemid01 trunk, generalweaponrack01) cut the ray
///   at their authored M2 collision triangles, even across visual gaps.
/// - Open doors (skipCollision), runtime spell visuals, and invisible
///   collision-only hulls whose batches all have 0 opacity (AuctioneerCollision,
///   Sunwell_BossCollision01/02) do not hide units behind them from the mouse.
[[nodiscard]] constexpr bool m2InstanceOccludesWorldPick(
    bool hasCollisionTriangles, bool skipCollision, bool isInvisibleTrap,
    bool isRuntimeSpellVisual, bool isSpellEffect, bool hasVisibleOccluderBatch) {
    return hasCollisionTriangles && !skipCollision && !isInvisibleTrap &&
           !isRuntimeSpellVisual && !isSpellEffect && hasVisibleOccluderBatch;
}

[[nodiscard]] inline bool m2ModelHasVisibleOccluderBatch(
    const pipeline::M2Model& model, float instanceOpacityScale = 1.0f) {
    for (const auto& b : model.batches) {
        const uint8_t blend = b.materialIndex < model.materials.size()
                                  ? static_cast<uint8_t>(model.materials[b.materialIndex].blendMode)
                                  : 0u;
        float opacity = instanceOpacityScale;
        if (b.transparencyIndex < model.textureWeightLookup.size()) {
            const uint16_t wi = model.textureWeightLookup[b.transparencyIndex];
            if (wi < model.textureWeightTracks.size() &&
                !m2_track::opacityVaries(model.textureWeightTracks[wi]) &&
                m2_track::isAlwaysZero(model.textureWeightTracks[wi])) {
                opacity = 0.0f;
            } else if (wi < model.textureWeights.size() &&
                       (wi >= model.textureWeightTracks.size() ||
                        !m2_track::opacityVaries(model.textureWeightTracks[wi]))) {
                opacity *= model.textureWeights[wi];
            }
        }
        if (b.colorIndex < model.colorAlphaTracks.size() &&
            !m2_track::opacityVaries(model.colorAlphaTracks[b.colorIndex]) &&
            m2_track::isAlwaysZero(model.colorAlphaTracks[b.colorIndex])) {
            opacity = 0.0f;
        }
        if (m2BatchIsPickable(b.flags, b.submeshLevel, blend, opacity, false, /*ownSurface=*/false)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] inline float pickM2CollisionMesh(
    const pipeline::M2Model& model,
    const glm::vec3& origin,
    const glm::vec3& direction,
    float limit,
    bool skipCollision = false,
    bool isInvisibleTrap = false,
    bool isRuntimeSpellVisual = false,
    bool isSpellEffect = false,
    float instanceOpacityScale = 1.0f) {
    const bool hasCollision = model.collisionIndices.size() >= 3 && !model.collisionVertices.empty();
    const bool hasVisibleBatch = m2ModelHasVisibleOccluderBatch(model, instanceOpacityScale);
    if (!m2InstanceOccludesWorldPick(hasCollision, skipCollision, isInvisibleTrap,
                                     isRuntimeSpellVisual, isSpellEffect, hasVisibleBatch)) {
        return limit;
    }
    return pickVisualTriangles(model.collisionVertices, model.collisionIndices,
                               origin, direction, limit);
}

/// The nearest drawn WMO triangle among `triangleStarts` (offsets into
/// `indices`, one per triangle) before `limit`. A collision-only triangle
/// stops the player and the camera but is not seen, so it does not hide what
/// stands behind it from the mouse: SSC's invisible hull around the Lady Vashj
/// Bridge Console (CoilfangRaid group 10, material 0xFF, MOPY 0x48) sits
/// 2.5 yards in front of the console, and original 2.4.3 shows the console's
/// gear and name through it (pair 17).
inline float pickDrawnWmoTriangles(std::span<const glm::vec3> vertices,
                                   std::span<const uint16_t> indices,
                                   std::span<const uint8_t> materials,
                                   std::span<const uint32_t> triangleStarts,
                                   const glm::vec3& origin, const glm::vec3& direction,
                                   float limit) {
    assert(materials.size() * 3 == indices.size());
    for (const uint32_t start : triangleStarts) {
        assert(start % 3 == 0 && start + 3 <= indices.size());
        if (materials[start / 3] == kWmoCollisionOnlyMaterial) continue;
        limit = pickVisualTriangles(vertices, indices.subspan(start, 3), origin, direction, limit);
    }
    return limit;
}

/// Build 8606 scene picking of a unit or game object model (CM2Scene,
/// Wow.exe 0x70b2b0): a sphere test first, then the model's rendered
/// triangles at the current pose. See docs/unit-size-rules.md.
///
/// Broad phase (0x70a253..0x70a367): the sphere of the current animation's
/// bounds - the box's middle and the sequence's radius - or, when that radius
/// is below 9.5e-7 (0x896724) or there is no sequence, the header vertex box
/// and vertex radius.
struct ModelPickSphere {
    glm::vec3 center{0.0f};
    float radius = 0.0f;
};

inline ModelPickSphere modelPickSphere(const pipeline::M2Model& model, int sequenceIndex) {
    constexpr float kNoRadius = 9.5367431640625e-07f;  // 0x896724
    if (sequenceIndex >= 0 && static_cast<size_t>(sequenceIndex) < model.sequences.size()) {
        const auto& sequence = model.sequences[static_cast<size_t>(sequenceIndex)];
        if (sequence.boundRadius >= kNoRadius)
            return {(sequence.boundMin + sequence.boundMax) * 0.5f, sequence.boundRadius};
    }
    return {(model.vertexBoxMin + model.vertexBoxMax) * 0.5f, model.vertexRadius};
}

/// Narrow phase (0x70ae50): every batch the caller says is drawn, skinned
/// with `bones` exactly as the vertex shader does, tested triangle by
/// triangle. Returns the nearest hit parameter below `limit`, or `limit`.
/// Model-space ray; an unnormalized direction keeps world distances.
template<class BatchDrawn>
float pickSkinnedModel(const pipeline::M2Model& model, std::span<const glm::mat4> bones,
                       BatchDrawn&& batchDrawn, const glm::vec3& origin,
                       const glm::vec3& direction, float limit) {
    std::vector<glm::vec3> positions;
    positions.reserve(model.vertices.size());
    for (const auto& vertex : model.vertices) {
        if (bones.empty()) {
            positions.push_back(vertex.position);
            continue;
        }
        glm::mat4 skin(0.0f);
        for (int i = 0; i < 4; ++i) {
            const size_t bone = std::min<size_t>(vertex.boneIndices[i], bones.size() - 1);
            skin += bones[bone] * (vertex.boneWeights[i] / 255.0f);
        }
        positions.emplace_back(skin * glm::vec4(vertex.position, 1.0f));
    }
    for (const auto& batch : model.batches) {
        if (!batchDrawn(batch)) continue;
        assert(batch.indexStart + batch.indexCount <= model.indices.size());
        limit = pickVisualTriangles(positions,
            std::span<const uint16_t>(model.indices).subspan(batch.indexStart, batch.indexCount),
            origin, direction, limit);
    }
    return limit;
}

} // namespace wowee::rendering
