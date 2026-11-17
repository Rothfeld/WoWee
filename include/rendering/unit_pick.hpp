#pragma once

// The unit picking rule WoWee applies, free of the renderer so a headless
// tool (tools/pick_mask.cpp) can evaluate exactly the same rule:
//   broad: sphere of the current sequence's bounds (else the vertex box),
//   narrow: the drawn batches' triangles skinned to the current pose.
// This rule is a hypothesis under test against native cursor sweeps (#79/#90).

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <unordered_set>
#include <vector>

#include "pipeline/m2_loader.hpp"

namespace wowee::rendering {

/// M2 colour-alpha track of a batch at a sequence time; 1 without a track.
float evalBatchColorAlpha(const pipeline::M2Model& model, const pipeline::M2Batch& batch,
                          int sequenceIndex, float animationTimeMs, float globalTimeMs);
/// M2 texture-weight (transparency) track of a batch; 1 without a track.
float evalBatchTextureWeight(const pipeline::M2Model& model, const pipeline::M2Batch& batch,
                             int sequenceIndex, float animationTimeMs, float globalTimeMs);

/// Bone local transform T(pivot) T R S T(-pivot) at a sequence time.
glm::mat4 m2BoneLocalTransform(const pipeline::M2Bone& bone, int sequenceIndex, float timeMs,
                               float globalTimeMs, const std::vector<uint32_t>& globalSequenceDurations);

/// Every bone's model-space matrix for one sequence at one time (no blends,
/// arm layers or grips - a unit standing in one animation).
std::vector<glm::mat4> poseModelBones(const pipeline::M2Model& model, int sequenceIndex,
                                      float timeMs, float globalTimeMs);

/// What a pick is tested against.
struct UnitPickState {
    std::span<const glm::mat4> bones;                       // model-space, may be empty
    const std::unordered_set<uint16_t>* activeGeosets = nullptr;  // null/empty = model default
    float opacity = 1.0f;
    int sequenceIndex = -1;
    float sampleTimeMs = 0.0f;
    float globalTimeMs = 0.0f;
};

/// Whether a batch takes part in picking: drawn, not texture-unit flag 0x08,
/// in an active geoset (or, with none given, not geoset group 15/17/18), and
/// not faded out by its alpha tracks.
bool unitPickBatchDrawn(const pipeline::M2Model& model, const pipeline::M2Batch& batch,
                        const UnitPickState& state);

/// Nearest hit parameter of a model-space ray (unnormalized direction keeps
/// world distance) below `limit`, or `limit` when nothing is hit.
float pickUnitModel(const pipeline::M2Model& model, const UnitPickState& state,
                    const glm::vec3& origin, const glm::vec3& direction, float limit);

} // namespace wowee::rendering
