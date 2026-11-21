#include "rendering/unit_pick.hpp"

#include "rendering/m2_track_sampler.hpp"
#include "rendering/visual_mesh_pick.hpp"

#include <algorithm>
#include <glm/gtc/quaternion.hpp>

namespace wowee::rendering {

float evalBatchColorAlpha(const pipeline::M2Model& model, const pipeline::M2Batch& batch,
                          int sequenceIndex, float animationTimeMs, float globalTimeMs) {
    if (batch.colorIndex == 0xFFFF || batch.colorIndex >= model.colorAlphaTracks.size()) return 1.0f;
    return m2_track::sampleFloat(model.colorAlphaTracks[batch.colorIndex], sequenceIndex,
                                 animationTimeMs, globalTimeMs, model.globalSequenceDurations, 1.0f);
}

float evalBatchTextureWeight(const pipeline::M2Model& model, const pipeline::M2Batch& batch,
                             int sequenceIndex, float animationTimeMs, float globalTimeMs) {
    if (batch.transparencyIndex == 0xFFFF || batch.transparencyIndex >= model.textureWeightLookup.size())
        return 1.0f;
    const uint16_t trackIndex = model.textureWeightLookup[batch.transparencyIndex];
    if (trackIndex == 0xFFFF || trackIndex >= model.textureWeightTracks.size()) return 1.0f;
    return m2_track::sampleFloat(model.textureWeightTracks[trackIndex], sequenceIndex,
                                 animationTimeMs, globalTimeMs, model.globalSequenceDurations, 1.0f);
}

glm::mat4 m2BoneLocalTransform(const pipeline::M2Bone& bone, int sequenceIndex, float timeMs,
                               float globalTimeMs, const std::vector<uint32_t>& gsd) {
    const glm::vec3 translation = m2_track::sampleVec3(bone.translation, sequenceIndex, timeMs,
                                                       globalTimeMs, gsd, glm::vec3(0.0f));
    const glm::quat rotation = m2_track::sampleQuat(bone.rotation, sequenceIndex, timeMs, globalTimeMs, gsd);
    const glm::vec3 scale = m2_track::sampleVec3(bone.scale, sequenceIndex, timeMs, globalTimeMs, gsd,
                                                 glm::vec3(1.0f));
    const glm::mat3 r = glm::mat3_cast(rotation);
    const glm::vec3 c0 = r[0] * scale.x, c1 = r[1] * scale.y, c2 = r[2] * scale.z;
    const glm::vec3 t = (bone.pivot + translation) - (c0 * bone.pivot.x + c1 * bone.pivot.y + c2 * bone.pivot.z);
    glm::mat4 transform;
    transform[0] = glm::vec4(c0, 0.0f);
    transform[1] = glm::vec4(c1, 0.0f);
    transform[2] = glm::vec4(c2, 0.0f);
    transform[3] = glm::vec4(t, 1.0f);
    return transform;
}

std::vector<glm::mat4> poseModelBones(const pipeline::M2Model& model, int sequenceIndex,
                                      float timeMs, float globalTimeMs) {
    std::vector<glm::mat4> bones(model.bones.size());
    for (size_t i = 0; i < bones.size(); ++i) {
        const auto& bone = model.bones[i];
        const glm::mat4 local = m2BoneLocalTransform(bone, sequenceIndex, timeMs, globalTimeMs,
                                                     model.globalSequenceDurations);
        bones[i] = bone.parentBone >= 0 && static_cast<size_t>(bone.parentBone) < bones.size()
            ? bones[bone.parentBone] * local : local;
    }
    return bones;
}

bool unitPickBatchDrawn(const pipeline::M2Model& model, const pipeline::M2Batch& batch,
                        const UnitPickState& state) {
    if (batch.flags & kM2BatchFlagNonPickable) return false;
    bool filter = state.activeGeosets && !state.activeGeosets->empty();
    if (filter) {
        filter = std::any_of(model.batches.begin(), model.batches.end(), [&](const pipeline::M2Batch& b) {
            return state.activeGeosets->count(b.submeshId) != 0;
        });
    }
    if (filter) {
        if (!state.activeGeosets->count(batch.submeshId)) return false;
    } else {
        const uint16_t group = batch.submeshId / 100;
        if (group == 17 || group == 18) return false;  // drawn as CharacterRenderer draws them
    }
    const float alpha = state.opacity *
        evalBatchColorAlpha(model, batch, state.sequenceIndex, state.sampleTimeMs, state.globalTimeMs) *
        evalBatchTextureWeight(model, batch, state.sequenceIndex, state.sampleTimeMs, state.globalTimeMs);
    return alpha > 0.0f;
}

float pickUnitModel(const pipeline::M2Model& model, const UnitPickState& state,
                    const glm::vec3& origin, const glm::vec3& direction, float limit) {
    const ModelPickSphere sphere = modelPickSphere(model, state.sequenceIndex);
    const glm::vec3 oc = origin - sphere.center;
    const float a = glm::dot(direction, direction);
    const float b = glm::dot(oc, direction);
    const float c = glm::dot(oc, oc) - sphere.radius * sphere.radius;
    const float disc = b * b - a * c;
    if (!(a > 0.0f && disc >= 0.0f && (-b + std::sqrt(disc)) / a >= 0.0f)) return limit;
    return pickSkinnedModel(model, state.bones,
        [&](const pipeline::M2Batch& batch) { return unitPickBatchDrawn(model, batch, state); },
        origin, direction, limit);
}

} // namespace wowee::rendering
