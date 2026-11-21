#include <cstdlib>
#include "rendering/m2_renderer.hpp"
#include "rendering/m2_renderer_internal.h"
#include "rendering/m2_model_classifier.hpp"
#include "rendering/wmo_doodad_rooms.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_buffer.hpp"
#include "rendering/vk_texture.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_utils.hpp"
#include "rendering/vk_frame_data.hpp"
#include "rendering/camera.hpp"
#include "rendering/frustum.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/blp_loader.hpp"
#include "core/logger.hpp"
#include "rendering/movement_limits.hpp"
#include "rendering/visual_mesh_pick.hpp"
#include "rendering/body_collision.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <cassert>
#include <set>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <stdexcept>

namespace wowee {
namespace rendering {

// Thread-local scratch buffers for collision queries (moved from header to
// avoid inline thread_local TLS init linker errors on Windows ARM64 / LLD).
namespace m2_internal {
thread_local std::vector<size_t> tl_m2_candidateScratch;
thread_local std::unordered_set<uint32_t> tl_m2_candidateIdScratch;
thread_local std::vector<uint32_t> tl_m2_collisionTriScratch;
} // namespace m2_internal

void M2Renderer::setInstancePosition(uint32_t instanceId, const glm::vec3& position) {
    if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
        !std::isfinite(position.z)) return;
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return;
    auto& inst = instances[idxIt->second];

    // The box the instance is filed under, which is what it has to be taken
    // out of once it has moved.
    const glm::vec3 oldBoundsMin = inst.worldBoundsMin;
    const glm::vec3 oldBoundsMax = inst.worldBoundsMax;

    inst.position = position;
    inst.updateModelMatrix();
    // Use cachedModel instead of a fresh models.find() - the pointer was set
    // at addInstance and stays valid as long as the instance exists.
    if (inst.cachedModel) {
        glm::vec3 localMin, localMax;
        getTightCollisionBounds(*inst.cachedModel, localMin, localMax);
        transformAABB(inst.modelMatrix, localMin, localMax, inst.worldBoundsMin, inst.worldBoundsMax);
        inst.recomputeCachedCullFactors();
    }

    // Incrementally update spatial grid
    refileBounds(spatialGrid, oldBoundsMin, oldBoundsMax,
                 inst.worldBoundsMin, inst.worldBoundsMax, instanceId);
}

void M2Renderer::setInstanceFade(uint32_t instanceId, float alpha) {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return;
    instances[idxIt->second].fade = std::clamp(alpha, 0.0f, 1.0f);
}

void M2Renderer::setInstanceIndoorLighting(uint32_t instanceId, const glm::vec3& color,
                                           const DirectionalLightRig* rig) {
    const auto index = instanceIndexById.find(instanceId);
    assert(index != instanceIndexById.end());
    auto& inst = instances[index->second];
    const auto rgb = glm::uvec3(glm::round(glm::clamp(color, 0.0f, 1.0f) * 255.0f));
    inst.indoorLighting = 2u | (rgb.r << 8) | (rgb.g << 16) | (rgb.b << 24);
    inst.interiorFog = true;
    inst.outdoorShadowed = false;
    inst.customLightRig = rig ? *rig : indoorDoodadRig(glm::clamp(color, 0.0f, 1.0f));
}

void M2Renderer::setInstanceOutdoorShadowed(uint32_t instanceId, bool shadowed) {
    const auto index = instanceIndexById.find(instanceId);
    if (index == instanceIndexById.end()) return;
    instances[index->second].outdoorShadowed = shadowed;
}

void M2Renderer::addInstanceWmoDoodad(uint32_t instanceId, uint32_t wmoInstanceId,
                                      const std::vector<uint16_t>& rooms) {
    const auto index = instanceIndexById.find(instanceId);
    assert(index != instanceIndexById.end());
    assert(wmoInstanceId != 0);
    M2Instance& instance = instances[index->second];
    const bool otherBuilding = instance.wmoDoodadPlaced && instance.wmoRoomInstance != 0 &&
                               instance.wmoRoomInstance != wmoInstanceId;
    WmoDoodadRooms merged = addWmoDoodad(
        WmoDoodadRooms{instance.wmoDoodadPlaced, instance.wmoRoomInstance, std::move(instance.wmoRooms)},
        wmoInstanceId, rooms);
    if (otherBuilding)
        LOG_WARNING("M2 instance ", instanceId, " is a doodad of WMO instances ", instance.wmoRoomInstance,
                    " and ", wmoInstanceId, "; drawn without rooms");
    instance.wmoDoodadPlaced = merged.placed;
    instance.wmoRoomInstance = merged.wmoInstance;
    instance.wmoRooms = std::move(merged.rooms);
}

void M2Renderer::copyInstanceRoom(uint32_t instanceId, uint32_t sourceInstanceId) {
    const auto index = instanceIndexById.find(instanceId);
    const auto source = instanceIndexById.find(sourceInstanceId);
    assert(index != instanceIndexById.end() && source != instanceIndexById.end());
    M2Instance& instance = instances[index->second];
    assert(!instance.wmoDoodadPlaced);
    instance.wmoRoomInstance = instances[source->second].wmoRoomInstance;
    instance.wmoRooms = instances[source->second].wmoRooms;
}

void M2Renderer::setInstanceHighlight(uint32_t instanceId, float amount) {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return;
    instances[idxIt->second].highlight = amount;
}

void M2Renderer::clearInstanceHighlights() {
    // A walk of every instance rather than a record of the lit one, because a
    // press can end with the instance gone - the object despawned, the tile
    // unloaded - and a remembered id would then light whatever took its slot.
    for (auto& inst : instances) inst.highlight = 0.0f;
}

void M2Renderer::setInstanceAnimationFrozen(uint32_t instanceId, bool frozen) {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return;
    auto& inst = instances[idxIt->second];
    inst.animSpeed = frozen ? 0.0f : 1.0f;
    inst.holdAtEnd = false;
    if (frozen) {
        inst.animTime = 0.0f;  // Reset to bind pose
    }
}

void M2Renderer::setInstanceAnimationProgress(uint32_t instanceId, float fraction) {
    auto& inst = instances.at(instanceIndexById.at(instanceId));
    assert(std::isfinite(fraction));
    inst.animSpeed = 0.0f;
    inst.holdAtEnd = false;
    // Just short of the end: reaching it wraps a looping sequence to its start.
    inst.animTime = std::max(0.0f, std::clamp(fraction, 0.0f, 1.0f) * inst.animDuration - 0.5f);
}

void M2Renderer::restartInstanceAnimation(uint32_t instanceId) {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return;
    auto& inst = instances[idxIt->second];
    inst.animTime = 0.0f;
    inst.emitterPrevSequence = -1;
    inst.emitterPrevAnimTime = 0.0f;
    if (inst.cachedModel) computeBoneMatrices(*inst.cachedModel, inst, &cachedCamPos_);
}

void M2Renderer::startInstanceStateEffect(uint32_t instanceId) {
    auto& instance = instances.at(instanceIndexById.at(instanceId));
    assert(instance.cachedModel);
    startStateEffect(*instance.cachedModel, instance);
    instance.emitterPrevSequence = -1;
    instance.emitterPrevAnimTime = 0.0f;
    computeBoneMatrices(*instance.cachedModel, instance, &cachedCamPos_);
}

float M2Renderer::finishInstanceStateEffect(uint32_t instanceId) {
    if (!hasAnimation(instanceId, 159)) return 0;
    setInstanceAnimationHeld(instanceId, 159, false);
    return getInstanceAnimDuration(instanceId) * 0.001f;
}

std::optional<uint32_t> M2Renderer::soleSequenceId(uint32_t instanceId) const {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return std::nullopt;
    const auto& inst = instances[idxIt->second];
    if (!inst.cachedModel || inst.cachedModel->sequences.size() != 1) return std::nullopt;
    return inst.cachedModel->sequences[0].id;
}

std::optional<uint32_t> M2Renderer::standSequenceId(uint32_t instanceId) const {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return std::nullopt;
    const auto& inst = instances[idxIt->second];
    if (!inst.cachedModel || inst.cachedModel->sequences.empty()) return std::nullopt;
    return inst.cachedModel->sequences[inst.cachedModel->standSequenceIndex].id;
}

void M2Renderer::setInstanceStateAnimation(uint32_t instanceId, uint32_t transitionId,
                                           uint32_t stateId, bool playTransition) {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return;
    auto& inst = instances[idxIt->second];
    if (!inst.cachedModel) return;
    const auto& seqs = inst.cachedModel->sequences;
    int state = -1, transition = -1;
    for (int i = 0; i < static_cast<int>(seqs.size()); ++i) {
        if (state < 0 && seqs[i].id == stateId) state = i;
        if (transition < 0 && seqs[i].id == transitionId) transition = i;
    }
    assert(state >= 0);
    // The loop is the instance's idle: playingVariation returns to it.
    inst.idleSequenceIndex = state;
    inst.holdAtEnd = false;
    inst.animSpeed = 1.0f;
    inst.animTime = 0.0f;
    const int first = playTransition && transition >= 0 ? transition : state;
    inst.currentSequenceIndex = first;
    inst.animDuration = static_cast<float>(seqs[first].duration);
    inst.playingVariation = first != state;
}

void M2Renderer::playInstanceStand(uint32_t instanceId) {
    auto& inst = instances.at(instanceIndexById.at(instanceId));
    assert(inst.cachedModel && !inst.cachedModel->sequences.empty());
    const int stand = inst.cachedModel->standSequenceIndex;
    inst.currentSequenceIndex = stand;
    inst.idleSequenceIndex = stand;
    inst.animDuration = static_cast<float>(inst.cachedModel->sequences[stand].duration);
    inst.animTime = 0.0f;
    inst.animSpeed = 1.0f;
    inst.holdAtEnd = false;
    inst.playingVariation = false;
}

void M2Renderer::setInstanceAnimationHeld(uint32_t instanceId, uint32_t animationId,
                                          bool skipToEnd) {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return;
    auto& inst = instances[idxIt->second];
    if (!inst.cachedModel) return;
    const auto& seqs = inst.cachedModel->sequences;
    for (int i = 0; i < static_cast<int>(seqs.size()); ++i) {
        if (seqs[i].id != animationId) continue;
        inst.currentSequenceIndex = i;
        inst.animDuration = static_cast<float>(seqs[i].duration);
        inst.playingVariation = false;   // not a variation: it does not go back
        inst.holdAtEnd = true;
        if (skipToEnd) {
            inst.animTime = inst.animDuration;
            inst.animSpeed = 0.0f;
        } else {
            inst.animTime = 0.0f;
            inst.animSpeed = 1.0f;
        }
        return;
    }
}

void M2Renderer::setInstanceAnimation(uint32_t instanceId, uint32_t animationId, bool loop) {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return;
    auto& inst = instances[idxIt->second];
    if (!inst.cachedModel) return;
    const auto& seqs = inst.cachedModel->sequences;
    // Find the first sequence matching the requested animation ID
    for (int i = 0; i < static_cast<int>(seqs.size()); ++i) {
        if (seqs[i].id == animationId) {
            inst.currentSequenceIndex = i;
            inst.animDuration = static_cast<float>(seqs[i].duration);
            inst.animTime = 0.0f;
            inst.animSpeed = 1.0f;
            // Use playingVariation=true for one-shot (returns to idle when done)
            inst.playingVariation = !loop;
            inst.holdAtEnd = false;
            return;
        }
    }
}

bool M2Renderer::hasAnimation(uint32_t instanceId, uint32_t animationId) const {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return false;
    const auto& inst = instances[idxIt->second];
    if (!inst.cachedModel) return false;
    for (const auto& seq : inst.cachedModel->sequences) {
        if (seq.id == animationId) return true;
    }
    return false;
}

float M2Renderer::getInstanceAnimDuration(uint32_t instanceId) const {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return 0.0f;
    const auto& inst = instances[idxIt->second];
    if (!inst.cachedModel) return 0.0f;
    const auto& seqs = inst.cachedModel->sequences;
    if (seqs.empty()) return 0.0f;
    int seqIdx = inst.currentSequenceIndex;
    if (seqIdx < 0 || seqIdx >= static_cast<int>(seqs.size())) seqIdx = 0;
    return seqs[seqIdx].duration; // in milliseconds
}

bool M2Renderer::getInstancePosition(uint32_t instanceId, glm::vec3& outPosition) const {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return false;
    outPosition = instances[idxIt->second].position;
    return true;
}

bool M2Renderer::getInstanceCollisionBoxHeight(uint32_t instanceId, float& outHeight) const {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return false;
    const auto& inst = instances[idxIt->second];
    const auto& model = *inst.cachedModel;
    outHeight = (model.headerBoxMax.z - model.headerBoxMin.z) * glm::length(glm::vec3(inst.modelMatrix[2]));
    return true;
}

bool M2Renderer::getInstanceBounds(uint32_t instanceId, glm::vec3& outCenter, float& outRadius) const {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return false;
    const auto& inst = instances[idxIt->second];
    const auto& model = *inst.cachedModel;
    outCenter = glm::vec3(inst.modelMatrix * glm::vec4((model.boundMin + model.boundMax) * 0.5f, 1));
    // Rotating a world-axis-aligned box enlarges its diagonal. Cursor bounds
    // use the local model sphere, whose radius must not change with heading.
    const float scale = std::max({glm::length(glm::vec3(inst.modelMatrix[0])),
                                  glm::length(glm::vec3(inst.modelMatrix[1])),
                                  glm::length(glm::vec3(inst.modelMatrix[2]))});
    outRadius = glm::length(model.boundMax - model.boundMin) * 0.5f * scale;
    return outRadius > 0.0f;
}

void M2Renderer::setInstanceTransform(uint32_t instanceId, const glm::mat4& transform) {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return;
    // Reject NaN matrix - would propagate into the model matrix uniform
    // and the spatial-grid bounds, leaving stale grid cells pointing at
    // a NaN-bounded instance.
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            if (!std::isfinite(transform[c][r])) return;
    auto& inst = instances[idxIt->second];

    // Remove old grid cells before updating bounds
    const glm::vec3 oldBoundsMin = inst.worldBoundsMin;
    const glm::vec3 oldBoundsMax = inst.worldBoundsMax;
    const glm::vec3 oldPosition = inst.position;

    // Update model matrix directly
    inst.modelMatrix = transform;
    inst.invModelMatrix = glm::inverse(transform);

    // Extract position and uniform scale from transform for bounds and collision
    inst.position = glm::vec3(transform[3]);
    inst.scale = glm::length(glm::vec3(transform[0]));

    // The dedup map is keyed on position, so it has to move with the instance.
    // It did not, and only rebuildSpatialIndex ever put it right - which this
    // path deliberately avoids. A ship's doodads are created at the origin and
    // moved here a frame later, so the origin key stayed pointing at them and
    // the next ship of the same class was handed the first ship's sails instead
    // of its own. Both hulls then wrote their transform to the one instance, so
    // it rendered at whichever ship updated last, and whichever hull unloaded
    // first destroyed it for the other.
    if (!inst.cachedIsGroundDetail) {
        auto keyFor = [&](const glm::vec3& p) {
            return DedupKey{.modelId = inst.modelId,
                            .qx = static_cast<int32_t>(std::round(p.x * 10.0f)),
                            .qy = static_cast<int32_t>(std::round(p.y * 10.0f)),
                            .qz = static_cast<int32_t>(std::round(p.z * 10.0f))};
        };
        const DedupKey oldKey = keyFor(oldPosition);
        const DedupKey newKey = keyFor(inst.position);
        if (!(oldKey == newKey)) {
            auto oldIt = instanceDedupMap_.find(oldKey);
            // Only drop the entry if it still names this instance.
            if (oldIt != instanceDedupMap_.end() && oldIt->second == instanceId) {
                instanceDedupMap_.erase(oldIt);
            }
            instanceDedupMap_.emplace(newKey, instanceId);
        }
    }

    // Update bounds via the cached model pointer
    if (inst.cachedModel) {
        glm::vec3 localMin, localMax;
        getTightCollisionBounds(*inst.cachedModel, localMin, localMax);
        transformAABB(inst.modelMatrix, localMin, localMax, inst.worldBoundsMin, inst.worldBoundsMax);
        inst.recomputeCachedCullFactors();
    }

    // Incrementally update spatial grid (remove old cells, add new cells)
    refileBounds(spatialGrid, oldBoundsMin, oldBoundsMax,
                 inst.worldBoundsMin, inst.worldBoundsMax, instanceId);
    // No spatialIndexDirty_ = true - handled incrementally
}

void M2Renderer::retireInstance(uint32_t instanceId) {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return;
    auto& inst = instances[idxIt->second];
    if (inst.particles.empty()) {
        removeInstance(instanceId);
        return;
    }
    inst.retiring = true;
    inst.ribbonEdges.clear();
    inst.ribbonEdgeAccumulators.clear();
}

void M2Renderer::retireMissileInstance(uint32_t instanceId) {
    auto& inst = instances.at(instanceIndexById.at(instanceId));
    // Ribbons age in the particle pass, which only visits models with emitters.
    if (inst.cachedHasParticleEmitters && inst.cachedModel && !inst.cachedModel->ribbonEmitters.empty())
        updateRibbons(inst, *inst.cachedModel, 0.0f, /*emit=*/false, /*finalEdge=*/true);
    const bool trail = inst.cachedHasParticleEmitters &&
        std::any_of(inst.ribbonEdges.begin(), inst.ribbonEdges.end(),
                    [](const auto& edges) { return edges.size() >= 2; });
    if (inst.particles.empty() && !trail) {
        removeInstance(instanceId);
        return;
    }
    inst.retiring = true;
    inst.ribbonsLiveOut = true;
}

void M2Renderer::removeInstance(uint32_t instanceId) {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return;
    size_t idx = idxIt->second;
    if (idx >= instances.size()) return;

    auto& inst = instances[idx];

    // Remove from spatial grid incrementally (same pattern as the move-update path)
    eraseBounds(spatialGrid, inst.worldBoundsMin, inst.worldBoundsMax, instanceId);

    // Remove from dedup map
    if (!inst.cachedIsGroundDetail) {
        DedupKey dk{.modelId = inst.modelId,
                    .qx = static_cast<int32_t>(std::round(inst.position.x * 10.0f)),
                    .qy = static_cast<int32_t>(std::round(inst.position.y * 10.0f)),
                    .qz = static_cast<int32_t>(std::round(inst.position.z * 10.0f))};
        instanceDedupMap_.erase(dk);
    }

    destroyInstanceBones(inst, /*defer=*/true);

    // Swap-remove: move last element to the hole and pop_back to avoid O(n) shift
    instanceIndexById.erase(instanceId);
    if (idx < instances.size() - 1) {
        uint32_t movedId = instances.back().id;
        instances[idx] = std::move(instances.back());
        instances.pop_back();
        instanceIndexById[movedId] = idx;
    } else {
        instances.pop_back();
    }

    // Rebuild the lightweight auxiliary index vectors (portal, particles, etc.)
    // These are small vectors of indices that are rebuilt cheaply.
    animatedInstanceIndices_.clear();
    particleOnlyInstanceIndices_.clear();
    particleInstanceIndices_.clear();
    for (size_t i = 0; i < instances.size(); i++) {
        auto& ri = instances[i];
        if (ri.externalPose) {
            if (ri.cachedModel && (!ri.cachedModel->particleEmitters.empty() ||
                                   !ri.cachedModel->ribbonEmitters.empty()))
                particleInstanceIndices_.push_back(i);
            continue;
        }
        if (ri.cachedHasEmitters) particleInstanceIndices_.push_back(i);
        if (ri.cachedHasAnimation && !ri.cachedDisableAnimation)
            animatedInstanceIndices_.push_back(i);
        else if (ri.cachedHasEmitters)
            particleOnlyInstanceIndices_.push_back(i);
    }
}

void M2Renderer::setInstanceIsGameObject(uint32_t instanceId, bool isGameObject) {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end() || idxIt->second >= instances.size()) return;
    instances[idxIt->second].isGameObject = isGameObject;
}

void M2Renderer::setInstanceIsSpellVisual(uint32_t instanceId) {
    auto idxIt = instanceIndexById.find(instanceId);
    assert(idxIt != instanceIndexById.end());
    instances[idxIt->second].isSpellVisual = true;
}

void M2Renderer::setSkipCollision(uint32_t instanceId, bool skip) {
    for (auto& inst : instances) {
        if (inst.id == instanceId) {
            inst.skipCollision = skip;
            return;
        }
    }
}

void M2Renderer::removeInstances(const std::vector<uint32_t>& instanceIds) {
    if (instanceIds.empty() || instances.empty()) {
        return;
    }

    std::unordered_set<uint32_t> toRemove(instanceIds.begin(), instanceIds.end());
    const size_t oldSize = instances.size();
    for (auto& inst : instances) {
        if (toRemove.count(inst.id)) {
            destroyInstanceBones(inst, /*defer=*/true);
        }
    }
    instances.erase(std::remove_if(instances.begin(), instances.end(),
                   [&toRemove](const M2Instance& inst) {
                       return toRemove.find(inst.id) != toRemove.end();
                   }),
                   instances.end());

    if (instances.size() != oldSize) {
        rebuildSpatialIndex();
    }
}

void M2Renderer::clear() {
    if (vkCtx_) {
        vkDeviceWaitIdle(vkCtx_->getDevice());
        for (auto& [id, model] : models) {
            destroyModelGPU(model);
        }
        for (auto& inst : instances) {
            destroyInstanceBones(inst);
        }
        // Reset descriptor pools so new allocations succeed after reload.
        // destroyModelGPU/destroyInstanceBones don't free individual sets,
        // so the pools fill up across map changes without this reset.
        VkDevice device = vkCtx_->getDevice();
        if (materialDescPool_) {
            vkResetDescriptorPool(device, materialDescPool_, 0);
            chainTexSets_.clear();
            // Re-allocate the glow texture descriptor set (pre-allocated during init,
            // invalidated by pool reset).
            // Cleared before the test, not inside it: the pool reset above
            // invalidated whatever was here, so a run that does not re-allocate
            // was leaving a dangling set behind to be bound.
            glowTexDescSet_ = VK_NULL_HANDLE;
            // Valid, not merely present. A texture whose upload or view
            // creation failed writes a null view into this set, and the glow
            // pass samples it - the same shape as the particle and ribbon sets.
            if (glowTexture_ && glowTexture_->isValid() && particleTexLayout_) {
                VkDescriptorSetAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                ai.descriptorPool = materialDescPool_;
                ai.descriptorSetCount = 1;
                ai.pSetLayouts = &particleTexLayout_;
                if (vkAllocateDescriptorSets(device, &ai, &glowTexDescSet_) == VK_SUCCESS) {
                    VkDescriptorImageInfo imgInfo = glowTexture_->descriptorInfo();
                    VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                    write.dstSet = glowTexDescSet_;
                    write.dstBinding = 0;
                    write.descriptorCount = 1;
                    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                    write.pImageInfo = &imgInfo;
                    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
                }
            }
        }
        if (boneDescPool_) {
            if (boneDescPoolGeneration_) boneDescPoolGeneration_->fetch_add(1, std::memory_order_relaxed);
            vkResetDescriptorPool(device, boneDescPool_, 0);
            // Re-allocate the dummy bone set (invalidated by pool reset)
            dummyBoneSet_ = allocateBoneSet();
            if (dummyBoneSet_ && dummyBoneBuffer_) {
                VkDescriptorBufferInfo bufInfo{};
                bufInfo.buffer = dummyBoneBuffer_;
                bufInfo.offset = 0;
                bufInfo.range = sizeof(glm::mat4);
                VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = dummyBoneSet_;
                write.dstBinding = 0;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                write.pBufferInfo = &bufInfo;
                vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
            }
            // Re-allocate mega bone sets (invalidated by pool reset)
            for (int i = 0; i < 2; i++) {
                megaBoneSet_[i] = allocateBoneSet();
                if (megaBoneSet_[i] && megaBoneBuffer_[i]) {
                    VkDescriptorBufferInfo mbInfo{};
                    mbInfo.buffer = megaBoneBuffer_[i];
                    mbInfo.offset = 0;
                    mbInfo.range = VkDeviceSize(MEGA_BONE_MATRIX_CAPACITY) * sizeof(glm::mat4);
                    VkWriteDescriptorSet mw{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                    mw.dstSet = megaBoneSet_[i];
                    mw.dstBinding = 0;
                    mw.descriptorCount = 1;
                    mw.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                    mw.pBufferInfo = &mbInfo;
                    vkUpdateDescriptorSets(device, 1, &mw, 0, nullptr);
                }
            }
        }
    }
    models.clear();
    pinnedModelIds_.clear();
    instances.clear();
    spatialGrid.clear();
    instanceIndexById.clear();
    boneSeedInstanceByModel_.clear();
    instanceDedupMap_.clear();
    animatedInstanceIndices_.clear();
    particleOnlyInstanceIndices_.clear();
    particleInstanceIndices_.clear();

    // Clear texture cache so stale textures don't block loads for the next
    // character/map.  Without this, the old session's textures fill the cache
    // budget and failedTextureRetryAt_ blocks legitimate reloads, causing an
    // infinite model-load loop on character switch.
    textureCache.clear();
    texturePropsByPtr_.clear();
    textureCacheBytes_ = 0;
    textureCacheCounter_ = 0;
    failedTextureCache_.clear();
    failedTextureRetryAt_.clear();
    loggedTextureLoadFails_.clear();
    textureLookupSerial_ = 0;
    textureBudgetRejectWarnings_ = 0;
}

void M2Renderer::clearInstances() {
    if (vkCtx_) vkDeviceWaitIdle(vkCtx_->getDevice());
    for (auto& inst : instances) destroyInstanceBones(inst);
    instances.clear();
    spatialGrid.clear();
    instanceIndexById.clear();
    boneSeedInstanceByModel_.clear();
    instanceDedupMap_.clear();
    animatedInstanceIndices_.clear();
    particleOnlyInstanceIndices_.clear();
    particleInstanceIndices_.clear();
}

void M2Renderer::setCollisionFocus(const glm::vec3& worldPos, float radius) {
    collisionFocus.set(worldPos, radius);
}

void M2Renderer::resetQueryStats() {
    queryTimeMs = 0.0;
    queryCallCount = 0;
}

void M2Renderer::rebuildSpatialIndex() {
    spatialGrid.clear();
    instanceIndexById.clear();
    boneSeedInstanceByModel_.clear();
    instanceDedupMap_.clear();
    instanceIndexById.reserve(instances.size());
    animatedInstanceIndices_.clear();
    particleOnlyInstanceIndices_.clear();
    particleInstanceIndices_.clear();

    for (size_t i = 0; i < instances.size(); i++) {
        auto& inst = instances[i];
        instanceIndexById[inst.id] = i;

        // Re-cache model pointer (may have changed after model map modifications)
        auto mdlIt = models.find(inst.modelId);
        inst.cachedModel = (mdlIt != models.end()) ? &mdlIt->second : nullptr;

        // Rebuild dedup map (skip ground detail)
        if (!inst.cachedIsGroundDetail) {
            DedupKey dk{.modelId = inst.modelId,
                        .qx = static_cast<int32_t>(std::round(inst.position.x * 10.0f)),
                        .qy = static_cast<int32_t>(std::round(inst.position.y * 10.0f)),
                        .qz = static_cast<int32_t>(std::round(inst.position.z * 10.0f))};
            instanceDedupMap_[dk] = inst.id;
        }

        if (inst.externalPose) {
            if (inst.cachedModel && (!inst.cachedModel->particleEmitters.empty() ||
                                     !inst.cachedModel->ribbonEmitters.empty()))
                particleInstanceIndices_.push_back(i);
        } else {
            if (inst.cachedHasEmitters) {
                particleInstanceIndices_.push_back(i);
            }
            if (inst.cachedHasAnimation && !inst.cachedDisableAnimation) {
                animatedInstanceIndices_.push_back(i);
            } else if (inst.cachedHasEmitters) {
                particleOnlyInstanceIndices_.push_back(i);
            }
        }

        insertBounds(spatialGrid, inst.worldBoundsMin, inst.worldBoundsMax, inst.id);
    }
    spatialIndexDirty_ = false;
}

void M2Renderer::gatherCandidates(const glm::vec3& queryMin, const glm::vec3& queryMax,
                                  std::vector<size_t>& outIndices) const {
    outIndices.clear();

    gatherIds(spatialGrid, queryMin, queryMax, tl_m2_candidateIdScratch,
              [&](uint32_t id) {
                  auto idxIt = instanceIndexById.find(id);
                  if (idxIt != instanceIndexById.end()) {
                      outIndices.push_back(idxIt->second);
                  }
              });

    // Safety fallback to preserve collision correctness if the spatial index
    // misses candidates (e.g. during streaming churn).
    if (outIndices.empty() && !instances.empty()) {
        for (size_t i = 0; i < instances.size(); i++) {
            const auto& inst = instances[i];
            if (queryMax.x >= inst.worldBoundsMin.x && queryMin.x <= inst.worldBoundsMax.x &&
                queryMax.y >= inst.worldBoundsMin.y && queryMin.y <= inst.worldBoundsMax.y &&
                queryMax.z >= inst.worldBoundsMin.z && queryMin.z <= inst.worldBoundsMax.z) {
                outIndices.push_back(i);
            }
        }
    }
}

void M2Renderer::setModelPinned(uint32_t modelId, bool pinned) {
    if (pinned) {
        pinnedModelIds_.insert(modelId);
        modelUnusedSince_.erase(modelId);
    } else {
        pinnedModelIds_.erase(modelId);
    }
}

void M2Renderer::cleanupUnusedModels() {
    // Build set of model IDs that are still referenced by instances
    std::unordered_set<uint32_t> usedModelIds;
    for (const auto& instance : instances) {
        usedModelIds.insert(instance.modelId);
    }

    const auto now = std::chrono::steady_clock::now();
    constexpr auto kGracePeriod = std::chrono::seconds(60);

    // Find models with no instances that have exceeded the grace period.
    // Models that just lost their last instance get tracked but not evicted
    // immediately - this prevents thrashing when GO models are briefly
    // instance-free between despawn and respawn cycles.
    std::vector<uint32_t> toRemove;
    for (const auto& [id, model] : models) {
        if (usedModelIds.find(id) != usedModelIds.end() ||
            pinnedModelIds_.find(id) != pinnedModelIds_.end()) {
            // Model still in use or pinned - clear any pending unused timestamp
            modelUnusedSince_.erase(id);
            continue;
        }
        auto unusedIt = modelUnusedSince_.find(id);
        if (unusedIt == modelUnusedSince_.end()) {
            // First cycle with no instances - start the grace timer
            modelUnusedSince_[id] = now;
        } else if (now - unusedIt->second >= kGracePeriod) {
            // Grace period expired - mark for removal
            toRemove.push_back(id);
            modelUnusedSince_.erase(unusedIt);
        }
    }

    // Delete GPU resources and remove from map.
    // Wait for the GPU to finish all in-flight frames before destroying any
    // buffers - the previous frame's command buffer may still be referencing
    // vertex/index buffers that are about to be freed. Without this wait,
    // the GPU reads freed memory, which can cause VK_ERROR_DEVICE_LOST.
    // Suspected (not yet confirmed) contributor to multi-second freezes seen
    // right after taxi landings - timed here so the next repro pins it down.
    if (!toRemove.empty() && vkCtx_) {
        const auto waitStart = std::chrono::steady_clock::now();
        vkDeviceWaitIdle(vkCtx_->getDevice());
        const float waitMs = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - waitStart).count();
        LOG_DEBUG("M2 cleanup: vkDeviceWaitIdle took ", waitMs, "ms (", toRemove.size(), " models to remove)");
    }
    for (uint32_t id : toRemove) {
        auto it = models.find(id);
        if (it != models.end()) {
            destroyModelGPU(it->second);
            models.erase(it);
            // Record the eviction so owners caching "uploaded" model IDs can
            // drop it; otherwise their next spawn skips the load as a stale hit
            // and pushes an empty placeholder (missing doodads on revisit).
            reapedModelIds_.push_back(id);
        }
    }

    if (!toRemove.empty()) {
        LOG_INFO("M2 cleanup: removed ", toRemove.size(), " unused models, ", models.size(), " remaining");
    }
}

std::vector<uint32_t> M2Renderer::drainReapedModelIds() {
    std::vector<uint32_t> out;
    out.swap(reapedModelIds_);
    return out;
}

void M2Renderer::unloadModel(uint32_t modelId) {
    auto it = models.find(modelId);
    if (it == models.end()) return;
    if (vkCtx_) vkDeviceWaitIdle(vkCtx_->getDevice());
    destroyModelGPU(it->second);
    models.erase(it);
    modelUnusedSince_.erase(modelId);
    pinnedModelIds_.erase(modelId);
    reapedModelIds_.push_back(modelId);
}

size_t M2Renderer::evictUnreferencedTextures(size_t bytesNeeded) {
    if (textureCache.empty() || bytesNeeded == 0) return 0;

    // Free a slab rather than exactly what was asked for.
    //
    // Working out what is evictable means walking every loaded model, because
    // the in-use set is what those models point at. Doing that per texture is
    // a walk per load for as long as the cache sits at its ceiling - which is
    // exactly when textures are streaming in fastest. One sweep that frees a
    // slab serves the next few hundred loads instead.
    constexpr size_t kEvictionSlab = 64ull * 1024 * 1024;
    bytesNeeded = std::max(bytesNeeded, kEvictionSlab);

    // What a loaded model still points at. A batch, a particle emitter and a
    // ribbon each keep a raw pointer into this cache, and the per-frame
    // particle and ribbon groups are rebuilt from those - so anything a model
    // refers to is off limits however old it is.
    std::unordered_set<const VkTexture*> inUse;
    inUse.reserve(textureCache.size());
    for (const auto& [modelId, model] : models) {
        for (const auto& batch : model.batches) {
            if (batch.texture) inUse.insert(batch.texture);
        }
        for (const VkTexture* tex : model.particleTextures) {
            if (tex) inUse.insert(tex);
        }
        for (const VkTexture* tex : model.ribbonTextures) {
            if (tex) inUse.insert(tex);
        }
    }

    std::vector<std::pair<uint64_t, std::string>> candidates;
    candidates.reserve(textureCache.size());
    for (const auto& [key, entry] : textureCache) {
        if (!entry.texture || inUse.count(entry.texture.get()) != 0) continue;
        candidates.emplace_back(entry.lastUse, key);
    }
    if (candidates.empty()) return 0;
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    size_t freed = 0;
    size_t dropped = 0;
    for (const auto& [lastUse, key] : candidates) {
        if (freed >= bytesNeeded) break;
        auto it = textureCache.find(key);
        if (it == textureCache.end()) continue;
        freed += it->second.approxBytes;
        textureCacheBytes_ -= std::min(textureCacheBytes_, it->second.approxBytes);
        texturePropsByPtr_.erase(it->second.texture.get());
        // A command buffer submitted a frame or two ago may still be reading
        // it, so the texture outlives this call and dies once every frame slot
        // has fenced. std::function needs a copyable capture, hence the shared
        // pointer around what was a unique one.
        auto doomed = std::shared_ptr<VkTexture>(it->second.texture.release());
        vkCtx_->deferAfterAllFrameFences([doomed]() mutable { doomed.reset(); });
        textureCache.erase(it);
        ++dropped;
    }

    if (freed > 0) {
        // Whatever was refused while the cache was full can be asked for again
        // now. Only those: a texture that failed because its file is missing
        // is not worth retrying every time something else is evicted.
        for (const auto& key : budgetRejected_) {
            failedTextureCache_.erase(key);
            failedTextureRetryAt_.erase(key);
        }
        budgetRejected_.clear();
        LOG_INFO("M2 texture cache: evicted ", dropped, " unreferenced texture(s), freed ",
                 freed / (1024 * 1024), " MB (now ", textureCacheBytes_ / (1024 * 1024),
                 " MB / ", textureCacheBudgetBytes_ / (1024 * 1024), " MB)");
    }
    return freed;
}

VkTexture* M2Renderer::loadTexture(const std::string& path, uint32_t texFlags) {
    constexpr uint64_t kFailedTextureRetryLookups = 512;
    auto normalizeKey = [](std::string key) {
        std::replace(key.begin(), key.end(), '/', '\\');
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return key;
    };
    std::string key = normalizeKey(path);
    // A cached VkTexture owns both image and sampler. Models sharing a BLP
    // can request different WrapS/WrapT modes (barrel01 vs barrellowpoly).
    // Keep asset lookup keyed by path, but GPU entries by path and sampling.
    const std::string cacheKey = key + "|wrap=" + std::to_string(texFlags & 0x3u);
    const uint64_t lookupSerial = ++textureLookupSerial_;

    // Check cache
    auto it = textureCache.find(cacheKey);
    if (it != textureCache.end()) {
        it->second.lastUse = ++textureCacheCounter_;
        return it->second.texture.get();
    }
    auto failIt = failedTextureRetryAt_.find(key);
    if (failIt != failedTextureRetryAt_.end() && lookupSerial < failIt->second) {
        return whiteTexture_.get();
    }

    // Check pre-decoded BLP cache first (populated by background worker threads)
    pipeline::BLPImage blp;
    if (predecodedBLPCache_) {
        auto pit = predecodedBLPCache_->find(key);
        if (pit != predecodedBLPCache_->end()) {
            blp = std::move(pit->second);
            predecodedBLPCache_->erase(pit);
        }
    }
    if (!blp.isValid()) {
        // M2 skins are sampled and their transparency is read from the
        // blocks, so they need no decode.
        blp = assetManager->loadTexture(key, true);
    }
    if (!blp.isValid()) {
        // Cache misses briefly to avoid repeated expensive MPQ/disk probes.
        failedTextureCache_.insert(key);
        failedTextureRetryAt_[key] = lookupSerial + kFailedTextureRetryLookups;
        if (loggedTextureLoadFails_.insert(key).second) {
            LOG_WARNING("M2: Failed to load texture: ", path);
        }
        return whiteTexture_.get();
    }

    const size_t approxBytes = blp.approxUploadBytes();
    if (textureCacheBytes_ + approxBytes > textureCacheBudgetBytes_) {
        // Make room before giving up. lastUse has been recorded on every hit
        // and every insert since this cache was written and nothing ever read
        // it: over budget, the newest texture the player looked at was refused
        // and handed back the white one, permanently, while whatever filled
        // the cache first kept its place. A tree whose leaf sheet was refused
        // renders nothing in the main pass and casts the shadow of a solid
        // quad, and the retry timer below is what made that come and go.
        evictUnreferencedTextures(approxBytes);
    }
    if (textureCacheBytes_ + approxBytes > textureCacheBudgetBytes_) {
        static constexpr size_t kMaxFailedTextureCache = 200000;
        budgetRejected_.insert(key);
        if (failedTextureCache_.size() < kMaxFailedTextureCache) {
            // Cache budget-rejected keys too; without this we repeatedly decode/load
            // the same textures every frame once budget is saturated.
            failedTextureCache_.insert(key);
            failedTextureRetryAt_[key] = lookupSerial + kFailedTextureRetryLookups;
        }
        if (textureBudgetRejectWarnings_ < 3) {
            LOG_WARNING("M2 texture cache full (", textureCacheBytes_ / (1024 * 1024),
                        " MB / ", textureCacheBudgetBytes_ / (1024 * 1024),
                        " MB), rejecting texture: ", path);
        }
        ++textureBudgetRejectWarnings_;
        return whiteTexture_.get();
    }

    // Whether the texture actually uses alpha. Reads the DXT blocks when the
    // loader kept them, and every fourth decoded byte when it did not; the two
    // agree, which tests/test_blp_alpha_scan.cpp checks against the assets.
    // This was the only thing forcing M2 textures to be decoded.
    const bool hasAlpha = blp.hasTransparency();

    // And whether that alpha is a silhouette or an atlas leftover, which is
    // what an opaque batch has to know before anything keys on it.
    const bool alphaIsSilhouette = blp.alphaIsSilhouette();

    // Create Vulkan texture
    auto tex = std::make_unique<VkTexture>();
    tex->uploadBLP(*vkCtx_, blp);

    // M2Texture flags: bit 0 = WrapS (1=repeat, 0=clamp), bit 1 = WrapT
    VkSamplerAddressMode wrapS = (texFlags & 0x1) ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSamplerAddressMode wrapT = (texFlags & 0x2) ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    // WOWEE_SKY_MIP_BIAS pushes the sky's textures toward smaller mips.
    //
    // Every per-frame quantity behind the sky has now been measured and holds
    // still while it flickers: the lighting, the clock, the culling, the frame
    // time, and every one of the thirty-four batches' material flags. Nothing
    // is alpha tested, colour keyed, or drawn as a glow card. What is left is
    // the texture sample, and the sky is the one model where that is loud:
    // eighteen of its layers are additive, so each layer's sampling noise adds
    // to the last rather than replacing it, and the dome is at its most
    // foreshortened exactly where a turn sweeps fastest.
    //
    // A knob rather than a fixed value, because if this is the cause the right
    // amount is a thing to measure and not to guess.
    static const float skyMipBias = [] {
        const char* set = std::getenv("WOWEE_SKY_MIP_BIAS");
        return set ? std::strtof(set, nullptr) : 0.0f;
    }();
    tex->createSampler(vkCtx_->getDevice(), VK_FILTER_LINEAR, wrapS, wrapT,
                       16.0f, skyMode_ ? skyMipBias : 0.0f);

    VkTexture* texPtr = tex.get();

    TextureCacheEntry e;
    e.texture = std::move(tex);
    e.approxBytes = approxBytes;
    e.lastUse = ++textureCacheCounter_;
    textureCacheBytes_ += e.approxBytes;
    textureCache[cacheKey] = std::move(e);
    failedTextureCache_.erase(key);
    failedTextureRetryAt_.erase(key);
    texturePropsByPtr_[texPtr] = {.hasAlpha = hasAlpha,
                                  .alphaIsSilhouette = alphaIsSilhouette};
    core::Logger::getInstance().debug("M2: Loaded texture: ", path, " (", blp.width, "x", blp.height,
                                     ") wrap=", texFlags & 0x3u);

    return texPtr;
}

uint32_t M2Renderer::getTotalTriangleCount() const {
    uint32_t total = 0;
    for (const auto& instance : instances) {
        if (instance.cachedModel) {
            total += instance.cachedModel->indexCount / 3;
        }
    }
    return total;
}

void M2Renderer::debugDumpFloorCandidatesAt(float glX, float glY, float glZ) const {
    LOG_WARNING("=== M2 Floor Debug at render(", glX, ", ", glY, ", ", glZ, ") ===");
    glm::vec3 queryMin(glX - 2.0f, glY - 2.0f, glZ - 6.0f);
    glm::vec3 queryMax(glX + 2.0f, glY + 2.0f, glZ + 8.0f);
    gatherCandidates(queryMin, queryMax, tl_m2_candidateScratch);
    int reported = 0;
    for (size_t idx : tl_m2_candidateScratch) {
        const auto& instance = instances[idx];
        const char* rejected = nullptr;
        if (!instance.cachedModel) {
            rejected = "no model";
        } else if (instance.scale <= 0.001f) {
            rejected = "zero scale";
        } else if (instance.skipCollision) {
            rejected = "skipCollision";
        }
        const M2ModelGPU* model = instance.cachedModel;
        const bool authored = model && model->collision.valid();
        if (!rejected && model) {
            if (!authored) {
                rejected = "no authored collision triangles";
            } else if (model->isInvisibleTrap || model->isRuntimeSpellVisual) {
                rejected = "classified as non-blocking";
            }
        }
        if (++reported > 12) { LOG_WARNING("  ... and more"); break; }
        LOG_WARNING("  '", model ? model->name : std::string("?"),
                    "' isGameObject=", instance.isGameObject ? 1 : 0,
                    " authoredCollision=", authored ? 1 : 0,
                    " tris=", authored ? model->collision.triCount : 0u,
                    " bounds z ", instance.worldBoundsMin.z, "..", instance.worldBoundsMax.z,
                    (rejected ? "  REJECTED: " : "  considered"), (rejected ? rejected : ""));
    }
    if (reported == 0) {
        LOG_WARNING("  nothing at all - no doodad instance overlaps this spot,"
                    " so a deck underfoot is not in the spatial grid here");
    }
    LOG_WARNING("=== Total: ", reported, " M2 candidates ===");
}

bool M2Renderer::getInstanceWorldBounds(uint32_t instanceId, glm::vec3& outMin,
                                        glm::vec3& outMax) const {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end() || idxIt->second >= instances.size()) return false;
    const auto& inst = instances[idxIt->second];
    if (!inst.cachedModel) return false;
    outMin = inst.worldBoundsMin;
    outMax = inst.worldBoundsMax;
    return true;
}

namespace {

// Grid categories use model-local normals. Pitch and roll can turn local walls
// into world-space floors, so world-down rays must query both categories.
void gatherPlacedFloorTriangles(const M2ModelGPU::CollisionMesh& mesh,
        const glm::vec3& rayOrigin, const glm::vec3& rayEnd,
        std::vector<uint32_t>& out) {
    const glm::vec2 low = glm::min(glm::vec2(rayOrigin), glm::vec2(rayEnd)) - glm::vec2(1);
    const glm::vec2 high = glm::max(glm::vec2(rayOrigin), glm::vec2(rayEnd)) + glm::vec2(1);
    mesh.getFloorTrisInRange(low.x, low.y, high.x, high.y, out);
    thread_local std::vector<uint32_t> walls;
    mesh.getWallTrisInRange(low.x, low.y, high.x, high.y, walls);
    out.insert(out.end(), walls.begin(), walls.end());
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

} // namespace

std::optional<float> M2Renderer::getInstanceFloorHeight(uint32_t instanceId,
                                                       float glX, float glY,
                                                       float glZ) const {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end() || idxIt->second >= instances.size()) {
        return std::nullopt;
    }
    const auto& instance = instances[idxIt->second];
    if (!instance.cachedModel || instance.scale <= 0.001f) return std::nullopt;
    const M2ModelGPU& model = *instance.cachedModel;
    if (!model.collision.valid()) return std::nullopt;

    // Same cast as getFloorHeight: world-down through the instance transform,
    // so a car placed with any pitch still sees a ray along gravity.
    const glm::vec3 localRayOrigin = glm::vec3(
        instance.invModelMatrix * glm::vec4(glX, glY, glZ + 5.0f, 1.0f));
    const glm::vec3 localRayEnd = glm::vec3(
        instance.invModelMatrix * glm::vec4(glX, glY, glZ - 10.0f, 1.0f));
    const glm::vec3 localRayVector = localRayEnd - localRayOrigin;
    const float localRayLength = glm::length(localRayVector);
    if (localRayLength <= 1e-5f) return std::nullopt;
    const glm::vec3 localRayDir = localRayVector / localRayLength;

    gatherPlacedFloorTriangles(model.collision, localRayOrigin, localRayEnd,
                               tl_m2_collisionTriScratch);

    std::optional<float> best;
    for (uint32_t ti : tl_m2_collisionTriScratch) {
        if (ti >= model.collision.triCount) continue;
        const auto& verts = model.collision.vertices;
        const auto& idx = model.collision.indices;
        const float tHit = rayTriangleIntersect(localRayOrigin, localRayDir,
                                                verts[idx[ti * 3]],
                                                verts[idx[ti * 3 + 1]],
                                                verts[idx[ti * 3 + 2]]);
        if (tHit < 0.0f || tHit > localRayLength) continue;
        const glm::vec3 worldHit = glm::vec3(
            instance.modelMatrix * glm::vec4(localRayOrigin + localRayDir * tHit, 1.0f));
        // At or under the probe, and the highest such - the deck rather than
        // whatever structure the car carries beneath it.
        if (worldHit.z <= glZ && (!best || worldHit.z > *best)) best = worldHit.z;
    }
    return best;
}

std::optional<float> M2Renderer::getFloorHeight(float glX, float glY, float glZ, float* outNormalZ) const {
    QueryTimer timer(&queryTimeMs, &queryCallCount);
    static const bool traceFloor = [] {
        const char* value = std::getenv("WOWEE_TRACE_M2_FLOOR");
        return value && value[0] == '1';
    }();
    std::optional<float> bestFloor;
    float bestNormalZ = 1.0f;  // Default to flat

    glm::vec3 queryMin(glX - 2.0f, glY - 2.0f, glZ - 6.0f);
    glm::vec3 queryMax(glX + 2.0f, glY + 2.0f, glZ + 8.0f);
    gatherCandidates(queryMin, queryMax, tl_m2_candidateScratch);

    for (size_t idx : tl_m2_candidateScratch) {
        const auto& instance = instances[idx];
        if (glX < instance.worldBoundsMin.x || glX > instance.worldBoundsMax.x ||
            glY < instance.worldBoundsMin.y || glY > instance.worldBoundsMax.y ||
            glZ + 5.0f < instance.worldBoundsMin.z || glZ - 10.0f > instance.worldBoundsMax.z) {
            continue;
        }

        if (!instance.cachedModel) continue;
        if (instance.scale <= 0.001f) continue;

        const M2ModelGPU& model = *instance.cachedModel;
        if (!m2BlocksMovement(model)) continue;
        if (instance.skipCollision) continue;

        // --- Mesh-based floor: vertical ray vs collision triangles ---
        // Authored collision geometry is authoritative, including empty space.
        if (model.collision.valid()) {
            // Opt-in placed geometry for reproducing contact outside the client.
            // Preserve float precision; rounded world coordinates can move a
            // small prop's collision edge by several centimeters.
            static const char* dumpDirectory = std::getenv("WOWEE_DUMP_M2_COLLISION");
            static std::unordered_set<uint32_t> dumpedInstances;
            if (dumpDirectory && dumpedInstances.insert(instance.id).second) {
                const auto path = std::filesystem::path(dumpDirectory) /
                    (std::to_string(instance.id) + ".obj");
                if (!std::filesystem::is_directory(dumpDirectory) || std::filesystem::exists(path)) {
                    LOG_ERROR("Collision dump requires existing directory and unused file: ", path.string());
                    throw std::runtime_error("Invalid collision dump destination");
                }
                std::ofstream output(path);
                output << std::setprecision(std::numeric_limits<float>::max_digits10)
                       << "# " << model.name << '\n';
                for (const auto& vertex : model.collision.vertices) {
                    const glm::vec3 world(instance.modelMatrix * glm::vec4(vertex, 1));
                    output << "v " << world.x << ' ' << world.y << ' ' << world.z << '\n';
                }
                for (uint32_t ti = 0; ti < model.collision.triCount; ++ti) {
                    output << "f " << model.collision.indices[ti*3]+1 << ' '
                           << model.collision.indices[ti*3+1]+1 << ' '
                           << model.collision.indices[ti*3+2]+1 << '\n';
                }
                output.close();
                if (!output) {
                    LOG_ERROR("Failed collision dump write: ", path.string());
                    throw std::runtime_error("Collision dump write failed");
                }
                LOG_INFO("Placed collision mesh saved: ", path.string(), " model=", model.name);
            }
            // Cast world-down through the instance transform. Using local -Z
            // only works for yaw-only placements; pitched flat models (notably
            // Exodarplatform01, placed as the Azuremyst flight-master ramp)
            // otherwise see a ray that is diagonal relative to world gravity.
            const glm::vec3 localRayOrigin = glm::vec3(
                instance.invModelMatrix * glm::vec4(glX, glY, glZ + 5.0f, 1.0f));
            const glm::vec3 localRayEnd = glm::vec3(
                instance.invModelMatrix * glm::vec4(glX, glY, glZ - 10.0f, 1.0f));
            glm::vec3 localRayVector = localRayEnd - localRayOrigin;
            const float localRayLength = glm::length(localRayVector);
            if (localRayLength <= 1e-5f) continue;
            const glm::vec3 localRayDir = localRayVector / localRayLength;

            gatherPlacedFloorTriangles(model.collision, localRayOrigin, localRayEnd,
                                       tl_m2_collisionTriScratch);

            float bestWorldHitZ = -std::numeric_limits<float>::max();
            float bestHitNormalZ = 1.0f;
            bool hitAny = false;

            for (uint32_t ti : tl_m2_collisionTriScratch) {
                if (ti >= model.collision.triCount) continue;

                const auto& verts = model.collision.vertices;
                const auto& idx   = model.collision.indices;
                const auto& v0 = verts[idx[ti * 3]];
                const auto& v1 = verts[idx[ti * 3 + 1]];
                const auto& v2 = verts[idx[ti * 3 + 2]];

                // The intersection is already two-sided, so the reversed
                // winding this used to retry answers the same thing.
                const float tHit = rayTriangleIntersect(localRayOrigin, localRayDir, v0, v1, v2);
                if (tHit < 0.0f || tHit > localRayLength) continue;

                const glm::vec3 localHit = localRayOrigin + localRayDir * tHit;
                const glm::vec3 worldHit = glm::vec3(
                    instance.modelMatrix * glm::vec4(localHit, 1.0f));

                // Walkable normal check (world space)
                glm::vec3 worldN(0.0f, 0.0f, 1.0f);  // Default to flat
                glm::vec3 localN = glm::cross(v1 - v0, v2 - v0);
                float nLen = glm::length(localN);
                if (nLen > 0.001f) {
                    localN /= nLen;
                    if (localN.z < 0.0f) localN = -localN;
                    glm::vec3 transformedN = glm::vec3(
                        instance.modelMatrix * glm::vec4(localN, 0.0f));
                    float wnLen = glm::length(transformedN);
                    if (wnLen > 0.001f) {
                        worldN = transformedN / wnLen;
                    } // else: keep worldN = (0,0,1) flat default
                    if (std::abs(worldN.z) < 0.35f) continue; // too steep (~70° max slope)
                }

                if (worldHit.z <= glZ + 3.0f && worldHit.z > bestWorldHitZ) {
                    bestWorldHitZ = worldHit.z;
                    hitAny = true;
                    bestHitNormalZ = std::abs(worldN.z);
                }
            }

            if (hitAny) {
                if (!bestFloor || bestWorldHitZ > *bestFloor) {
                    bestFloor = bestWorldHitZ;
                    bestNormalZ = bestHitNormalZ;
                    if (traceFloor) LOG_INFO("M2 floor candidate: model=", model.name,
                        " source=mesh height=", bestWorldHitZ, " normal=", bestHitNormalZ,
                        " query=", glX, ",", glY, ",", glZ);
                }
            }
            // A ray miss is empty space. Using the bounds as a floor here
            // creates invisible platforms beside trunks and across mesh gaps.
        }
    }

    // Output surface normal if requested
    if (outNormalZ) {
        *outNormalZ = bestNormalZ;
    }

    return bestFloor;
}

void M2Renderer::appendBodyCollisionTriangles(const glm::vec3& worldLow, const glm::vec3& worldHigh,
                                              std::vector<movement::BodyTriangle>& triangles) const {
    gatherCandidates(worldLow, worldHigh, tl_m2_candidateScratch);
    for (size_t idx : tl_m2_candidateScratch) {
        const auto& instance = instances[idx];
        if (instance.skipCollision || !instance.cachedModel || instance.scale <= 0.001f ||
            collisionFocus.excludes(instance.worldBoundsMin, instance.worldBoundsMax)) continue;
        const auto& model = *instance.cachedModel;
        if (!m2BlocksMovement(model)) continue;

        glm::vec3 localLow(std::numeric_limits<float>::max());
        glm::vec3 localHigh(-std::numeric_limits<float>::max());
        for (int corner = 0; corner < 8; ++corner) {
            const glm::vec3 world((corner & 1) ? worldHigh.x : worldLow.x,
                                  (corner & 2) ? worldHigh.y : worldLow.y,
                                  (corner & 4) ? worldHigh.z : worldLow.z);
            const glm::vec3 local(instance.invModelMatrix * glm::vec4(world, 1));
            localLow = glm::min(localLow, local);
            localHigh = glm::max(localHigh, local);
        }
        model.collision.getWallTrisInRange(localLow.x, localLow.y, localHigh.x, localHigh.y,
                                           tl_m2_collisionTriScratch);
        thread_local std::vector<uint32_t> floorTriangles;
        model.collision.getFloorTrisInRange(localLow.x, localLow.y, localHigh.x, localHigh.y,
                                            floorTriangles);
        tl_m2_collisionTriScratch.insert(tl_m2_collisionTriScratch.end(),
                                         floorTriangles.begin(), floorTriangles.end());
        std::sort(tl_m2_collisionTriScratch.begin(), tl_m2_collisionTriScratch.end());
        tl_m2_collisionTriScratch.erase(std::unique(tl_m2_collisionTriScratch.begin(),
            tl_m2_collisionTriScratch.end()), tl_m2_collisionTriScratch.end());
        for (uint32_t ti : tl_m2_collisionTriScratch) {
            movement::BodyTriangle triangle;
            for (size_t corner = 0; corner < 3; ++corner) {
                triangle[corner] = glm::vec3(instance.modelMatrix * glm::vec4(
                    model.collision.vertices[model.collision.indices[ti*3+corner]], 1));
            }
            triangles.push_back(triangle);
        }
    }
}

bool M2Renderer::constrainFloorSnap(const glm::vec3& from, float requestedZ,
                                   float radius, float height,
                                   float& supportZ, float& supportNormalZ) const {
    assert(requestedZ < from.z);
    QueryTimer timer(&queryTimeMs, &queryCallCount);
    const glm::vec3 to(from.x, from.y, requestedZ);
    const glm::vec3 delta = to - from;
    thread_local std::vector<movement::BodyTriangle> triangles;
    triangles.clear();
    appendBodyCollisionTriangles(glm::vec3(from.x-radius, from.y-radius, requestedZ),
                                 glm::vec3(from.x+radius, from.y+radius, from.z+height), triangles);
    std::optional<movement::BodyContact> first;
    for (const auto& triangle : triangles) {
        const auto contact = movement::sweepBodyTriangle(from, delta, radius, height, triangle);
        if (contact && (!first || contact->time < first->time)) first = contact;
    }
    if (!first) return false;
    supportZ = from.z + delta.z * first->time;
    supportNormalZ = first->normal.z;
    return true;
}

glm::vec3 M2Renderer::stepBody(const glm::vec3& from, const glm::vec3& to,
                               float radius, float height, float maxStepUp,
                               glm::vec3* landingFace) const {
    QueryTimer timer(&queryTimeMs, &queryCallCount);
    thread_local std::vector<movement::BodyTriangle> triangles;
    triangles.clear();
    appendBodyCollisionTriangles(glm::min(from, to) - glm::vec3(radius, radius, 0.0f),
                                 glm::max(from, to) + glm::vec3(radius, radius, height + maxStepUp),
                                 triangles);
    return movement::stepBodyAcross(from, to, radius, height, triangles, maxStepUp, landingFace);
}

bool M2Renderer::checkCollision(const glm::vec3& from, const glm::vec3& to,
                                 glm::vec3& adjustedPos, float playerRadius, float playerHeight,
                                 bool allowHorizontalRise) const {
    QueryTimer timer(&queryTimeMs, &queryCallCount);
    assert(playerRadius > 0.0f && playerHeight >= movement::kBodyBevel * playerRadius);
    adjustedPos = to;
    bool collided = false;

    glm::vec3 queryMin = glm::min(from, to) - glm::vec3(7.0f, 7.0f, 5.0f);
    glm::vec3 queryMax = glm::max(from, to) + glm::vec3(7.0f, 7.0f, std::max(5.0f, playerHeight));
    gatherCandidates(queryMin, queryMax, tl_m2_candidateScratch);

    // Check against all M2 instances in local space (rotation-aware).
    for (size_t idx : tl_m2_candidateScratch) {
        const auto& instance = instances[idx];
        if (collisionFocus.excludes(instance.worldBoundsMin,
                                    instance.worldBoundsMax)) {
            continue;
        }

        const float broadMargin = playerRadius + 1.0f;
        if (from.x < instance.worldBoundsMin.x - broadMargin && adjustedPos.x < instance.worldBoundsMin.x - broadMargin) continue;
        if (from.x > instance.worldBoundsMax.x + broadMargin && adjustedPos.x > instance.worldBoundsMax.x + broadMargin) continue;
        if (from.y < instance.worldBoundsMin.y - broadMargin && adjustedPos.y < instance.worldBoundsMin.y - broadMargin) continue;
        if (from.y > instance.worldBoundsMax.y + broadMargin && adjustedPos.y > instance.worldBoundsMax.y + broadMargin) continue;
        if (from.z > instance.worldBoundsMax.z + 2.5f && adjustedPos.z > instance.worldBoundsMax.z + 2.5f) continue;
        if (from.z + playerHeight < instance.worldBoundsMin.z && adjustedPos.z + playerHeight < instance.worldBoundsMin.z) continue;

        if (!instance.cachedModel) continue;

        const M2ModelGPU& model = *instance.cachedModel;
        if (!m2BlocksMovement(model)) continue;
        // WOWEE_TRACE_M2_COLLISION=<model name substring>: log every wall
        // query that reaches that model, in its local space (ledger #12,
        // Exodar table contact). Read once, like WOWEE_TRACE_M2_FLOOR.
        static const std::string traceModel = [] {
            const char* value = std::getenv("WOWEE_TRACE_M2_COLLISION");
            return std::string(value ? value : "");
        }();
        if (!traceModel.empty() && model.name.find(traceModel) != std::string::npos) {
            const glm::vec3 local(instance.invModelMatrix * glm::vec4(from, 1.0f));
            LOG_INFO("M2 collision query model=", model.name, " local=", local.x, ",", local.y, ",", local.z,
                     " body=", playerRadius, ",", playerHeight, " scale=", instance.scale,
                     " mesh=", model.collision.triCount, " skip=", instance.skipCollision);
        }
        if (instance.skipCollision) continue;
        if (instance.scale <= 0.001f) continue;

        // --- Mesh-based wall collision: closest-point push ---
        if (model.collision.valid()) {
            glm::vec3 localFrom = glm::vec3(instance.invModelMatrix * glm::vec4(from, 1.0f));
            glm::vec3 localPos  = glm::vec3(instance.invModelMatrix * glm::vec4(adjustedPos, 1.0f));
            float localRadius = playerRadius / instance.scale;

            // The body is vertical in world space, even for pitched/rolled
            // doodads. Query every triangle classification touched by its span:
            // a model-local floor can become a world-space wall after placement.
            const glm::vec3 localTopFrom(instance.invModelMatrix * glm::vec4(from + glm::vec3(0,0,playerHeight), 1));
            const glm::vec3 localTopTo(instance.invModelMatrix * glm::vec4(adjustedPos + glm::vec3(0,0,playerHeight), 1));
            const glm::vec3 queryLow = glm::min(glm::min(localFrom, localPos), glm::min(localTopFrom, localTopTo));
            const glm::vec3 queryHigh = glm::max(glm::max(localFrom, localPos), glm::max(localTopFrom, localTopTo));
            model.collision.getWallTrisInRange(
                queryLow.x - localRadius, queryLow.y - localRadius,
                queryHigh.x + localRadius, queryHigh.y + localRadius, tl_m2_collisionTriScratch);
            thread_local std::vector<uint32_t> floorTriangles;
            model.collision.getFloorTrisInRange(
                queryLow.x - localRadius, queryLow.y - localRadius,
                queryHigh.x + localRadius, queryHigh.y + localRadius, floorTriangles);
            tl_m2_collisionTriScratch.insert(tl_m2_collisionTriScratch.end(), floorTriangles.begin(), floorTriangles.end());
            std::sort(tl_m2_collisionTriScratch.begin(), tl_m2_collisionTriScratch.end());
            tl_m2_collisionTriScratch.erase(std::unique(tl_m2_collisionTriScratch.begin(), tl_m2_collisionTriScratch.end()), tl_m2_collisionTriScratch.end());
            thread_local std::vector<movement::BodyTriangle> triangles;
            triangles.clear();
            for (uint32_t ti : tl_m2_collisionTriScratch) {
                movement::BodyTriangle triangle;
                for (size_t corner=0;corner<3;++corner) {
                    triangle[corner]=glm::vec3(instance.modelMatrix * glm::vec4(
                        model.collision.vertices[model.collision.indices[ti*3+corner]],1.0f));
                }
                triangles.push_back(triangle);
            }
            glm::vec3 contactPosition;
            const bool pushed=movement::moveBody(from,adjustedPos,playerRadius,playerHeight,
                triangles,contactPosition,allowHorizontalRise);
            adjustedPos=contactPosition;

            if (pushed) {
                collided = true;
                // Which doodad is in the way, once per model.
                //
                // A model that blocks and should not is reported as "I am
                // stuck on this bush", and the one thing needed to fix it -
                // the model's name - is the one thing nobody can see. The
                // classifier decides collision from that name, so without it
                // the only way forward is guessing tokens, which is how the
                // folder-token regression happened.
                //
                // Once per distinct model, not per frame: walking into
                // something touches it every frame for as long as you lean on
                // it.
                static std::set<std::string> saidBlocked;
                if (!model.name.empty() && saidBlocked.insert(model.name).second) {
                    LOG_WARNING("Collision: blocked by '", model.name,
                                "' - if this should be walked through, that is "
                                "the name the classifier needs");
                }
            }
        }
    }

    return collided;
}

float M2Renderer::raycastVisibleGeometry(const glm::vec3& origin, const glm::vec3& direction,
                                        float maxDistance, uint32_t excludedInstanceId,
                                        uint32_t requiredInstanceId) const {
    float closest = maxDistance;
    const glm::vec3 end = origin + direction * maxDistance;
    gatherCandidates(glm::min(origin, end), glm::max(origin, end), tl_m2_candidateScratch);
    for (size_t index : tl_m2_candidateScratch) {
        const auto& instance = instances[index];
        if (requiredInstanceId != 0 && instance.id != requiredInstanceId) continue;
        if (instance.id == excludedInstanceId || !instance.cachedModel) continue;
        const auto& model = *instance.cachedModel;
        // Picking one object (requiredInstanceId) tests every batch it draws.
        // Shattrath's city portals (MagePortal_*.m2) are a single additive
        // quad; build 8606 takes the cursor over that quad (pair 0 hover),
        // so neither the spell-effect class nor additive blending removes
        // an object's own surface. Occlusion queries still skip them.
        const bool ownSurface = requiredInstanceId != 0;
        if (model.isInvisibleTrap || (model.isSpellEffect && !ownSurface)) continue;
        float enter = 0.0f;
        if (!segmentIntersectsAABB(origin, end, instance.worldBoundsMin,
                                   instance.worldBoundsMax, enter)) continue;
        const glm::vec3 localOrigin(instance.invModelMatrix * glm::vec4(origin, 1));
        const glm::vec3 localDirection(instance.invModelMatrix * glm::vec4(direction, 0));
        const bool useBones = model.hasAnimation && !model.disableAnimation &&
                              !instance.boneMatrices.empty();
        std::vector<glm::vec3> positions;
        positions.reserve(model.pickVertices.size());
        for (const auto& vertex : model.pickVertices) {
            glm::vec4 position(vertex.position, 1.0f);
            if (useBones) {
                position = glm::vec4(0.0f);
                for (size_t b = 0; b < 4; ++b) {
                    if (!vertex.boneWeights[b]) continue;
                    const size_t bone = std::min<size_t>(vertex.boneIndices[b], instance.boneMatrices.size() - 1);
                    position += instance.boneMatrices[bone] * glm::vec4(vertex.position, 1.0f) *
                                (vertex.boneWeights[b] / 255.0f);
                }
            }
            positions.emplace_back(position);
        }
        for (const auto& batch : model.batches) {
            // Batch flag 0x08 marks non-pickable light/aura geometry; additive
            // glow cards skip occlusion queries while remaining pickable for portals.
            if (!m2BatchIsPickable(batch.batchFlags, batch.submeshLevel, batch.blendMode,
                                   batch.batchOpacity, batch.starLayer, ownSurface)) continue;
            assert(batch.indexStart + batch.indexCount <= model.pickIndices.size());
            closest = pickVisualTriangles(positions,
                std::span<const uint16_t>(model.pickIndices).subspan(batch.indexStart, batch.indexCount),
                localOrigin, localDirection, closest);
        }
    }
    return closest;
}

float M2Renderer::raycastBoundingBoxes(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, uint32_t excludedInstanceId) const {
    QueryTimer timer(&queryTimeMs, &queryCallCount);
    float closestHit = maxDistance;

    glm::vec3 rayEnd = origin + direction * maxDistance;
    glm::vec3 queryMin = glm::min(origin, rayEnd) - glm::vec3(1.0f);
    glm::vec3 queryMax = glm::max(origin, rayEnd) + glm::vec3(1.0f);
    gatherCandidates(queryMin, queryMax, tl_m2_candidateScratch);

    for (size_t idx : tl_m2_candidateScratch) {
        const auto& instance = instances[idx];
        if (instance.id == excludedInstanceId) continue;
        // An open door (GO state 0) stops nothing, the camera included: in
        // Zul'Aman the open fire door (186859, ZulAman_FireDoor.m2) at
        // (120.3, 732.0) let 8606's camera reach 15 yd from (120, 722) where
        // WoWee's was held at 8.45; with the door shut 8606 stops at 8.45.
        if (instance.skipCollision) continue;
        if (collisionFocus.excludes(instance.worldBoundsMin,
                                    instance.worldBoundsMax)) {
            continue;
        }

        // Cheap world-space broad-phase.
        float tEnter = 0.0f;
        glm::vec3 worldMin = instance.worldBoundsMin - glm::vec3(0.35f);
        glm::vec3 worldMax = instance.worldBoundsMax + glm::vec3(0.35f);
        if (!segmentIntersectsAABB(origin, origin + direction * maxDistance, worldMin, worldMax, tEnter)) {
            continue;
        }

        if (!instance.cachedModel) continue;

        const M2ModelGPU& model = *instance.cachedModel;
        if (!m2BlocksMovement(model)) continue;
        glm::vec3 localMin, localMax;
        getTightCollisionBounds(model, localMin, localMax);
        // Skip tiny doodads for camera occlusion; they cause jitter and false hits.
        glm::vec3 extents = (localMax - localMin) * instance.scale;
        if (glm::dot(extents, extents) < 0.5625f) continue;

        glm::vec3 localOrigin = glm::vec3(instance.invModelMatrix * glm::vec4(origin, 1.0f));
        glm::vec3 localDir = glm::normalize(glm::vec3(instance.invModelMatrix * glm::vec4(direction, 0.0f)));
        if (!std::isfinite(localDir.x) || !std::isfinite(localDir.y) || !std::isfinite(localDir.z)) {
            continue;
        }

        // Bounds only cull candidates. Their empty corners must not push
        // the camera into the player when approaching an irregular prop.
        const glm::vec3 localEnd = glm::vec3(instance.invModelMatrix *
            glm::vec4(origin + direction * closestHit, 1.0f));
        const glm::vec2 rangeMin = glm::min(glm::vec2(localOrigin), glm::vec2(localEnd));
        const glm::vec2 rangeMax = glm::max(glm::vec2(localOrigin), glm::vec2(localEnd));
        const auto& mesh = model.collision;
        // Camera rays must test floors and ceilings as well as walls.
        for (const auto* cells : {&mesh.cellFloorTris, &mesh.cellWallTris}) {
            mesh.gatherTrisInRange(*cells, rangeMin.x, rangeMin.y,
                                  rangeMax.x, rangeMax.y, tl_m2_collisionTriScratch);
            for (uint32_t ti : tl_m2_collisionTriScratch) {
                const float t = rayTriangleIntersect(localOrigin, localDir,
                    mesh.vertices[mesh.indices[ti * 3]],
                    mesh.vertices[mesh.indices[ti * 3 + 1]],
                    mesh.vertices[mesh.indices[ti * 3 + 2]]);
                if (t < 0.0f) continue;
                const glm::vec3 worldHit = glm::vec3(instance.modelMatrix *
                    glm::vec4(localOrigin + localDir * t, 1.0f));
                closestHit = std::min(closestHit, glm::length(worldHit - origin));
            }
        }
    }

    return closestHit;
}

void M2Renderer::recreatePipelines() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();

    // Destroy old main-pass pipelines (NOT shadow, NOT pipeline layouts)
    destroy(device, opaquePipeline_);
    destroy(device, cutoutPipeline_);
    destroy(device, foliageCullOpaquePipeline_);
    destroy(device, foliageCullCutoutPipeline_);
    destroy(device, cullAlphaPipeline_);
    destroy(device, cullAdditivePipeline_);
    destroy(device, alphaTestPipeline_);
    destroy(device, alphaPipeline_);
    destroy(device, additivePipeline_);
    destroy(device, modPipeline_);
    destroy(device, mod2xPipeline_);
    destroy(device, cullModPipeline_);
    destroy(device, cullMod2xPipeline_);
    destroy(device, particlePipeline_);
    destroy(device, particleAdditivePipeline_);
    destroy(device, particleModPipeline_);
    destroy(device, particleMod2xPipeline_);
    destroy(device, ribbonPipeline_);
    destroy(device, ribbonAdditivePipeline_);
    destroy(device, particleMeshPipeline_);
    destroy(device, particleMeshAdditivePipeline_);

    // The same ten pipelines initialize() builds, built by the same
    // function. The layouts are untouched above, so it makes none.
    buildMainPassPipelines(perFrameLayout_);

    core::Logger::getInstance().info("M2Renderer: pipelines recreated");
}

void M2Renderer::collectGrassClearings(float minX, float minY, float maxX, float maxY,
                                       std::vector<pipeline::GrassClearingSource>& out) const {
    constexpr float kEase = 4.0f;
    for (const auto& inst : instances) {
        // The ground detail IS the grass's kin - clutter clearing the ground
        // around clutter would eat the whole field.
        if (inst.cachedIsGroundDetail) continue;

        // Footprint radius from the horizontal bounds, capped: a fence post
        // clears its foot, a wagon its wheelbase, and a tree its trunk - not
        // the shadow of its crown, or every forest would be bald.
        const float ex = inst.worldBoundsMax.x - inst.worldBoundsMin.x;
        const float ey = inst.worldBoundsMax.y - inst.worldBoundsMin.y;
        const float clearing = std::clamp(0.35f * std::max(ex, ey), 0.4f, 3.0f);
        const float reach = clearing + kEase;

        const float cx = 0.5f * (inst.worldBoundsMin.x + inst.worldBoundsMax.x);
        const float cy = 0.5f * (inst.worldBoundsMin.y + inst.worldBoundsMax.y);
        if (cx + reach < minX || cx - reach > maxX ||
            cy + reach < minY || cy - reach > maxY) {
            continue;
        }
        out.push_back({cx, cy, cx, cy, clearing, kEase});
    }
}

} // namespace rendering
} // namespace wowee
