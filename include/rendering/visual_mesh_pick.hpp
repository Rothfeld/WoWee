#pragma once

#include <glm/glm.hpp>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <span>
#include <cstdint>
#include <vector>

#include "pipeline/m2_loader.hpp"

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
