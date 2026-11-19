#include "rendering/shadow_params.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/m2_renderer_internal.h"
#include "rendering/m2_draw_mode.hpp"
#include "rendering/m2_blend_mode.hpp"
#include "rendering/m2_foliage_culling.hpp"
#include "rendering/m2_glow_card.hpp"
#include "game/game_utils.hpp"
#include "core/thread_pool.hpp"
#include "rendering/m2_model_classifier.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_buffer.hpp"
#include "rendering/vk_texture.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_utils.hpp"
#include "rendering/vk_frame_data.hpp"
#include "rendering/camera.hpp"
#include "rendering/frustum.hpp"
#include "rendering/render_constants.hpp"
#include "rendering/m2_view_distance.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/blp_loader.hpp"
#include "core/logger.hpp"
#include "core/profiler.hpp"
#include <chrono>
#include <cctype>
#include <cstring>
#include <span>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <unordered_set>
#include <functional>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <random>
#include <limits>
#include <future>
#include <thread>

#include <set>
#include <utility>

namespace wowee {
namespace rendering {

/// Starts a new instance's animation and gives it bones to draw with now.
///
/// Both spawn paths need this: the one that takes a position and the one that
/// takes a whole matrix, and each had its own copy.
///
/// The bone seed is what keeps a new instance from being invisible for a
/// frame. Bones are computed in update(), so an instance spawned mid-frame has
/// none until the next one; copying them from a sibling of the same model
/// draws it immediately. A seed entry pointing at an instance that has since
/// gone is dropped rather than followed.
void M2Renderer::seedInstanceAnimation(const M2ModelGPU& model, uint32_t modelId,
                                       M2Instance& instance) {
        if (!model.sequences.empty()) {
            // Stand, not whichever sequence comes first. G_BrazierTroll lists
            // Destroy first and the Stranglethorn cage doors (g_cage02.m2) Open:
            // looped, the brazier went out and back on and the cage doors
            // swung open forever, where the original stands lit and shut.
            const int stand = model.standSequenceIndex;
            instance.currentSequenceIndex = stand;
            instance.idleSequenceIndex = stand;
            instance.animDuration = static_cast<float>(model.sequences[stand].duration);
            instance.animTime = static_cast<float>(randRange(std::max(1u, model.sequences[stand].duration)));
            instance.variationTimer = randFloat(rendering::M2_VARIATION_TIMER_MIN_MS, rendering::M2_VARIATION_TIMER_MAX_MS);
        }

    auto seedIt = boneSeedInstanceByModel_.find(modelId);
    const bool cameraDependent = std::any_of(model.bones.begin(), model.bones.end(), [](const auto& bone) {
        return (bone.flags & (kM2BoneSphericalBillboard | kM2BoneCylindricalBillboardZ)) != 0;
    });
    if (!cameraDependent && seedIt != boneSeedInstanceByModel_.end()) {
        auto idxIt = instanceIndexById.find(seedIt->second);
        if (idxIt != instanceIndexById.end() && idxIt->second < instances.size()) {
            const auto& existing = instances[idxIt->second];
            if (existing.modelId == modelId && !existing.boneMatrices.empty()) {
                instance.boneMatrices = existing.boneMatrices;
                instance.bonesDirty[0] = instance.bonesDirty[1] = true;
            } else {
                boneSeedInstanceByModel_.erase(seedIt);  // stale entry
            }
        } else {
            boneSeedInstanceByModel_.erase(seedIt);  // that instance is gone
        }
    }

    // No sibling to copy from, so pay for the bones now.
    if (instance.boneMatrices.empty()) {
        computeBoneMatrices(model, instance, &cachedCamPos_);
    }
    if (!instance.boneMatrices.empty()) {
        boneSeedInstanceByModel_.emplace(modelId, instance.id);
    }
}

// A model the bone loop does not step still plays its Stand sequence: its
// texture scroll and its emitters are keyed to it, and update() wraps the
// clock at this length. Left at zero, the length was unknown, the clock was
// wrapped at a fixed 3.3 s instead, and a waterfall whose scroll lasts 2.6 s
// stood still for the difference every lap.
void M2Renderer::seedUnanimatedSequence(const M2ModelGPU& model, M2Instance& instance) {
    if (model.sequences.empty()) return;
    const int stand = model.standSequenceIndex;
    if (model.sequences[stand].duration == 0) return;
    instance.currentSequenceIndex = stand;
    instance.idleSequenceIndex = stand;
    instance.animDuration = static_cast<float>(model.sequences[stand].duration);
    instance.animTime = static_cast<float>(randRange(model.sequences[stand].duration));
}

uint32_t M2Renderer::createInstance(uint32_t modelId, const glm::vec3& position,
                                     const glm::vec3& rotation, float scale,
                                     bool allowPositionDedup) {
    // Reject NaN inputs at the boundary - std::round of NaN is implementation-
    // defined and a NaN instance position propagates into the GPU model matrix,
    // either tripping Vulkan validation or rendering at the world origin.
    if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
        !std::isfinite(position.z) || !std::isfinite(rotation.x) ||
        !std::isfinite(rotation.y) || !std::isfinite(rotation.z) ||
        !std::isfinite(scale) || scale <= 0.0f) {
        return 0;
    }
    auto modelIt = models.find(modelId);
    if (modelIt == models.end()) {
        LOG_WARNING("Cannot create instance: model ", modelId, " not loaded");
        return 0;
    }
    const auto& mdlRef = modelIt->second;
    modelUnusedSince_.erase(modelId);


    // Deduplicate: skip if same model already at nearly the same position.
    // Uses hash map for O(1) lookup instead of O(N) scan.
    // Spell effects are exempt - transient visuals must always create fresh instances.
    if (allowPositionDedup && !mdlRef.isGroundDetail && !mdlRef.isSpellEffect) {
        DedupKey dk{.modelId = modelId,
                    .qx = static_cast<int32_t>(std::round(position.x * 10.0f)),
                    .qy = static_cast<int32_t>(std::round(position.y * 10.0f)),
                    .qz = static_cast<int32_t>(std::round(position.z * 10.0f))};
        auto dit = instanceDedupMap_.find(dk);
        if (dit != instanceDedupMap_.end()) {
            return dit->second;
        }
    }

    M2Instance instance;
    instance.id = nextInstanceId++;
    instance.modelId = modelId;
    instance.position = position;
    if (mdlRef.isGroundDetail) {
        instance.position.z -= computeGroundDetailDownOffset(mdlRef, scale);
    }
    instance.rotation = rotation;
    instance.scale = scale;
    instance.updateModelMatrix();
    glm::vec3 localMin, localMax;
    getTightCollisionBounds(mdlRef, localMin, localMax);
    transformAABB(instance.modelMatrix, localMin, localMax, instance.worldBoundsMin, instance.worldBoundsMax);

    // Cache model flags on instance to avoid per-frame hash lookups
    instance.cachedHasAnimation = mdlRef.hasAnimation;
    instance.cachedDisableAnimation = mdlRef.disableAnimation;
    instance.cachedHasParticleEmitters = !mdlRef.particleEmitters.empty();
    instance.cachedBoundRadius = mdlRef.boundRadius;
    instance.cachedIsGroundDetail = mdlRef.isGroundDetail;
    instance.cachedIsInvisibleTrap = mdlRef.isInvisibleTrap;
    instance.cachedIsSkyBird = mdlRef.isSkyBird;
    instance.cachedIsLightBeam = mdlRef.isLightBeam;
    instance.cachedIsTransportDoodad = mdlRef.isTransportDoodad;
    instance.cachedIsValid = mdlRef.isValid();
    instance.cachedModel = &mdlRef;
    instance.recomputeCachedCullFactors();

    // Initialize animation: play first sequence (usually Stand/Idle)
    const auto& mdl = mdlRef;
    if (mdl.hasAnimation && !mdl.disableAnimation) {
        seedInstanceAnimation(mdlRef, modelId, instance);
    } else {
        seedUnanimatedSequence(mdlRef, instance);
    }

    // Register in dedup map before pushing (uses original position, not ground-adjusted)
    // Spell effects are exempt from dedup tracking (transient, overlapping allowed).
    if (allowPositionDedup && !mdlRef.isGroundDetail && !mdlRef.isSpellEffect) {
        DedupKey dk{.modelId = modelId,
                    .qx = static_cast<int32_t>(std::round(position.x * 10.0f)),
                    .qy = static_cast<int32_t>(std::round(position.y * 10.0f)),
                    .qz = static_cast<int32_t>(std::round(position.z * 10.0f))};
        instanceDedupMap_[dk] = instance.id;
    }

    // WOWEE_M2_CENSUS: what the model is drawn at, beside what it was
    // authored at. The load-time census gives the authored height; a game
    // object's scale comes from the server and is only known here.
    censusInstance(instance);
    instances.push_back(instance);
    size_t idx = instances.size() - 1;
    // Track special instances for fast-path iteration
    if (!mdlRef.particleEmitters.empty()) {
        particleInstanceIndices_.push_back(idx);
    }
    if (mdlRef.hasAnimation && !mdlRef.disableAnimation) {
        animatedInstanceIndices_.push_back(idx);
    } else if (!mdlRef.particleEmitters.empty()) {
        particleOnlyInstanceIndices_.push_back(idx);
    }
    instanceIndexById[instance.id] = idx;
    insertBounds(spatialGrid, instance.worldBoundsMin, instance.worldBoundsMax, instance.id);

    return instance.id;
}

uint32_t M2Renderer::createInstanceWithMatrix(uint32_t modelId, const glm::mat4& modelMatrix,
                                                const glm::vec3& position) {
    // Reject NaN inputs at the boundary. position feeds the dedup hash
    // (std::round of NaN is implementation-defined); the matrix goes
    // straight to the GPU UBO and would crash validation.
    if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
        !std::isfinite(position.z)) {
        return 0;
    }
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            if (!std::isfinite(modelMatrix[c][r])) return 0;
    if (models.find(modelId) == models.end()) {
        LOG_WARNING("Cannot create instance: model ", modelId, " not loaded");
        return 0;
    }
    modelUnusedSince_.erase(modelId);

    // Deduplicate: O(1) hash lookup
    {
        DedupKey dk{.modelId = modelId,
                    .qx = static_cast<int32_t>(std::round(position.x * 10.0f)),
                    .qy = static_cast<int32_t>(std::round(position.y * 10.0f)),
                    .qz = static_cast<int32_t>(std::round(position.z * 10.0f))};
        auto dit = instanceDedupMap_.find(dk);
        if (dit != instanceDedupMap_.end()) {
            return dit->second;
        }
    }

    M2Instance instance;
    instance.id = nextInstanceId++;
    instance.modelId = modelId;
    instance.position = position;  // Used for frustum culling
    instance.rotation = glm::vec3(0.0f);
    instance.scale = 1.0f;
    instance.modelMatrix = modelMatrix;
    instance.invModelMatrix = glm::inverse(modelMatrix);
    glm::vec3 localMin, localMax;
    getTightCollisionBounds(models[modelId], localMin, localMax);
    transformAABB(instance.modelMatrix, localMin, localMax, instance.worldBoundsMin, instance.worldBoundsMax);
    // Cache model flags on instance to avoid per-frame hash lookups
    const auto& mdl2 = models[modelId];
    instance.cachedHasAnimation = mdl2.hasAnimation;
    instance.cachedDisableAnimation = mdl2.disableAnimation;
    instance.cachedHasParticleEmitters = !mdl2.particleEmitters.empty();
    instance.cachedBoundRadius = mdl2.boundRadius;
    instance.cachedIsGroundDetail = mdl2.isGroundDetail;
    instance.cachedIsInvisibleTrap = mdl2.isInvisibleTrap;
    instance.cachedIsSkyBird = mdl2.isSkyBird;
    instance.cachedIsLightBeam = mdl2.isLightBeam;
    instance.cachedIsTransportDoodad = mdl2.isTransportDoodad;
    instance.cachedIsValid = mdl2.isValid();
    instance.cachedModel = &mdl2;
    instance.recomputeCachedCullFactors();

    // Initialize animation
    if (mdl2.hasAnimation && !mdl2.disableAnimation) {
        seedInstanceAnimation(mdl2, modelId, instance);
    } else {
        // A model with no skeleton can still have particle emitters, and their
        // rate and lifespan tracks are sampled at animTime. Starting every
        // instance at zero puts a courtyard of identical torches in lockstep,
        // so the phase is spread.
        //
        // createInstance above now does the same.
        instance.animTime = randFloat(0.0f, 10000.0f);
        seedUnanimatedSequence(mdl2, instance);
    }

    // Register in dedup map
    {
        DedupKey dk{.modelId = modelId,
                    .qx = static_cast<int32_t>(std::round(position.x * 10.0f)),
                    .qy = static_cast<int32_t>(std::round(position.y * 10.0f)),
                    .qz = static_cast<int32_t>(std::round(position.z * 10.0f))};
        instanceDedupMap_[dk] = instance.id;
    }

    // WOWEE_M2_CENSUS: what the model is drawn at, beside what it was
    // authored at. The load-time census gives the authored height; a game
    // object's scale comes from the server and is only known here.
    censusInstance(instance);
    instances.push_back(instance);
    size_t idx = instances.size() - 1;
    if (!mdl2.particleEmitters.empty()) {
        particleInstanceIndices_.push_back(idx);
    }
    if (mdl2.hasAnimation && !mdl2.disableAnimation) {
        animatedInstanceIndices_.push_back(idx);
    } else if (!mdl2.particleEmitters.empty()) {
        particleOnlyInstanceIndices_.push_back(idx);
    }
    instanceIndexById[instance.id] = idx;
    insertBounds(spatialGrid, instance.worldBoundsMin, instance.worldBoundsMax, instance.id);

    return instance.id;
}

uint32_t M2Renderer::createExternalPoseInstance(uint32_t modelId, const glm::mat4& modelMatrix) {
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            if (!std::isfinite(modelMatrix[c][r])) return 0;
    auto modelIt = models.find(modelId);
    if (modelIt == models.end()) {
        LOG_WARNING("Cannot create external-pose instance: model ", modelId, " not loaded");
        return 0;
    }
    const auto& mdl = modelIt->second;
    modelUnusedSince_.erase(modelId);

    // No dedup: two creatures of one model standing on one spot each keep
    // their own emitters.
    M2Instance instance;
    instance.id = nextInstanceId++;
    instance.modelId = modelId;
    instance.externalPose = true;
    instance.skipCollision = true;
    instance.position = glm::vec3(modelMatrix[3]);
    instance.rotation = glm::vec3(0.0f);
    instance.scale = 1.0f;
    instance.modelMatrix = modelMatrix;
    instance.invModelMatrix = glm::inverse(modelMatrix);
    instance.worldBoundsMin = instance.worldBoundsMax = instance.position;
    instance.cachedHasParticleEmitters = !mdl.particleEmitters.empty();
    instance.cachedBoundRadius = mdl.boundRadius;
    instance.cachedIsValid = mdl.isValid();
    instance.cachedModel = &mdl;
    instance.recomputeCachedCullFactors();

    instances.push_back(std::move(instance));
    const size_t idx = instances.size() - 1;
    // Only the emitter list: never the animated list (its bones are the
    // owner's) nor the particle-only list (its clock is the owner's).
    if (!mdl.particleEmitters.empty() || !mdl.ribbonEmitters.empty())
        particleInstanceIndices_.push_back(idx);
    instanceIndexById[instances[idx].id] = idx;
    insertBounds(spatialGrid, instances[idx].worldBoundsMin, instances[idx].worldBoundsMax,
                 instances[idx].id);
    return instances[idx].id;
}

void M2Renderer::setExternalPose(uint32_t instanceId, const ExternalPose& pose) {
    auto idxIt = instanceIndexById.find(instanceId);
    assert(idxIt != instanceIndexById.end());
    auto& inst = instances[idxIt->second];
    assert(inst.externalPose);
    setInstanceTransform(instanceId, pose.modelMatrix);
    if (pose.boneMatrices) inst.boneMatrices = *pose.boneMatrices;
    else inst.boneMatrices.clear();
    inst.currentSequenceIndex = pose.sequenceIndex;
    inst.animTime = pose.animTimeMs;
    inst.globalSequenceTime = pose.globalSequenceTimeMs;
    inst.fade = pose.fade;
    inst.externalClockHeld = pose.clockHeld;
    assert(pose.particleScale > 0.0f && "ExternalPose::particleScale must be set by the owner");
    inst.externalParticleScale = pose.particleScale;
}

void M2Renderer::updateExternalPoseEffects(float deltaTime) {
    for (size_t idx : particleInstanceIndices_) {
        if (idx >= instances.size()) continue;
        auto& instance = instances[idx];
        if (!instance.externalPose || !instance.cachedModel) continue;
        const glm::vec3 toCam = instance.position - cachedCamPos_;
        if (glm::dot(toCam, toCam) > particleMaxDistSq(instance)) continue;
        updateParticles(instance, deltaTime);
        emitParticles(instance, *instance.cachedModel, deltaTime);
        if (!instance.cachedModel->ribbonEmitters.empty())
            updateRibbons(instance, *instance.cachedModel, deltaTime);
    }
}

// WOWEE_SKY_M2_MAX_BATCH=<n> draws only the sky model's first n layers.
//
// Thirty-four of them, and every measurement says the set drawn is the same
// every frame - so if the flicker is one layer's doing, bisecting the count
// finds it in about five runs. That has worked three times on this fault where
// reading the code has worked none.
static bool skyBatchAllowed(bool skyMode, std::size_t index) {
    if (!skyMode) return true;
    static const int maxBatch = [] {
        const char* set = std::getenv("WOWEE_SKY_M2_MAX_BATCH");
        return set ? std::atoi(set) : -1;
    }();
    return maxBatch < 0 || static_cast<int>(index) < maxBatch;
}

void M2Renderer::update(float deltaTime, const glm::vec3& cameraPos, const glm::mat4& viewProjection) {
    ZoneScopedN("M2Renderer::update");
    if (spatialIndexDirty_) {
        rebuildSpatialIndex();
    }

    float dtMs = deltaTime * 1000.0f;

    // Cache camera state for frustum-culling bone computation
    cachedCamPos_ = cameraPos;
    // Never past the ground. The density constants are how far models are
    // worth drawing, not how far there is anything to draw them on: the
    // terrain and the WMOs stop at the view distance itself, so a doodad
    // beyond it is a tree standing on nothing.
    const float maxRenderDistance = std::min(
        cappedViewDistance(),
        viewDistanceScale_ *
            ((instances.size() > rendering::M2_HIGH_DENSITY_INSTANCE_THRESHOLD)
                 ? rendering::M2_MAX_RENDER_DISTANCE_HIGH_DENSITY
                 : rendering::M2_MAX_RENDER_DISTANCE_LOW_DENSITY));
    cachedMaxRenderDistSq_ = maxRenderDistance * maxRenderDistance;

    // Build frustum for culling bones
    Frustum updateFrustum;
    updateFrustum.extractFromMatrix(viewProjection);

    // --- Normal M2 animation update ---
    // Advance animTime for ALL instances (needed for texture UV animation on static doodads).
    // This is a tight loop touching only one float per instance - no hash lookups.
    //
    // An instance the bone loop below does not step is wrapped here, at the
    // length of its own sequence. A texture track is sampled against this
    // time and holds its last key once the time runs past the end, so a
    // waterfall with no bones of its own scrolled once after it loaded and
    // then stood still, and one with emitters - wrapped at a fixed 3.3 s
    // below, whatever its sequence - stopped for the rest of each lap.
    // A game object in a WMO interior is lit by the floor under it as an
    // interior doodad is by its MODD colour: Shattrath's Terrace of Light
    // braziers (ANCIENT_D_STANDING_LIGHT.m2) take the purple of the floor
    // there, where the outdoor Light 458 drew them green. Asked again once
    // moved a quarter yard or a second on (a WMO loading in).
    // Its room is asked at the same moments (setRoomAtQuery).
    if (interiorLightQuery_ || roomAtQuery_) {
        for (auto& instance : instances) {
            if (!instance.isGameObject) continue;
            instance.interiorLightAge += deltaTime;
            const glm::vec3 moved = instance.position - instance.interiorLightQueriedAt;
            if (instance.interiorLightAge < 1.0f && glm::dot(moved, moved) <= 0.0625f) continue;
            instance.interiorLightAge = 0.0f;
            instance.interiorLightQueriedAt = instance.position;
            if (roomAtQuery_ && !instance.wmoDoodadPlaced) {
                const auto room = roomAtQuery_(instance.position);
                instance.wmoRoomInstance = room ? room->first : 0;
                if (room) instance.wmoRooms = std::move(room->second);
                else instance.wmoRooms.clear();
            }
            if (!interiorLightQuery_) continue;
            if (const auto light = interiorLightQuery_(instance.position)) {
                const auto rgb = glm::uvec3(glm::round(glm::clamp(*light, 0.0f, 1.0f) * 255.0f));
                instance.indoorLighting = 2u | (rgb.r << 8) | (rgb.g << 16) | (rgb.b << 24);
            } else {
                instance.indoorLighting = 0;
            }
        }
    }

    for (auto& instance : instances) {
        if (instance.externalPose) continue;  // its owner keeps its clock
        instance.animTime += dtMs * instance.animSpeed;
        instance.globalSequenceTime += dtMs;
        const bool steppedBelow = instance.cachedHasAnimation && !instance.cachedDisableAnimation;
        if (!steppedBelow && instance.animDuration > 0.0f &&
            instance.animTime >= instance.animDuration) {
            if (instance.holdAtEnd) {
                instance.animTime = instance.animDuration;
                instance.animSpeed = 0.0f;
            } else if (instance.playingVariation && instance.cachedModel &&
                       instance.idleSequenceIndex >= 0 &&
                       instance.idleSequenceIndex < static_cast<int>(instance.cachedModel->sequences.size())) {
                // A state effect without bone animation still goes on from
                // its Stand intro to its Hold loop. Fiery Aura 39839
                // (Fel_Immolate_State_Base: Stand 1.5 s emitting throughout,
                // Hold 4.5 s emitting its first 1.5 s) is lit 4.4 s after it
                // lands, dark 1.7 s, lit 2 s in the original; looping Stand
                // burned without a break.
                instance.animTime -= instance.animDuration;
                instance.playingVariation = false;
                instance.currentSequenceIndex = instance.idleSequenceIndex;
                instance.animDuration = static_cast<float>(
                    instance.cachedModel->sequences[instance.idleSequenceIndex].duration);
                if (instance.animDuration > 0.0f)
                    instance.animTime = std::fmod(instance.animTime, instance.animDuration);
            } else {
                instance.animTime = std::fmod(instance.animTime, instance.animDuration);
            }
        }
    }

    // The sky model's clock, when this is the renderer that draws one.
    //
    // A report of the sky "playing an animation that brightens and dims, faster
    // when walking" survived three fixes to what chooses the sky, and the
    // lighting diagnostic then showed every input to it holding still while it
    // happened - the zone, the volumes, the model and the target colour. So
    // what is moving is this, and the two things worth telling apart are
    // whether the instance is being rebuilt (animTime back to zero) and
    // whether the clock runs at wall speed (animTime should advance by dtMs and
    // by nothing else).
    //
    // Only this renderer has skyMode_, and only on a change worth seeing, so it
    // is quiet unless asked for with WOWEE_LOG_LEVEL=info.
    if (skyMode_ && !instances.empty()) {
        const auto& sky = instances.front();
        const bool restarted = sky.animTime < skyDiagAnimTime_;
        if (restarted || sky.id != skyDiagInstanceId_ ||
            sky.animTime - skyDiagAnimTime_ > 1000.0f) {
            LOG_INFO("skyM2: instance=", sky.id, " instances=", instances.size(),
                     " animTime=", sky.animTime, " duration=", sky.animDuration,
                     " gsTime=", sky.globalSequenceTime, " dtMs=", dtMs,
                     restarted ? " RESTARTED" : "");
            skyDiagInstanceId_ = sky.id;
            skyDiagAnimTime_ = sky.animTime;
        }
    }
    // Wrap animTime for particle-only instances so emission rate tracks keep looping.
    // 3333ms chosen as a safe wrap period: long enough to cover the longest known M2
    // particle emission cycle (~3s for torch/campfire effects) while preventing float
    // precision loss that accumulates over hours of runtime.
    static constexpr float kParticleWrapMs = 3333.0f;
    // Only for those with no sequence length to wrap at; the rest were
    // wrapped at their own above.
    for (size_t idx : particleOnlyInstanceIndices_) {
        if (idx >= instances.size()) continue;
        auto& instance = instances[idx];
        if (instance.animDuration > 0.0f) continue;
        // Use iterative subtraction instead of fmod() to preserve precision
        while (instance.animTime > kParticleWrapMs) {
            instance.animTime -= kParticleWrapMs;
        }
    }

    boneWorkIndices_.clear();
    boneWorkIndices_.reserve(animatedInstanceIndices_.size());

    // Update animated instances (full animation state + bone computation culling)
    // Note: animTime was already advanced by dtMs in the global loop above.
    // Here we apply the speed factor: subtract the base dtMs and add dtMs*speed.
    // Ground clutter stops being stepped once it is past the distance it draws
    // at. There are hundreds of tufts to a tile and each one plays a sequence of
    // its own, so this list is mostly grass that nothing can see; the sequences
    // loop, so one that resumes from a stale time is indistinguishable from one
    // that never stopped.
    const float clutterAnimCutoffSq = (groundDetailMaxDistance_ > 0.0f)
        ? (groundDetailMaxDistance_ * groundDetailMaxDistance_) : 0.0f;

    for (size_t idx : animatedInstanceIndices_) {
        if (idx >= instances.size()) continue;
        auto& instance = instances[idx];

        if (clutterAnimCutoffSq > 0.0f && instance.cachedIsGroundDetail) {
            const glm::vec3 toCam = instance.position - cachedCamPos_;
            if (glm::dot(toCam, toCam) > clutterAnimCutoffSq) continue;
        }


        // For animation looping/variation, we need the actual model data.
        if (!instance.cachedModel) continue;
        const M2ModelGPU& model = *instance.cachedModel;

        // Validate sequence index
        if (instance.currentSequenceIndex < 0 ||
            instance.currentSequenceIndex >= static_cast<int>(model.sequences.size())) {
            instance.currentSequenceIndex = 0;
            if (!model.sequences.empty()) {
                instance.animDuration = static_cast<float>(model.sequences[0].duration);
            }
        }

        // Handle animation looping / variation transitions
        if (instance.animDuration <= 0.0f && instance.cachedHasParticleEmitters) {
            instance.animDuration = rendering::M2_DEFAULT_PARTICLE_ANIM_MS;
        }
        if (instance.animDuration > 0.0f && instance.animTime >= instance.animDuration) {
            if (instance.holdAtEnd) {
                // Stay on the last frame. A door's open sequence ends with the
                // door open, and that is the pose the server is describing.
                instance.animTime = instance.animDuration;
                instance.animSpeed = 0.0f;
            } else if (instance.playingVariation) {
                instance.playingVariation = false;
                instance.currentSequenceIndex = instance.idleSequenceIndex;
                if (instance.idleSequenceIndex < static_cast<int>(model.sequences.size())) {
                    instance.animDuration = static_cast<float>(model.sequences[instance.idleSequenceIndex].duration);
                }
                instance.animTime = 0.0f;
                instance.variationTimer = randFloat(rendering::M2_LOOP_VARIATION_TIMER_MIN_MS, rendering::M2_LOOP_VARIATION_TIMER_MAX_MS);
            } else {
                // Use iterative subtraction instead of fmod() to preserve precision
                float duration = std::max(1.0f, instance.animDuration);
                while (instance.animTime >= duration) {
                    instance.animTime -= duration;
                }
            }
        }

        // Idle variation timer
        if (!instance.playingVariation && !instance.holdAtEnd && !instance.stateEffectAnimation &&
            model.idleVariationIndices.size() > 1) {
            instance.variationTimer -= dtMs;
            if (instance.variationTimer <= 0.0f) {
                int pick = static_cast<int>(randRange(static_cast<uint32_t>(model.idleVariationIndices.size())));
                int newSeq = model.idleVariationIndices[pick];
                if (newSeq != instance.currentSequenceIndex && newSeq < static_cast<int>(model.sequences.size())) {
                    instance.playingVariation = true;
                    instance.currentSequenceIndex = newSeq;
                    instance.animDuration = static_cast<float>(model.sequences[newSeq].duration);
                    instance.animTime = 0.0f;
                } else {
                    instance.variationTimer = randFloat(rendering::M2_IDLE_VARIATION_TIMER_MIN_MS, rendering::M2_IDLE_VARIATION_TIMER_MAX_MS);
                }
            }
        }

        // Frustum + distance cull: skip expensive bone computation for off-screen instances.
        // Both effectiveMaxDistSq and paddedRadius are precomputed per instance in
        // recomputeCachedCullFactors(); we only need the per-frame distance and frustum test.
        glm::vec3 toCam = instance.position - cachedCamPos_;
        float distSq = glm::dot(toCam, toCam);
        float effectiveMaxDistSq = maxDistSqOf(instance, cachedMaxRenderDistSq_);
        if (instance.cachedIsSkyBird) {
            constexpr float kBirdMaxDistSq =
                rendering::M2_SKY_BIRD_MAX_RENDER_DISTANCE *
                rendering::M2_SKY_BIRD_MAX_RENDER_DISTANCE;
            effectiveMaxDistSq = std::min(effectiveMaxDistSq, kBirdMaxDistSq);
        }
        if (distSq > effectiveMaxDistSq) continue;
        float paddedRadius = instance.cachedPaddedRadius;
        if (paddedRadius > 0.0f && !updateFrustum.intersectsSphere(instance.cachedCullCenter, paddedRadius)) continue;

        // Distance and frustum culling above limit work; visible meshes retain
        // a fresh pose every frame, including distant wings and machinery.
        boneWorkIndices_.push_back(idx);
    }

    // Compute bone matrices (expensive, parallel if enough work)
    const size_t animCount = boneWorkIndices_.size();
    if (animCount > 0) {
        static const size_t minParallelAnimInstances = std::max<size_t>(
            8, envSizeOrDefault("WOWEE_M2_ANIM_MT_MIN", 96));
        if (animCount < minParallelAnimInstances || numAnimThreads_ <= 1) {
            // Sequential - not enough work to justify thread overhead
            for (size_t i : boneWorkIndices_) {
                if (i >= instances.size()) continue;
                auto& inst = instances[i];
                if (!inst.cachedModel) continue;
                computeBoneMatrices(*inst.cachedModel, inst, &cachedCamPos_);
            }
        } else {
            // Parallel - dispatch across worker threads
            static const size_t minAnimWorkPerThread = std::max<size_t>(
                16, envSizeOrDefault("WOWEE_M2_ANIM_WORK_PER_THREAD", 64));
            const size_t maxUsefulThreads = std::max<size_t>(
                1, (animCount + minAnimWorkPerThread - 1) / minAnimWorkPerThread);
            const size_t numThreads = std::min(static_cast<size_t>(numAnimThreads_), maxUsefulThreads);
            if (numThreads <= 1) {
                for (size_t i : boneWorkIndices_) {
                    if (i >= instances.size()) continue;
                    auto& inst = instances[i];
                    if (!inst.cachedModel) continue;
                    computeBoneMatrices(*inst.cachedModel, inst, &cachedCamPos_);
                }
            } else {
                const size_t chunkSize = animCount / numThreads;
                const size_t remainder = animCount % numThreads;

                auto processRange = [this](size_t begin, size_t end) {
                    for (size_t j = begin; j < end; ++j) {
                        size_t idx = boneWorkIndices_[j];
                        if (idx >= instances.size()) continue;
                        auto& inst = instances[idx];
                        if (!inst.cachedModel) continue;
                        computeBoneMatrices(*inst.cachedModel, inst, &cachedCamPos_);
                    }
                };

                // Reuse persistent futures vector to avoid allocation
                animFutures_.clear();
                if (animFutures_.capacity() < numThreads) {
                    animFutures_.reserve(numThreads);
                }

                // Dispatch all but the last chunk to the shared pool; process the
                // last chunk on this thread so this call always makes progress even
                // when it itself runs on a pool worker (see ThreadPool docs).
                size_t start = 0;
                for (size_t t = 0; t + 1 < numThreads; ++t) {
                    size_t end = start + chunkSize + (t < remainder ? 1 : 0);
                    animFutures_.push_back(core::ThreadPool::frameWorkers().submit(
                        [processRange, start, end]() { processRange(start, end); }));
                    start = end;
                }
                processRange(start, animCount);

                for (auto& f : animFutures_) {
                    f.get();
                }
            }
        }
    }

    // Particle update (sequential - uses RNG, not thread-safe)
    // Only iterate instances that have particle emitters (pre-built list).
    retiredInstanceIds_.clear();
    for (size_t idx : particleInstanceIndices_) {
        if (idx >= instances.size()) continue;
        auto& instance = instances[idx];
        // Posed later this frame by its owner; see updateExternalPoseEffects.
        if (instance.externalPose) continue;
        // Distance cull: only update particles within visible range
        glm::vec3 toCam = instance.position - cachedCamPos_;
        float distSq = glm::dot(toCam, toCam);
        if (distSq > particleMaxDistSq(instance)) {
            if (instance.retiring) retiredInstanceIds_.push_back(instance.id);
            continue;
        }
        if (!instance.cachedModel) continue;
        // Static skeletons still anchor attached particles. They never enter
        // the animated-bone work list, so initialize their bind transforms here.
        if (!instance.cachedModel->bones.empty() && instance.boneMatrices.empty()) {
            computeBoneMatrices(*instance.cachedModel, instance, &cachedCamPos_);
        }
        updateParticles(instance, deltaTime);
        if (instance.retiring) {
            if (instance.particles.empty()) retiredInstanceIds_.push_back(instance.id);
            continue;
        }
        emitParticles(instance, *instance.cachedModel, deltaTime);
        if (!instance.cachedModel->ribbonEmitters.empty()) {
            updateRibbons(instance, *instance.cachedModel, deltaTime);
        }
    }
    for (uint32_t id : retiredInstanceIds_) removeInstance(id);

}

// Diagnostic: WOWEE_M2_NO_SKINNING=1 renders every M2 in its bind pose by
// telling the shader to ignore bones, separating a skinning artifact from one
// drawn by the particle or ribbon systems.
static const bool kM2NoSkinning = envFlagEnabled("WOWEE_M2_NO_SKINNING");

void M2Renderer::updateProjectedTextures(const ProjectedSurfaceQuery& surface) {
    if (!initialized_) return;
    const uint32_t frame = vkCtx_->getCurrentFrame();
    auto& draws = projectedDraws_[frame];
    draws.clear();
    ++projectedFrameSerial_;
    auto* dst = static_cast<projected_texture::Vertex*>(projectedVBMapped_[frame]);
    if (!dst) return;
    uint32_t written = 0;
    for (const auto& instance : instances) {
        const M2ModelGPU* model = instance.cachedModel;
        if (!model || !model->hasProjectedBatches || instance.retiring) continue;
        const bool animated = model->hasAnimation && !model->disableAnimation;
        if (animated && instance.boneMatrices.empty()) continue;
        for (size_t bi = 0; bi < model->batches.size(); ++bi) {
            const auto& batch = model->batches[bi];
            if (!batch.projected) continue;
            assert(batch.indexStart + 6 <= model->pickIndices.size());
            const auto order = projected_texture::orderQuadCorners(
                std::span<const uint16_t, 6>(model->pickIndices.data() + batch.indexStart, 6));
            std::array<projected_texture::Corner, 4> corners;
            bool degenerate = false;
            for (int k = 0; k < 4; ++k) {
                assert(order[k] < model->pickVertices.size());
                const auto& v = model->pickVertices[order[k]];
                glm::mat4 skin(1.0f);
                if (animated) {
                    // As m2.vert: weights over clamped bone indices (indices
                    // above 127 are clamped at upload).
                    skin = glm::mat4(0.0f);
                    const int boneCount = static_cast<int>(instance.boneMatrices.size());
                    for (int w = 0; w < 4; ++w) {
                        const int idx = std::clamp(static_cast<int>(std::min<uint8_t>(v.boneIndices[w], 127)),
                                                   0, boneCount - 1);
                        skin += instance.boneMatrices[static_cast<size_t>(idx)] * (v.boneWeights[w] / 255.0f);
                    }
                }
                const glm::mat4 toWorld = instance.modelMatrix * skin;
                corners[k].position = glm::vec3(toWorld * glm::vec4(v.position, 1.0f));
                corners[k].normal = glm::vec3(toWorld * glm::vec4(v.normal, 0.0f));
                corners[k].uv0 = v.texCoords[0];
                corners[k].uv1 = v.texCoords[1];
            }
            // A quad scaled to nothing (authored hidden phase) draws nothing.
            const glm::vec3 e1 = corners[1].position - corners[0].position;
            const glm::vec3 e2 = corners[3].position - corners[0].position;
            if (glm::length(glm::cross(e1, e2)) < 1e-6f) degenerate = true;
            if (degenerate) continue;

            const uint64_t key = (static_cast<uint64_t>(instance.id) << 16) | static_cast<uint64_t>(bi);
            auto& cache = projectedCache_[key];
            bool same = !cache.vertices.empty();
            for (int k = 0; k < 4 && same; ++k) {
                same = glm::all(glm::lessThan(glm::abs(cache.corners[k] - corners[k].position), glm::vec3(1e-4f)));
            }
            if (!same) {
                const glm::vec3 origin = glm::vec3(instance.modelMatrix[3]);
                projected_texture::buildGrid(corners, origin, surface, cache.vertices);
                for (int k = 0; k < 4; ++k) cache.corners[k] = corners[k].position;
                cache.origin = origin;
            }
            cache.lastFrameSerial = projectedFrameSerial_;
            const auto count = static_cast<uint32_t>(cache.vertices.size());
            if (written + count > MAX_PROJECTED_VERTS) {
                if (!warnedProjectedOverflow_) {
                    warnedProjectedOverflow_ = true;
                    LOG_WARNING("M2 projected textures: more than ", MAX_PROJECTED_VERTS,
                                " vertices in one frame; the rest are not drawn");
                }
                continue;
            }
            std::memcpy(dst + written, cache.vertices.data(), count * sizeof(projected_texture::Vertex));
            draws[key] = ProjectedDraw{written, count, cache.origin};
            written += count;
        }
    }
    if (written > 0) {
        vmaFlushAllocation(vkCtx_->getAllocator(), projectedVBAlloc_[frame], 0,
                           written * sizeof(projected_texture::Vertex));
    }
    // Instances that are gone, or whose quad vanished, drop their grids.
    std::erase_if(projectedCache_, [&](const auto& entry) {
        return entry.second.lastFrameSerial != projectedFrameSerial_;
    });
}

void M2Renderer::prepareRender(uint32_t frameIndex, const Camera& camera) {
    if (!initialized_ || instances.empty()) return;
    (void)camera;  // reserved for future frustum-based culling

    // --- Mega bone SSBO: assign ranges and upload all animated instance bones ---
    // Offset 0 is reserved as the identity/no-bones sentinel; animated instances
    // are packed after it at their own bone count, so a 300-bone creature gets
    // all 300 matrices instead of being truncated into a fixed stride and
    // reading into its neighbour's range.
    uint32_t nextOffset = 1;
    for (size_t idx : animatedInstanceIndices_) {
        if (idx >= instances.size()) continue;
        auto& instance = instances[idx];

        if (instance.boneMatrices.empty()) {
            instance.megaBoneOffset = 0;  // Use identity slot
            continue;
        }

        // A bone that has left the model behind.
        //
        // The vertex shader skins by these matrices, so one that translates far
        // outside the model's own bounds drags every vertex weighted to it out
        // with it. What that looks like on screen is not a bone problem: it is
        // a long flat triangle with the skin smeared across it, which reads as
        // a missing texture or a stray card. The Elemental Slave's white sheets
        // were reported three times as missing textures, and the texture paths,
        // the display skins, the particle emitters and the ribbons were all
        // measured and found correct before the geometry was suspected.
        //
        // Judged against the other bones, not against the model's origin, and
        // against the model's own bounding radius so a large creature is not
        // accused for being large. Measuring from the origin flags every
        // flying critter: a bird's flight path is authored into its root bone,
        // so the whole skeleton is sixteen yards out and nothing is stretched.
        // What a stretch looks like is one bone leaving the set behind.
        if (instance.cachedModel != nullptr) {
            const M2ModelGPU& m = *instance.cachedModel;
            const glm::vec3 extent = m.boundMax - m.boundMin;
            const float radius = 0.5f * glm::length(extent);
            // Examined once per model, not once per frame: this walks every
            // bone, and there are models with three hundred of them drawn
            // dozens at a time.
            static std::unordered_set<uint32_t> boneRangeChecked;
            if (radius > 0.01f && boneRangeChecked.size() < 64 &&
                boneRangeChecked.insert(instance.modelId).second) {
                glm::vec3 centre(0.0f);
                for (const auto& bone : instance.boneMatrices) centre += glm::vec3(bone[3]);
                centre /= static_cast<float>(instance.boneMatrices.size());

                const float limit = radius * 4.0f;
                for (size_t bi = 0; bi < instance.boneMatrices.size(); ++bi) {
                    const glm::vec3 t(instance.boneMatrices[bi][3]);
                    const float stray = glm::length(t - centre);
                    if (stray <= limit) continue;
                    LOG_WARNING("M2 '", m.name, "' bone ", bi, " of ",
                                instance.boneMatrices.size(), " sits ", stray,
                                " from where the rest of its skeleton is, in a model whose"
                                " bounding radius is ", radius,
                                " - anything weighted to it is drawn stretched");
                    break;
                }
            }
        }

        const uint32_t boneCount = static_cast<uint32_t>(instance.boneMatrices.size());
        if (boneCount > MEGA_BONE_MATRIX_CAPACITY - nextOffset) {
            instance.megaBoneOffset = 0;  // Overflow - use identity
            continue;
        }

        instance.megaBoneOffset = nextOffset;

        // Upload bone matrices to mega buffer - only when they were recomputed
        // since the last upload into this frame's buffer, or the instance's
        // slot moved (animated set changed). Most animated instances are
        // distance/frustum/frame-skip culled and keep their previous bones, so
        // skipping their memcpy avoids megabytes of redundant writes per frame.
        if (megaBoneMapped_[frameIndex] &&
            (instance.bonesDirty[frameIndex] ||
             instance.megaBoneUploadedSlot[frameIndex] != instance.megaBoneOffset)) {
            auto* dst = static_cast<glm::mat4*>(megaBoneMapped_[frameIndex]) + instance.megaBoneOffset;
            memcpy(dst, instance.boneMatrices.data(), boneCount * sizeof(glm::mat4));
            instance.bonesDirty[frameIndex] = false;
            instance.megaBoneUploadedSlot[frameIndex] = instance.megaBoneOffset;
        }

        nextOffset += boneCount;
    }
}

void M2Renderer::render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const Camera& camera,
                        VkCommandBuffer transparentCmd,
                        const std::function<void()>& afterOpaque,
                        const std::function<bool(float)>& beforeTransparentDepth) {
    if (instances.empty() || !opaquePipeline_) {
        if (afterOpaque) afterOpaque();
        if (beforeTransparentDepth) beforeTransparentDepth(std::numeric_limits<float>::infinity());
        return;
    }

    // Debug: log once when we start rendering
    static bool loggedOnce = false;
    if (!loggedOnce) {
        loggedOnce = true;
        LOG_INFO("M2 render: ", instances.size(), " instances, ", models.size(), " models");
    }

    // Periodic diagnostic: report render pipeline stats every 10 seconds
    static int diagCounter = 0;
    if (++diagCounter == 600) { // ~10s at 60fps
        diagCounter = 0;
        uint32_t totalValid = 0, totalAnimated = 0, totalBonesReady = 0, totalMegaBoneOk = 0;
        for (const auto& inst : instances) {
            if (inst.cachedIsValid) totalValid++;
            if (inst.cachedHasAnimation && !inst.cachedDisableAnimation) {
                totalAnimated++;
                if (!inst.boneMatrices.empty()) totalBonesReady++;
                if (inst.megaBoneOffset != 0) totalMegaBoneOk++;
            }
        }
        LOG_INFO("M2 diag: total=", instances.size(),
                 " valid=", totalValid,
                 " animated=", totalAnimated,
                 " bonesReady=", totalBonesReady,
                 " megaBoneOk=", totalMegaBoneOk,
                 " visible=", sortedVisible_.size(),
                 " draws=", lastDrawCallCount);
    }

    // Reuse persistent buffers (clear instead of reallocating)
    glowSprites_.clear();

    lastDrawCallCount = 0;

    // Culled here, against this frame's camera. A GPU cull dispatch used to
    // decide visibility, read back when its frame slot came round again: two
    // frames late. While the camera turned, small models entering the view
    // stayed culled for those frames - clutter blinked in at the leading edge,
    // half the in-view tufts it covered missing at a 10 fps turn - and only the
    // first 24576 instances were covered at all; the rest were culled here.
    const float targetRenderDist = viewDistanceScale_ *
        ((instances.size() > 2000) ? 300.0f
         : (instances.size() > 1000) ? 500.0f
                                     : 1000.0f);
    const float shrinkRate = 0.005f;
    const float growRate = 0.05f;
    const float blendRate = (targetRenderDist < smoothedRenderDist_) ? shrinkRate : growRate;
    smoothedRenderDist_ = glm::mix(smoothedRenderDist_, targetRenderDist, blendRate);
    const float maxRenderDistanceSq = smoothedRenderDist_ * smoothedRenderDist_;

    const uint32_t frameIndex = vkCtx_->getCurrentFrame();
    const float fadeStartFraction = 0.75f;
    const glm::vec3 camPos = camera.getPosition();

    // Where this pass's three milliseconds go.
    //
    // Recording the M2 secondary is the largest single piece of CPU work in
    // the frame - renderWorld waits on the slowest worker and this is always
    // it - and the stage around it cannot see whether that is the cull, the
    // sort or the recording itself.
    static const bool m2Profile = core::envFlagEnabled("WOWEE_FRAME_PROFILE", false);
    const auto m2T0 = std::chrono::steady_clock::now();

    // Build sorted visible instance list
    sortedVisible_.clear();
    transparentVisible_.clear();
    skyDiagDrawsOpaque_ = 0;
    skyDiagDrawsTransparent_ = 0;
    const size_t expectedVisible = std::min(instances.size() / 3, size_t(600));
    if (sortedVisible_.capacity() < expectedVisible) {
        sortedVisible_.reserve(expectedVisible);
    }
    if (transparentVisible_.capacity() < expectedVisible / 4)
        transparentVisible_.reserve(expectedVisible / 4);

    Frustum frustum;
    {
        const glm::mat4 vp = camera.getProjectionMatrix() * camera.getViewMatrix();
        frustum.extractFromMatrix(vp);
    }
    // Headroom for server objects, which draw to the view distance.
    const float maxPossibleDistSq = std::max(
        maxRenderDistanceSq * 4.0f,
        viewDistanceAbsolute_ * viewDistanceAbsolute_);

    const uint32_t totalInstances = static_cast<uint32_t>(instances.size());
    struct VisibleChunk {
        std::vector<VisibleEntry> opaque;
        std::vector<VisibleEntry> transparent;
    };

    // Visibility classification is independent per instance and was the
    // remaining monolithic M2 CPU pass. Split dense scenes across a few pool
    // workers; the caller handles the final chunk so nested use from the M2
    // render worker cannot deadlock the shared pool.
    const uint32_t chunkCount = totalInstances >= 2048
        ? std::min<uint32_t>(4, (totalInstances + 1023) / 1024)
        : 1;
    std::vector<VisibleChunk> chunks(chunkCount);
    auto classifyRange = [&](uint32_t chunk, uint32_t begin, uint32_t end) {
        auto& out = chunks[chunk];
        out.opaque.reserve((end - begin) / 3);
        out.transparent.reserve((end - begin) / 12);
        for (uint32_t i = begin; i < end; ++i) {
            const auto& instance = instances[i];
            if (outsideVisibleRegion(instance.position)) continue;
            float distSq;
            float effectiveMaxDistSq;

            // Server objects draw to the view distance instead of following the
            // ambient doodad distance down; it also feeds the fade curve below,
            // so a mailbox doesn't fade out at the doodad boundary either.
            const float instanceMaxDistSq = maxDistSqOf(instance, maxRenderDistanceSq);

            if (forceNoCull_) {
                if (!instance.cachedIsValid) continue;
                glm::vec3 toCam = instance.position - camPos;
                distSq = glm::dot(toCam, toCam);
                effectiveMaxDistSq = instanceMaxDistSq;
            } else {
                if (!instance.cachedIsValid || instance.cachedIsInvisibleTrap || instance.retiring) continue;
                glm::vec3 toCam = instance.position - camPos;
                distSq = glm::dot(toCam, toCam);
                if (distSq > maxPossibleDistSq) continue;
                effectiveMaxDistSq = instanceMaxDistSq;
                if (distSq > effectiveMaxDistSq) continue;
                float paddedRadius = instance.cachedPaddedRadius;
                if (paddedRadius > 0.0f && !frustum.intersectsSphere(instance.cachedCullCenter, paddedRadius)) continue;
            }

            if (instance.cachedIsSkyBird && instance.cachedHasAnimation && !instance.cachedDisableAnimation) {
                constexpr float kBirdMaxDistSq =
                    rendering::M2_SKY_BIRD_MAX_RENDER_DISTANCE *
                    rendering::M2_SKY_BIRD_MAX_RENDER_DISTANCE;
                effectiveMaxDistSq = std::min(effectiveMaxDistSq, kBirdMaxDistSq);
                if (distSq > effectiveMaxDistSq) continue;
            }
            // A doodad inside a room is drawn with that room.
            if (instance.wmoRoomInstance != 0 && wmoRoomDrawn_ &&
                !wmoRoomDrawn_(instance.wmoRoomInstance, instance.wmoRooms)) continue;

            VisibleEntry visible{.index = i, .modelId = instance.modelId, .distSq = distSq,
                                 .effectiveMaxDistSq = effectiveMaxDistSq,
                                 .groundDetail = instance.cachedIsGroundDetail};
            // A faded instance is drawn blended, all of it - see M2Instance::fade.
            const bool faded = instance.fade < 0.999f;
            if (!faded) out.opaque.push_back(visible);
            if (faded || (instance.cachedModel &&
                (instance.cachedModel->hasTransparentBatches || instance.cachedModel->isSpellEffect ||
                 instance.cachedModel->hasProjectedBatches))) {
                out.transparent.push_back(visible);
            }
        }
    };

    std::vector<std::future<void>> visibilityFutures;
    visibilityFutures.reserve(chunkCount > 0 ? chunkCount - 1 : 0);
    const uint32_t chunkSize = (totalInstances + chunkCount - 1) / chunkCount;
    for (uint32_t chunk = 0; chunk + 1 < chunkCount; ++chunk) {
        const uint32_t begin = chunk * chunkSize;
        const uint32_t end = std::min(totalInstances, begin + chunkSize);
        visibilityFutures.push_back(core::ThreadPool::frameWorkers().submit(
            [&, chunk, begin, end]() { classifyRange(chunk, begin, end); }));
    }
    const uint32_t lastChunk = chunkCount - 1;
    classifyRange(lastChunk, lastChunk * chunkSize, totalInstances);
    for (auto& future : visibilityFutures) future.get();

    for (auto& chunk : chunks) {
        sortedVisible_.insert(sortedVisible_.end(),
                              std::make_move_iterator(chunk.opaque.begin()),
                              std::make_move_iterator(chunk.opaque.end()));
        transparentVisible_.insert(transparentVisible_.end(),
                                   std::make_move_iterator(chunk.transparent.begin()),
                                   std::make_move_iterator(chunk.transparent.end()));
    }

    // The furthest doodad actually drawn, for the diagnostic that sets it
    // against the furthest terrain chunk. See Renderer::logViewDistanceDiag.
    furthestDrawnSq_ = 0.0f;
    for (const auto& e : sortedVisible_)
        if (e.distSq > furthestDrawnSq_) furthestDrawnSq_ = e.distSq;
    for (const auto& e : transparentVisible_)
        if (e.distSq > furthestDrawnSq_) furthestDrawnSq_ = e.distSq;

    // Whether the sky model survived culling this frame, when this is the
    // renderer that draws one.
    //
    // Reported as flickering while the camera turns and steady while it does
    // not, with everything upstream measured and holding still: the lighting
    // inputs, the model's clock, the frame time. What that leaves is the dome
    // being drawn on some frames and not others, and this says so in one line
    // per change rather than one per frame.
    // WOWEE_SKY_M2_OPAQUE_ONLY=1 drops the sky model's blended layers.
    //
    // The model is the culprit - the flicker goes with WOWEE_NO_SKY_M2 - and it
    // is not a small one: hellfireskybox.m2 carries 23 textures over 34 render
    // flags, with five transparency tracks and seven UV animations driven by
    // six global sequences. The brightening and dimming is those alpha tracks.
    // This says whether the flicker is in the blended layers or in the base the
    // opaque pass draws, which halves what is left to read.
    static const bool skyOpaqueOnly = std::getenv("WOWEE_SKY_M2_OPAQUE_ONLY") != nullptr;
    if (skyMode_ && skyOpaqueOnly) transparentVisible_.clear();

    if (skyMode_) {
        const bool drawn = !sortedVisible_.empty() || !transparentVisible_.empty();
        if (drawn != skyDiagWasDrawn_) {
            skyDiagWasDrawn_ = drawn;
            LOG_INFO("skyM2 cull: ", drawn ? "DRAWN" : "CULLED",
                     " opaque=", sortedVisible_.size(),
                     " transparent=", transparentVisible_.size(),
                     " instances=", instances.size());
        }
    }

    // Two-pass rendering: opaque/alpha-test first (depth write ON), then transparent/additive
    // (depth write OFF, sorted back-to-front) so transparent geometry composites correctly
    // against all opaque geometry rather than only against what was rendered before it.

    // Pass 1: sort by modelId for minimum buffer rebinds (opaque batches)
    //
    // Ordering the groups front-to-back was tried, on the theory that cutout
    // foliage cannot be rejected before it is shaded so the near trees should
    // lay depth down first. It moved nothing: doodads measured 14.66ms against
    // 16.55 and 13.38 either side of it, which is the middle of the spread.
    // What it did cost was a hash map over every visible instance, two index
    // vectors and a copy of the whole list, every frame, in the pass that
    // turned out to be the largest single piece of CPU work in the frame.
    //
    // Ground clutter is the exception. It blends by texture alpha and writes
    // depth (see the pipeline choice below), so where two tufts overlap the
    // one drawn first decides the pixel: its faint blade fringe is already in
    // the depth buffer and hides the blades of a tuft behind it drawn later.
    // Grouped by model alone, the order within a group was whatever
    // std::sort left among equal keys, and that changes with every change to
    // the visible set - every frame of a turn - so overlapping blades blinked
    // in and out (Stranglethorn's grass). Drawn last and
    // far to near, each tuft blends over everything behind it, the result
    // the blending asks for, and the order is a function of the camera alone.
    const auto m2T1 = std::chrono::steady_clock::now();
    std::sort(sortedVisible_.begin(), sortedVisible_.end(),
              [](const VisibleEntry& a, const VisibleEntry& b) {
                  if (a.groundDetail != b.groundDetail) return b.groundDetail;
                  if (!a.groundDetail) return a.modelId < b.modelId;
                  if (a.distSq != b.distSq) return a.distSq > b.distSq;
                  return a.index < b.index;
              });
    const auto m2T2 = std::chrono::steady_clock::now();
    // Reported at the end of the pass; see m2Profile above.
    struct M2Phases { double cull, sort, record; std::size_t visible, instances; };
    const auto sayPhases = [&](const M2Phases& p) {
        static auto lastSaid = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (now - lastSaid <= std::chrono::seconds(10)) return;
        lastSaid = now;
        LOG_WARNING("  m2 record: cull ", p.cull, "ms, sort ", p.sort,
                    "ms, draws ", p.record, "ms over ", p.visible,
                    " visible of ", p.instances, " instances");
    };

    uint32_t currentModelId = UINT32_MAX;
    const M2ModelGPU* currentModel = nullptr;
    bool currentModelValid = false;

    // State tracking
    VkPipeline currentPipeline = VK_NULL_HANDLE;
    VkDescriptorSet currentMaterialSet = VK_NULL_HANDLE;

    // Push constants now carry per-batch data only; per-instance data is in instance SSBO.
    struct M2PushConstants {
        int32_t texCoordSet;        // UV set index (0 or 1)
        int32_t drawMode;           // -1 sky, 0 placed model, 2 ground clutter
        int32_t instanceDataOffset; // Base index into instance SSBO for this draw group
        float modelHeight;          // The model's own height (model space)
        int32_t texCoordSet2;
    };
    auto secondTextureOffset = [](const M2ModelGPU& model, const M2ModelGPU::BatchGPU& batch,
                                  const M2Instance& instance) {
        // Static models may carry a zero transform combo without any tracks.
        if (model.textureTransforms.empty()) return glm::vec4(0.0f);
        if (batch.textureAnimIndex2 == 0xFFFF) return glm::vec4(0.0f);
        const auto lookup = batch.textureAnimIndex2;
        if (lookup >= model.textureTransformLookup.size()) return glm::vec4(0.0f);
        const auto index = model.textureTransformLookup[lookup];
        if (index == 0xFFFF) return glm::vec4(0.0f);
        if (index >= model.textureTransforms.size()) {
            LOG_ERROR("Invalid second texture transform in ", model.name,
                ": combo=", lookup, " transform=", index, " count=", model.textureTransforms.size());
            throw std::runtime_error("Invalid M2 texture transform");
        }
        const auto translation = m2_track::sampleVec3(model.textureTransforms[index].translation,
            instance.currentSequenceIndex, instance.animTime, instance.globalSequenceTime,
            model.globalSequenceDurations, glm::vec3(0.0f));
        return glm::vec4(translation.x, translation.y, 0.0f, 0.0f);
    };

    // Fill the model half of the push constants (m2_draw_mode.hpp).
    auto fillDrawMode = [](M2PushConstants& pc, const M2ModelGPU& mdl, bool sky) {
        const M2DrawMode draw = m2DrawModeFor(sky, mdl.isGroundDetail,
                                              mdl.boundMin.z, mdl.boundMax.z);
        pc.drawMode = draw.mode;
        pc.modelHeight = draw.modelHeight;
    };

    // Validate per-frame descriptor set before any Vulkan commands
    if (!perFrameSet) {
        LOG_ERROR("M2Renderer::render: perFrameSet is VK_NULL_HANDLE - skipping M2 render");
        if (afterOpaque) afterOpaque();
        if (beforeTransparentDepth) beforeTransparentDepth(std::numeric_limits<float>::infinity());
        return;
    }

    // Bind per-frame descriptor set (set 0) - shared across all draws
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout_, 0, 1, &perFrameSet, 0, nullptr);

    // Start with opaque pipeline
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, opaquePipeline_);
    currentPipeline = opaquePipeline_;

    // Bind dummy bone set (set 2) so non-animated draws have a valid binding.
    // Bind mega bone SSBO instead - all instances index into one buffer via boneBase.
    if (megaBoneSet_[frameIndex]) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLayout_, 2, 1, &megaBoneSet_[frameIndex], 0, nullptr);
    } else if (dummyBoneSet_) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLayout_, 2, 1, &dummyBoneSet_, 0, nullptr);
    }

    // Bind instance data SSBO (set 3) - per-instance transforms, fade, bones
    if (instanceSet_[frameIndex]) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLayout_, 3, 1, &instanceSet_[frameIndex], 0, nullptr);
    }

    // Reset instance SSBO write cursor for this frame
    instanceDataCount_ = 0;
    auto* instSSBO = static_cast<M2InstanceGPU*>(instanceMapped_[frameIndex]);

    // =====================================================================
    // Opaque pass - instanced draws grouped by (modelId, LOD)
    // =====================================================================
    // sortedVisible_ is already sorted by modelId so consecutive entries share
    // the same vertex/index buffer.  Within each model group we sub-group by
    // targetLOD to guarantee all instances in one vkCmdDrawIndexed use the
    // same batch set.  Per-instance data (model matrix, fade, bones) is
    // written to the instance SSBO; the shader reads it via gl_InstanceIndex.
    {
        struct PendingInstance {
            uint32_t instanceIdx;
            float fadeAlpha;
            bool useBones;
            uint16_t targetLOD;
        };
        std::vector<PendingInstance> pending;
        pending.reserve(128);

        size_t visStart = 0;
        while (visStart < sortedVisible_.size()) {
            // Find group of consecutive entries with same modelId
            uint32_t groupModelId = sortedVisible_[visStart].modelId;
            size_t groupEnd = visStart;
            while (groupEnd < sortedVisible_.size() && sortedVisible_[groupEnd].modelId == groupModelId)
                groupEnd++;

            // Pull the model through the first entry's instance.cachedModel pointer
            // (set at addInstance) instead of doing models.find(groupModelId) per group.
            const auto& firstEntry = sortedVisible_[visStart];
            if (firstEntry.index >= instances.size() || !instances[firstEntry.index].cachedModel) {
                visStart = groupEnd;
                continue;
            }
            const M2ModelGPU& model = *instances[firstEntry.index].cachedModel;
            if (skipGroundDetail_ && model.isGroundDetail) {
                visStart = groupEnd;
                continue;
            }
            if (!model.vertexBuffer || !model.indexBuffer) {
                visStart = groupEnd;
                continue;
            }

            bool modelNeedsAnimation = model.hasAnimation && !model.disableAnimation;

            // Collect per-instance data for this model group
            pending.clear();
            for (size_t vi = visStart; vi < groupEnd; vi++) {
                const auto& entry = sortedVisible_[vi];
                if (entry.index >= instances.size()) continue;
                auto& instance = instances[entry.index];

                // Distance-based fade alpha
                float fadeFrac = model.disableAnimation ? 0.55f : fadeStartFraction;
                float fadeStartDistSq = entry.effectiveMaxDistSq * fadeFrac * fadeFrac;
                float fadeAlpha = 1.0f;
                if (entry.distSq > fadeStartDistSq) {
                    fadeAlpha = std::clamp((entry.effectiveMaxDistSq - entry.distSq) /
                                          (entry.effectiveMaxDistSq - fadeStartDistSq), 0.0f, 1.0f);
                }
                // Ground detail used to be held at 0.82 here. This is the
                // opaque pass: nothing blended it, so it was a number with no
                // effect - and now that the cutout pipeline turns alpha into
                // coverage, keeping it would punch a fifth of the pixels out
                // of every tuft of grass in reach.
                float instanceFadeAlpha = fadeAlpha;

                // Bone readiness check
                if (modelNeedsAnimation && instance.boneMatrices.empty()) continue;
                bool needsBones = modelNeedsAnimation && !instance.boneMatrices.empty();
                if (needsBones && instance.megaBoneOffset == 0) continue;

                // LOD selection
                uint16_t desiredLOD = 0;
                if (entry.distSq > 150.0f * 150.0f) desiredLOD = 3;
                else if (entry.distSq > 80.0f * 80.0f) desiredLOD = 2;
                else if (entry.distSq > 40.0f * 40.0f) desiredLOD = 1;
                // Down to the nearest level the model actually has, not all
                // the way back to full detail. A model carrying 0 and 1 and
                // asked for 3 was drawn at 0 - the most expensive level there
                // is - at the distance where it mattered least.
                uint16_t targetLOD = desiredLOD;
                while (targetLOD > 0 && !(model.availableLODs & (1u << targetLOD))) --targetLOD;
                // Clutter draws every batch at every distance (see the batch
                // filter below); one level keeps its far-to-near order whole.
                if (model.isGroundDetail) targetLOD = 0;

                pending.push_back({.instanceIdx = entry.index, .fadeAlpha = instanceFadeAlpha, .useBones = needsBones, .targetLOD = targetLOD});
            }

            if (pending.empty()) { visStart = groupEnd; continue; }

            // Sort by targetLOD so each sub-group occupies a contiguous SSBO range.
            // Stable: the incoming order is the draw order clutter depends on.
            std::stable_sort(pending.begin(), pending.end(),
                      [](const PendingInstance& a, const PendingInstance& b) { return a.targetLOD < b.targetLOD; });

            // Bind vertex/index buffers once per model group
            VkDeviceSize vbOffset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &model.vertexBuffer, &vbOffset);
            vkCmdBindIndexBuffer(cmd, model.indexBuffer, 0, VK_INDEX_TYPE_UINT16);

            // Write base instance data to SSBO (uvOffset=0 - overridden for tex-anim batches)
            uint32_t baseSSBOOffset = instanceDataCount_;
            size_t writtenInstances = 0;
            for (const auto& p : pending) {
                if (instanceDataCount_ >= MAX_INSTANCE_DATA) break;
                auto& inst = instances[p.instanceIdx];
                auto& e = instSSBO[instanceDataCount_];
                e.model = inst.modelMatrix;
                e.uvOffset = glm::vec2(0.0f);
                e.uvOffset2 = glm::vec4(0.0f);
                e.fadeAlpha = p.fadeAlpha;
                e.useBones = (p.useBones && !kM2NoSkinning) ? 1 : 0;
                e.boneBase = p.useBones ? static_cast<int32_t>(inst.megaBoneOffset) : 0;
                e.boneCount = static_cast<int32_t>(inst.boneMatrices.size());
                e.highlight = inst.highlight;
                e.flags = static_cast<int32_t>(inst.indoorLighting);
                instanceDataCount_++;
                ++writtenInstances;
            }

            // Drop what did not fit. The loop above stops writing at the cap,
            // but the LOD sub-groups below are ranges over `pending` and were
            // still being drawn in full - groupSSBOOffset then runs past the
            // end of the buffer and the vertex shader reads instance data that
            // is not there. That is a real out-of-bounds read on the GPU, not a
            // missing model: it fires once per instance past the cap, hundreds
            // of times a frame, and the device is lost seconds later.
            //
            // Truncating is safe here precisely because `pending` was sorted by
            // LOD before the write: the sub-groups are contiguous and in the
            // same order, so cutting the tail cuts whole instances rather than
            // splitting a range.
            if (writtenInstances < pending.size()) {
                static bool warnedInstanceCap = false;
                if (!warnedInstanceCap) {
                    warnedInstanceCap = true;
                    LOG_WARNING("M2Renderer: instance buffer full at ", MAX_INSTANCE_DATA,
                                "; dropping ", pending.size() - writtenInstances,
                                " instances of '", model.name, "' this frame");
                }
                pending.resize(writtenInstances);
            }

            // Process LOD sub-groups within this model group
            size_t lodIdx = 0;
            while (lodIdx < pending.size()) {
                uint16_t lod = pending[lodIdx].targetLOD;
                size_t lodEnd = lodIdx + 1;
                while (lodEnd < pending.size() && pending[lodEnd].targetLOD == lod) lodEnd++;
                uint32_t groupSize = static_cast<uint32_t>(lodEnd - lodIdx);
                uint32_t groupSSBOOffset = baseSSBOOffset + static_cast<uint32_t>(lodIdx);

                // All instances in a draw share the front-face convention.
                // If one transform reflects or collapses the mesh, retain the
                // old two-sided path for the whole LOD group instead of
                // accidentally culling that instance's front faces.
                bool windingPreserved = true;
                if (!skyMode_ &&
                    (foliageCullOpaquePipeline_ || foliageCullCutoutPipeline_)) {
                    for (size_t j = lodIdx; j < lodEnd; ++j) {
                        if (!m2FoliageTransformPreservesWinding(
                                instances[pending[j].instanceIdx].modelMatrix)) {
                            windingPreserved = false;
                            break;
                        }
                    }
                }

                // What each batch of a named model does at draw time, once.
                //
                // WOWEE_M2_BATCH_DIAG says what a batch IS, at load. It could
                // not say whether the batch was drawn, and a tree reported as
                // missing a section of trunk is exactly that question: four
                // readings of the model data in a row said every batch was
                // present and correct, which it is, so the answer has to come
                // from the frame rather than from the file.
                static const std::string kDrawDiag = [] {
                    const char* v = std::getenv("WOWEE_M2_BATCH_DIAG");
                    std::string t = v ? v : "";
                    std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) {
                        return static_cast<char>(std::tolower(c));
                    });
                    return t;
                }();
                bool diagThisModel = false;
                if (!kDrawDiag.empty()) {
                    std::string lowerName = model.name;
                    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    diagThisModel = lowerName.find(kDrawDiag) != std::string::npos;
                }
                // WOWEE_M2_ONLY_BATCH=<n> draws just that batch of the named
                // model and hides the rest.
                //
                // Everything checkable has now been checked on these trees and
                // every answer came back correct: the trunk mesh is continuous
                // from root to tip, its submesh level matches, its texture
                // loads, its mips preserve coverage, and the draw diagnostic
                // says every batch reaches the frame. A section still looks
                // missing. Seeing the trunk by itself is the difference
                // between the trunk being wrong and the canopy eating it, and
                // no amount of reading the files can tell those apart.
                static const int kOnlyBatch = [] {
                    const char* v = std::getenv("WOWEE_M2_ONLY_BATCH");
                    return (v && *v) ? std::atoi(v) : -1;
                }();
                for (size_t bi = 0; bi < model.batches.size(); bi++) {
                    const auto& batch = model.batches[bi];
                    const char* skipped = nullptr;
                    if (kOnlyBatch >= 0 && diagThisModel &&
                        bi != static_cast<size_t>(kOnlyBatch)) {
                        skipped = "WOWEE_M2_ONLY_BATCH";
                    }
                    else if (batch.indexCount == 0) skipped = "no indices";
                    else if (!model.isGroundDetail && batch.submeshLevel != lod)
                        skipped = "submeshLevel is not this LOD";
                    else if (batch.batchOpacity < 0.01f) skipped = "opacity is zero";
                    else if (!skyBatchAllowed(skyMode_, bi)) skipped = "sky batch rule";
                    else if (suppressBakedStars_ && batch.starLayer) skipped = "baked star layer";
                    if (diagThisModel) {
                        static std::set<std::pair<const void*, size_t>> saidDraw;
                        if (saidDraw.size() < 64 && saidDraw.insert({&model, bi}).second) {
                            LOG_WARNING("M2 DRAW '", model.name, "' batch ", bi,
                                        " lod=", lod, " submeshLevel=", batch.submeshLevel,
                                        " idx=", batch.indexCount,
                                        " opacity=", batch.batchOpacity,
                                        (skipped ? "  SKIPPED: " : "  drawn"),
                                        (skipped ? skipped : ""));
                        }
                    }
                    if (skipped) continue;
                    const bool batchUnlit = (batch.materialFlags & 0x01) != 0;
                    M2GlowCardBatch glowCard;
                    glowCard.glowSize = batch.glowSize;
                    glowCard.blendMode = batch.blendMode;
                    glowCard.lanternGlowHint = batch.lanternGlowHint;
                    glowCard.glowCardLike = batch.glowCardLike;
                    glowCard.unlit = batchUnlit;
                    glowCard.preserveGlowMesh = batch.preserveGlowMesh;
                    glowCard.modelIsElvenLike = model.isElvenLike;
                    glowCard.modelIsLanternLike = model.isLanternLike;
                    glowCard.modelIsTorch = model.isTorch;
                    glowCard.modelIsBrazierOrFire = model.isBrazierOrFire;
                    glowCard.modelIsSpellEffect = model.isSpellEffect;
                    glowCard.modelIsKoboldFlame = model.isKoboldFlame;
                    const bool shouldUseGlowSprite = !game::isActiveExpansion("tbc") && m2WantsGlowSprite(glowCard);
                    if (shouldUseGlowSprite) {
                        // Generate glow sprites for each instance in the group
                        for (size_t j = lodIdx; j < lodEnd; j++) {
                            auto& inst = instances[pending[j].instanceIdx];
                            glm::vec3 worldPos;
                            if (model.isGroundFire &&
                                !model.particleEmitters.empty()) {
                                worldPos = glm::vec3(std::numeric_limits<float>::max());
                                for (const auto& emitter : model.particleEmitters) {
                                    glm::mat4 boneXform(1.0f);
                                    if (emitter.bone < inst.boneMatrices.size()) {
                                        boneXform = inst.boneMatrices[emitter.bone];
                                    }
                                    const glm::vec3 emitterWorld = glm::vec3(
                                        inst.modelMatrix * boneXform * glm::vec4(emitter.position, 1.0f));
                                    if (emitterWorld.z < worldPos.z) worldPos = emitterWorld;
                                }
                            } else {
                                worldPos = animatedBatchWorldCenter(inst, batch);
                            }
                            // Preserved emissive glass writes opaque depth before
                            // this additive point sprite. Move only the visual
                            // halo just beyond the camera-facing glass surface so
                            // depth testing does not reject it; the associated
                            // local light remains at the true batch center.
                            if (batch.preserveGlowMesh) {
                                const glm::vec3 towardCamera = camPos - worldPos;
                                const float lenSq = glm::dot(towardCamera, towardCamera);
                                if (lenSq > 0.0001f) {
                                    worldPos += towardCamera * glm::inversesqrt(lenSq) *
                                        (batch.glowSize * inst.scale * 1.25f);
                                }
                            }
                            GlowSprite gs;
                            gs.worldPos = worldPos;
                            if (batch.glowTint == 1 || model.isElvenLike)
                                gs.color = glm::vec4(0.48f, 0.72f, 1.0f, 1.05f);
                            else if (batch.glowTint == 2)
                                gs.color = glm::vec4(1.0f, 0.28f, 0.22f, 1.10f);
                            else
                                gs.color = glm::vec4(1.0f, 0.82f, 0.46f, 1.15f);
                            // Match the parent M2's distance fade instead of a separate
                            // hard 180-unit cutoff, which made tunnel lights pop on.
                            gs.color.a *= pending[j].fadeAlpha;
                            gs.size = batch.glowSize * inst.scale *
                                (batch.preserveGlowMesh ? 2.0f : 1.45f);
                            if (batch.preserveGlowMesh) gs.color.a *= 1.25f;

                            // A fixture with real particle flames should read as
                            // flames with a halo behind them. The sprite is sized
                            // from its glow card's geometric radius, which on a
                            // chandelier spans the whole fixture - an additive
                            // blob about a unit across, against candle flames of
                            // 0.15, so the glow swallowed them entirely. Cap it
                            // just above what a small glow card already produces
                            // (the 0.5 floor times 1.45), so candles, lanterns
                            // and torches are untouched and only oversized cards
                            // are clamped.
                            if (!model.particleEmitters.empty() && model.isLanternLike) {
                                constexpr float kMaxHaloRadius = 0.75f;
                                gs.size = std::min(gs.size, kMaxHaloRadius * inst.scale);
                            }

                            // Fire burning inside a hearth. The sprite is a point
                            // billboard carrying one depth value for the whole
                            // quad, so as soon as its centre shows through the
                            // fireplace opening the entire square draws - brick
                            // surround included, which reads as the fire glowing
                            // through the masonry. Sized from the glow card's
                            // geometric radius these spheres are wider than the
                            // opening, so keep them inside it.
                            const bool hearthFire = model.isBrazierOrFire ||
                                                    model.isGroundFire ||
                                                    model.isForge;
                            if (hearthFire) {
                                constexpr float kMaxFireGlowRadius = 0.5f;
                                gs.size = std::min(gs.size, kMaxFireGlowRadius * inst.scale);
                            }

                            // Flame guttering. The phase comes from the lamp's own
                            // world position, so two lanterns on the same street
                            // never pulse together - a synchronised row of lamps
                            // reads as a rendering artifact, not firelight. Two
                            // detuned sines keep any single lamp from looping
                            // visibly. Alpha and size move together, since a
                            // brighter flame also looks slightly larger.
                            {
                                // Matches the phase used for this lamp's local
                                // light, so the sprite and the pool of light it
                                // casts breathe together.
                                // Same clock and parameters as the local light in
                                // gatherLocalLights, so the sprite and the pool of
                                // light it casts rise and fall together.
                                const float flicker = lampFlicker(
                                    inst.position, lampFlickerClockSeconds(),
                                    0.82f, 0.12f, 0.06f);
                                gs.color.a *= flicker;
                                gs.size    *= 0.98f + 0.02f * flicker;
                            }
                            glowSprites_.push_back(gs);
                            GlowSprite halo = gs;
                            halo.color.a *= batch.preserveGlowMesh ? 0.34f : 0.42f;
                            halo.size *= batch.preserveGlowMesh ? 2.2f : 1.8f;
                            // The halo is nearly twice the sprite, so capping only
                            // the sprite would leave the bleed to the halo.
                            if (hearthFire) {
                                constexpr float kMaxFireHaloRadius = 0.85f;
                                halo.size = std::min(halo.size, kMaxFireHaloRadius * inst.scale);
                            }
                            glowSprites_.push_back(halo);
                        }
                        if (m2GlowSpriteReplacesMesh(glowCard)) continue;
                    }

                    // Opaque gate - transparent glow cards were handled above so their
                    // sprites are generated before the mesh moves to pass 2.
                    const bool rawTransparent = m2BatchIsTransparent(
                        batch.blendMode, model.isSpellEffect, batch.hasAuthoredMaterial);
                    if (rawTransparent || batch.projected) continue;


                    // Handle texture animation: if this batch has per-instance uvOffset,
                    // write a separate SSBO range with the correct offsets.
                    bool hasBatchTexAnim = (batch.textureAnimIndex != 0xFFFF && model.hasTextureAnimation)
                                           ||
                                           !batch.animatedColorAlpha.sequences.empty() ||
                                           !batch.animatedTextureWeight.sequences.empty();
                    uint32_t drawOffset = groupSSBOOffset;
                    if (hasBatchTexAnim && instanceDataCount_ + groupSize <= MAX_INSTANCE_DATA) {
                        drawOffset = instanceDataCount_;
                        // Hoist per-batch lookups: the transform pointer is fixed for
                        // every instance in this group; only the sampled translation
                        // varies (per-instance animTime).
                        const pipeline::M2TextureTransform* tt = nullptr;
                        if (batch.textureAnimIndex != 0xFFFF && model.hasTextureAnimation) {
                            uint16_t lookupIdx = batch.textureAnimIndex;
                            if (lookupIdx < model.textureTransformLookup.size()) {
                                uint16_t transformIdx = model.textureTransformLookup[lookupIdx];
                                if (transformIdx < model.textureTransforms.size()) {
                                    tt = &model.textureTransforms[transformIdx];
                                }
                            }
                        }
                        for (size_t j = lodIdx; j < lodEnd; j++) {
                            const auto& p = pending[j];
                            auto& inst = instances[p.instanceIdx];
                            glm::vec2 uvOffset(0.0f);
                            if (tt) {
                                glm::vec3 trans = m2_track::sampleVec3(
                                    tt->translation, inst.currentSequenceIndex,
                                    inst.animTime, inst.globalSequenceTime,
                                    model.globalSequenceDurations, glm::vec3(0.0f));
                                uvOffset = glm::vec2(trans.x, trans.y);
                            }
                            // Rebuild the entry from CPU-side data rather than copying it
                            // out of the base entry. instSSBO lives in write-combined
                            // upload memory: writing it is cheap, but reading it back is
                            // uncached and costs microseconds per instance.
                            auto& e = instSSBO[instanceDataCount_];
                            e.model = inst.modelMatrix;
                            e.uvOffset = uvOffset;
                            e.uvOffset2 = secondTextureOffset(model, batch, inst);
                            e.fadeAlpha = p.fadeAlpha * m2_internal::animatedBatchOpacity(model, batch, inst);
                            e.useBones = (p.useBones && !kM2NoSkinning) ? 1 : 0;
                            e.boneBase = p.useBones ? static_cast<int32_t>(inst.megaBoneOffset) : 0;
                            e.boneCount = static_cast<int32_t>(inst.boneMatrices.size());
                            e.highlight = inst.highlight;
                            e.flags = static_cast<int32_t>(inst.indoorLighting);
                            instanceDataCount_++;
                        }
                    }

                    // Pipeline selection (per-model/batch, not per-instance)
                    // A batch the artist marked opaque is not asking for
                    // any of this. Blend mode 0 means the alpha channel goes
                    // unread, and M2 textures are routinely atlases whose
                    // alpha belongs to another layer or another model - so
                    // keying on it eats surfaces that were painted solid.
                    //
                    // Silverpine's trees are the case that found it. Their
                    // trunk is one opaque batch sampling the right-hand panel
                    // of SilverPineTree01TrunkSkin, and that panel carries a
                    // stale alpha hole across its middle - 24% of the texels
                    // the trunk's own triangles cover, with unbroken bark
                    // painted underneath. Forced to the cutout pipeline, where
                    // alpha becomes coverage, the trunk lost its midsection
                    // and left the canopy floating over the stump. 133 foliage
                    // models were holed the same way - Tirisfal's canopytree07,
                    // the Plaguelands and Stonetalon pines, Eversong and
                    // Silvermoon, Nagrand, Crystalsong, Ruby Sanctum, the
                    // Storm Peaks, and every Zangarmarsh mushroom cap.
                    //
                    // Where the alpha really is the silhouette - a card drawn
                    // on a black backing, or DXT1 punch-through, which stores
                    // no colour under a transparent texel at all - the opaque
                    // batch is cut out, whatever the model is called. 2.4.3
                    // cuts Mulgore's thorn arches (MullgoreThorn01-07, one
                    // opaque batch on DXT1 AlteracTreeTrunk03) along the
                    // texture's ragged edge. Measured from the texture by
                    // alphaIsSilhouette.
                    const bool opaqueByMaterial = batch.blendMode == M2_BLEND_OPAQUE;
                    const bool silhouetteCutout = !model.isSpellEffect && opaqueByMaterial &&
                                                  batch.alphaIsSilhouette;
                    // A batch the artist marked additive is already doing
                    // what the cutout and the colour key are approximations
                    // of: black adds nothing, so it disappears on its own.
                    // Forcing it opaque and then keying the black out leaves
                    // the bright middle of a glow card as a solid disc, which
                    // is what Orgrimmar's bonfires were.
                    const bool forceCutout =
                        !model.isSpellEffect &&
                        !m2BlendIsAdditive(batch.blendMode) &&
                        (model.isGroundDetail || silhouetteCutout ||
                         m2BatchNeedsAlphaTest(batch.blendMode, batch.hasAlpha));

                    uint8_t effectiveBlendMode = batch.blendMode;
                    if (model.isSpellEffect && !batch.hasAuthoredMaterial) {
                        if (effectiveBlendMode <= 1) effectiveBlendMode = 3;
                        else if (effectiveBlendMode == 4 || effectiveBlendMode == 5) effectiveBlendMode = 3;
                    }
                    if (forceCutout) effectiveBlendMode = 1;

                    VkPipeline desiredPipeline;
                    if (forceCutout && model.isGroundDetail) {
                        // Blended, depth written: m2.frag keeps clutter's
                        // texture alpha (alphaTest 3) for its soft edges.
                        desiredPipeline = alphaTestPipeline_;
                    } else if (forceCutout) {
                        desiredPipeline = cutoutPipeline_;
                    } else {
                        switch (effectiveBlendMode) {
                            case 0: desiredPipeline = opaquePipeline_; break;
                            case 1: desiredPipeline = alphaTestPipeline_; break;
                            case 2: desiredPipeline = alphaPipeline_; break;
                            default: desiredPipeline = additivePipeline_; break;
                        }
                    }
                    if (m2UseAuthoredCull(
                            foliageCullOpaquePipeline_ || foliageCullCutoutPipeline_,
                            skyMode_, batch.hasAuthoredMaterial,
                            windingPreserved, batch.materialFlags)) {
                        desiredPipeline = culledPipeline(desiredPipeline);
                    }
                    if (desiredPipeline != currentPipeline) {
                        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, desiredPipeline);
                        currentPipeline = desiredPipeline;
                    }

                    // Update material UBO
                    if (batch.materialUBOMapped) {
                        auto* mat = static_cast<M2MaterialUBO*>(batch.materialUBOMapped);
                        // interiorDarken is a camera-based flag - it darkens ALL M2s (incl.
                        // outdoor trees) when the camera is inside a WMO.  Disable it; indoor
                        // M2s already look correct from the darker ambient/lighting.
                        mat->interiorDarken = 0.0f;
                        if (forceCutout) {
                            mat->alphaTest = model.isGroundDetail ? 3 : 1;
                            if (model.isGroundDetail) mat->unlit = 0;
                        }
                    }

                    // Bind material descriptor set (set 1)
                    if (!batch.materialSet) continue;
                    if (batch.materialSet != currentMaterialSet) {
                        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                pipelineLayout_, 1, 1, &batch.materialSet, 0, nullptr);
                        currentMaterialSet = batch.materialSet;
                    }

                    // Push constants + instanced draw
                    M2PushConstants pc;
                    pc.texCoordSet = static_cast<int32_t>(batch.textureUnit);
                    pc.texCoordSet2 = batch.textureUnit2;
                    fillDrawMode(pc, model, skyMode_);
                    pc.instanceDataOffset = static_cast<int32_t>(drawOffset);
                    vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
                    vkCmdDrawIndexed(cmd, batch.indexCount, groupSize, batch.indexStart, 0, 0);
                    if (skyMode_) ++skyDiagDrawsOpaque_;
                    lastDrawCallCount++;
                }

                lodIdx = lodEnd;
            }

            visStart = groupEnd;
        }
    }

    // =====================================================================
    // Pass 2: Transparent/additive batches - back-to-front per instance
    // =====================================================================
    // Transparent geometry must be drawn individually per instance in back-to-
    // front order for correct alpha compositing.  Each draw writes one
    // M2InstanceGPU entry and issues a single-instance indexed draw.
    if (afterOpaque) afterOpaque();
    if (transparentCmd) cmd = transparentCmd;
    // The boundary may switch buffers or record another renderer. Restore all
    // shared bindings; per-model and material bindings are reset below.
    auto restoreTransparentBindings = [&]() {
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout_, 0, 1, &perFrameSet, 0, nullptr);
    const VkDescriptorSet boneSet = megaBoneSet_[frameIndex] ? megaBoneSet_[frameIndex] : dummyBoneSet_;
    if (boneSet) vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout_, 2, 1, &boneSet, 0, nullptr);
    if (instanceSet_[frameIndex]) vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout_, 3, 1, &instanceSet_[frameIndex], 0, nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, opaquePipeline_);
    currentModelId = UINT32_MAX;
    currentModel = nullptr;
    currentModelValid = false;
    currentPipeline = opaquePipeline_;
    currentMaterialSet = VK_NULL_HANDLE;
    };
    restoreTransparentBindings();
    const auto viewDepth = [&](const VisibleEntry& entry) {
        return (camera.getViewMatrix() * glm::vec4(instances[entry.index].position, 1.0f)).z;
    };
    std::sort(transparentVisible_.begin(), transparentVisible_.end(),
              [&](const VisibleEntry& a, const VisibleEntry& b) { return viewDepth(a) < viewDepth(b); });

    currentModelId = UINT32_MAX;
    currentModel = nullptr;
    currentModelValid = false;
    currentPipeline = opaquePipeline_;
    currentMaterialSet = VK_NULL_HANDLE;

    for (const auto& entry : transparentVisible_) {
        if (entry.index >= instances.size()) continue;
        if (beforeTransparentDepth && beforeTransparentDepth(viewDepth(entry)))
            restoreTransparentBindings();
        auto& instance = instances[entry.index];

        // Model boundary: read cachedModel off the instance - was doing a
        // per-boundary models.find() even though every instance already has
        // the pointer cached at addInstance time.
        if (entry.modelId != currentModelId) {
            currentModelId = entry.modelId;
            currentModelValid = false;
            currentModel = instance.cachedModel;
            if (!currentModel) continue;
            if (!currentModel->hasTransparentBatches && !currentModel->isSpellEffect && !currentModel->hasProjectedBatches) continue;
            if (!currentModel->vertexBuffer || !currentModel->indexBuffer) continue;
            currentModelValid = true;
            VkDeviceSize vbOff = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &currentModel->vertexBuffer, &vbOff);
            vkCmdBindIndexBuffer(cmd, currentModel->indexBuffer, 0, VK_INDEX_TYPE_UINT16);
        }
        if (!currentModelValid) continue;

        const M2ModelGPU& model = *currentModel;

        // Fade alpha
        float fadeAlpha = 1.0f;
        float fadeFrac = model.disableAnimation ? 0.55f : fadeStartFraction;
        float fadeStartDistSq = entry.effectiveMaxDistSq * fadeFrac * fadeFrac;
        if (entry.distSq > fadeStartDistSq) {
            fadeAlpha = std::clamp((entry.effectiveMaxDistSq - entry.distSq) /
                                  (entry.effectiveMaxDistSq - fadeStartDistSq), 0.0f, 1.0f);
        }
        float instanceFadeAlpha = fadeAlpha * instance.fade;
        const bool instanceFaded = instance.fade < 0.999f;

        bool modelNeedsAnimation = model.hasAnimation && !model.disableAnimation;
        if (modelNeedsAnimation && instance.boneMatrices.empty()) continue;
        bool needsBones = modelNeedsAnimation && !instance.boneMatrices.empty();
        if (needsBones && instance.megaBoneOffset == 0) continue;

        uint16_t desiredLOD = 0;
        if (entry.distSq > 150.0f * 150.0f) desiredLOD = 3;
        else if (entry.distSq > 80.0f * 80.0f) desiredLOD = 2;
        else if (entry.distSq > 40.0f * 40.0f) desiredLOD = 1;
        // The nearest level the model has; see the note in the batched path.
        uint16_t targetLOD = desiredLOD;
        while (targetLOD > 0 && !(model.availableLODs & (1u << targetLOD))) --targetLOD;


        for (const auto& batch : model.batches) {
            if (batch.indexCount == 0) continue;
            if (!model.isGroundDetail && batch.submeshLevel != targetLOD) continue;
            if (batch.batchOpacity < 0.01f) continue;
            if (!skyBatchAllowed(skyMode_, static_cast<std::size_t>(&batch - model.batches.data()))) continue;

            // Pass 2 gate: only transparent/additive batches - or, for a faded
            // instance, every batch, since the opaque pass left it out.
            // Projected textures all draw here, over the opaque ground.
            {
                const bool rawTransparent = m2BatchIsTransparent(
                    batch.blendMode, model.isSpellEffect, batch.hasAuthoredMaterial) || instanceFaded;
                if (!rawTransparent && !batch.projected) continue;
            }
            const ProjectedDraw* projectedDraw = nullptr;
            if (batch.projected) {
                const uint64_t key = (static_cast<uint64_t>(instance.id) << 16) |
                    static_cast<uint64_t>(&batch - model.batches.data());
                const auto& draws = projectedDraws_[vkCtx_->getCurrentFrame()];
                const auto it = draws.find(key);
                if (it == draws.end()) continue;
                projectedDraw = &it->second;
            }

            // WOWEE_SKY_M2_SKIP_BLEND=<n> drops the sky's layers of one blend
            // mode. Its thirty-four batches are six opaque, ten alpha-blended
            // (2) and eighteen additive (4), and the two behave differently in
            // a way that matters: additive is order-independent and alpha
            // blending is not. Dropping ten or dropping eighteen says which
            // half the flicker lives in, the same way NO_SKY_M2 and
            // OPAQUE_ONLY narrowed it to the blended layers at all.
            if (skyMode_) {
                static const int skipBlend = [] {
                    const char* set = std::getenv("WOWEE_SKY_M2_SKIP_BLEND");
                    return set ? std::atoi(set) : -1;
                }();
                if (skipBlend >= 0 && batch.blendMode == skipBlend) continue;
            }

            // Skip glow sprites (handled in opaque pass)
            const bool batchUnlit = (batch.materialFlags & 0x01) != 0;
            M2GlowCardBatch glowCard;
            glowCard.glowSize = batch.glowSize;
            glowCard.blendMode = batch.blendMode;
            glowCard.lanternGlowHint = batch.lanternGlowHint;
            glowCard.glowCardLike = batch.glowCardLike;
            glowCard.unlit = batchUnlit;
            glowCard.preserveGlowMesh = batch.preserveGlowMesh;
            glowCard.modelIsElvenLike = model.isElvenLike;
            glowCard.modelIsLanternLike = model.isLanternLike;
            glowCard.modelIsTorch = model.isTorch;
            glowCard.modelIsBrazierOrFire = model.isBrazierOrFire;
            glowCard.modelIsSpellEffect = model.isSpellEffect;
            glowCard.modelIsKoboldFlame = model.isKoboldFlame;
            if (!game::isActiveExpansion("tbc") && m2WantsGlowSprite(glowCard) && m2GlowSpriteReplacesMesh(glowCard)) {
                continue;
            }


            // Compute UV offset for this instance + batch
            //
            // WOWEE_SKY_M2_NO_TEXANIM=1 freezes the sky's scrolling. Nine of
            // its eighteen additive layers carry a texture animation, and
            // those offsets are the one thing about those layers that changes
            // between frames at all - so if the flicker survives freezing them
            // it is not in what the layers sample, it is in the geometry they
            // are drawn on.
            static const bool skyNoTexAnim =
                std::getenv("WOWEE_SKY_M2_NO_TEXANIM") != nullptr;
            glm::vec2 uvOffset(0.0f);
            if (batch.textureAnimIndex != 0xFFFF && model.hasTextureAnimation &&
                !(skyMode_ && skyNoTexAnim)) {
                uint16_t lookupIdx = batch.textureAnimIndex;
                if (lookupIdx < model.textureTransformLookup.size()) {
                    uint16_t transformIdx = model.textureTransformLookup[lookupIdx];
                    if (transformIdx < model.textureTransforms.size()) {
                        const auto& tt = model.textureTransforms[transformIdx];
                        glm::vec3 trans = m2_track::sampleVec3(
                            tt.translation, instance.currentSequenceIndex,
                            instance.animTime, instance.globalSequenceTime,
                            model.globalSequenceDurations, glm::vec3(0.0f));
                        uvOffset = glm::vec2(trans.x, trans.y);
                    }
                }
            }

            // Write single instance entry to SSBO
            if (instanceDataCount_ >= MAX_INSTANCE_DATA) continue;
            uint32_t drawOffset = instanceDataCount_;
            auto& e = instSSBO[instanceDataCount_];
            e.model = instance.modelMatrix;
            e.uvOffset = uvOffset;
            e.uvOffset2 = secondTextureOffset(model, batch, instance);
            e.fadeAlpha = instanceFadeAlpha * m2_internal::animatedBatchOpacity(model, batch, instance);
            e.useBones = (needsBones && !kM2NoSkinning) ? 1 : 0;
            e.boneBase = needsBones ? static_cast<int32_t>(instance.megaBoneOffset) : 0;
            e.boneCount = static_cast<int32_t>(instance.boneMatrices.size());
            e.highlight = instance.highlight;
            e.flags = static_cast<int32_t>(instance.indoorLighting);
            if (projectedDraw) {
                // The grid is already in place in the world, relative to
                // the instance origin.
                e.model = glm::translate(glm::mat4(1.0f), projectedDraw->origin);
                e.useBones = 0;
                e.boneBase = 0;
            }
            instanceDataCount_++;

            // Pipeline selection
            uint8_t effectiveBlendMode = batch.blendMode;
            if (model.isSpellEffect && !batch.hasAuthoredMaterial) {
                if (effectiveBlendMode <= 1) effectiveBlendMode = 3;
                else if (effectiveBlendMode == 4 || effectiveBlendMode == 5) effectiveBlendMode = 3;
            }

            VkPipeline desiredPipeline;
            switch (effectiveBlendMode) {
                case 2: desiredPipeline = alphaPipeline_; break;
                default: desiredPipeline = additivePipeline_; break;
            }
            // An opaque layer of a faded instance: blended by its fade.
            if (instanceFaded && effectiveBlendMode <= 1) desiredPipeline = alphaPipeline_;
            if (m2UseAuthoredCull(cullAlphaPipeline_ || cullAdditivePipeline_, skyMode_,
                                  batch.hasAuthoredMaterial,
                                  m2FoliageTransformPreservesWinding(e.model), batch.materialFlags)) {
                desiredPipeline = culledPipeline(desiredPipeline);
            }
            if (desiredPipeline != currentPipeline) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, desiredPipeline);
                currentPipeline = desiredPipeline;
            }

            if (batch.materialUBOMapped) {
                auto* mat = static_cast<M2MaterialUBO*>(batch.materialUBOMapped);
                mat->interiorDarken = 0.0f;
            }

            if (!batch.materialSet) continue;
            if (batch.materialSet != currentMaterialSet) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        pipelineLayout_, 1, 1, &batch.materialSet, 0, nullptr);
                currentMaterialSet = batch.materialSet;
            }

            // Push constants + single-instance draw
            M2PushConstants pc;
            pc.texCoordSet = static_cast<int32_t>(batch.textureUnit);
            pc.texCoordSet2 = batch.textureUnit2;
            fillDrawMode(pc, model, skyMode_);
            pc.instanceDataOffset = static_cast<int32_t>(drawOffset);
            vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
            if (projectedDraw) {
                const VkDeviceSize projectedOffset = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &projectedVB_[vkCtx_->getCurrentFrame()], &projectedOffset);
                vkCmdDraw(cmd, projectedDraw->vertexCount, 1, projectedDraw->firstVertex, 0);
                const VkDeviceSize modelOffset = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &model.vertexBuffer, &modelOffset);
            } else {
                vkCmdDrawIndexed(cmd, batch.indexCount, 1, batch.indexStart, 0, 0);
            }
            if (skyMode_) ++skyDiagDrawsTransparent_;
            lastDrawCallCount++;
        }
    }

    if (beforeTransparentDepth) beforeTransparentDepth(std::numeric_limits<float>::infinity());

    // Render glow sprites as billboarded additive point lights
    const uint32_t glowFrame = vkCtx_->getCurrentFrame();
    if (!glowSprites_.empty() && particleAdditivePipeline_ && glowVB_[glowFrame] && glowTexDescSet_) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, particleAdditivePipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                particlePipelineLayout_, 0, 1, &perFrameSet, 0, nullptr);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                particlePipelineLayout_, 1, 1, &glowTexDescSet_, 0, nullptr);

        // Push constants for particle: tileCount(vec2) + blendMode(int); 4 = additive
        struct { float tileX, tileY; int blendMode; float viewportHeight; } particlePush = {
            .tileX = 1.0f, .tileY = 1.0f, .blendMode = 4,
            .viewportHeight = static_cast<float>(vkCtx_->getSwapchainExtent().height)};
        vkCmdPushConstants(cmd, particlePipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(particlePush), &particlePush);

        // Write glow vertex data directly to mapped buffer (no temp vector)
        size_t uploadCount = std::min(glowSprites_.size(), MAX_GLOW_SPRITES);
        float* dst = static_cast<float*>(glowVBMapped_[glowFrame]);
        for (size_t gi = 0; gi < uploadCount; gi++) {
            const auto& gs = glowSprites_[gi];
            *dst++ = gs.worldPos.x;
            *dst++ = gs.worldPos.y;
            *dst++ = gs.worldPos.z;
            *dst++ = gs.color.r;
            *dst++ = gs.color.g;
            *dst++ = gs.color.b;
            *dst++ = gs.color.a;
            *dst++ = gs.size;
            *dst++ = 0.0f;  // tile
            *dst++ = 0.0f;  // tail displacement (none)
            *dst++ = 0.0f;
            *dst++ = 0.0f;
            *dst++ = 0.0f;  // rotation
        }

        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &glowVB_[glowFrame], &offset);
        // The particle pipeline reads one sprite per instance, six corners each.
        vkCmdDraw(cmd, 6, static_cast<uint32_t>(uploadCount), 0, 0);
    }

    // How many of the sky's layers were actually drawn this frame.
    //
    // The instance-level report says DRAWN every frame, which is a different
    // question: it counts instances that survived culling, not batches that
    // reached a draw call. Every per-batch gate between the two is meant to be
    // constant here - the material flags are static, the LOD is chosen by a
    // distance that does not change for a dome centred on the camera, and the
    // batch opacity is baked at load - so this number should never move. If it
    // moves while the camera turns, the flicker is layers appearing and
    // disappearing and one of those gates is not as constant as it reads.
    if (skyMode_ && (skyDiagDrawsOpaque_ != skyDiagLastOpaque_ ||
                     skyDiagDrawsTransparent_ != skyDiagLastTransparent_)) {
        LOG_INFO("skyM2 draws: opaque=", skyDiagDrawsOpaque_,
                 " transparent=", skyDiagDrawsTransparent_,
                 " (was ", skyDiagLastOpaque_, "/", skyDiagLastTransparent_, ")");
        skyDiagLastOpaque_ = skyDiagDrawsOpaque_;
        skyDiagLastTransparent_ = skyDiagDrawsTransparent_;
    }

    if (m2Profile) {
        const auto m2T3 = std::chrono::steady_clock::now();
        const auto ms = [](auto a, auto b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };
        sayPhases({.cull = ms(m2T0, m2T1),
                   .sort = ms(m2T1, m2T2),
                   .record = ms(m2T2, m2T3),
                   .visible = sortedVisible_.size(),
                   .instances = instances.size()});
    }
}

bool M2Renderer::initializeShadow(VkRenderPass shadowRenderPass) {
    if (!vkCtx_ || shadowRenderPass == VK_NULL_HANDLE) return false;
    VkDevice device = vkCtx_->getDevice();

    // The set the shadow pass binds, built the same way for all four renderers.
    if (!createShadowParamsSet(device, vkCtx_->getAllocator(), sizeof(ShadowParamsUBO),
                               whiteTexture_->getImageView(),
                              whiteTexture_->getSampler(), "M2Renderer", shadowParams_)) {
        return false;
    }

    // Per-frame pools for foliage shadow texture sets (one per frame-in-flight, reset each frame)
    {
        VkDescriptorPoolSize texPoolSizes[2]{};
        texPoolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        texPoolSizes[0].descriptorCount = 256;
        texPoolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        texPoolSizes[1].descriptorCount = 256;
        VkDescriptorPoolCreateInfo texPoolCI{};
        texPoolCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        texPoolCI.maxSets = 256;
        texPoolCI.poolSizeCount = 2;
        texPoolCI.pPoolSizes = texPoolSizes;
        for (uint32_t f = 0; f < kShadowTexPoolFrames; ++f) {
            if (vkCreateDescriptorPool(device, &texPoolCI, nullptr, &shadowTexPool_[f]) != VK_SUCCESS) {
                LOG_ERROR("M2Renderer: failed to create shadow texture pool ", f);
                return false;
            }
        }
    }

    // Create shadow pipeline layout: set 1 = shadowParams_.layout, push constants = 128 bytes
    VkPushConstantRange pc{};
    // The fragment stage reads the alpha-test flags out of the same block.
    pc.stageFlags = kSharedShadowPushStages;
    pc.offset = 0;
    pc.size = sizeof(ShadowPush);  // one combined matrix and the pass flags
    shadowPipelineLayout_ = createPipelineLayout(device, {shadowParams_.layout}, {pc});
    if (!shadowPipelineLayout_) {
        LOG_ERROR("M2Renderer: failed to create shadow pipeline layout");
        return false;
    }

    // Load shadow shaders
    VkShaderModule vertShader, fragShader;
    if (!vertShader.loadFromFile(device, "assets/shaders/shadow.vert.spv")) {
        LOG_ERROR("M2Renderer: failed to load shadow vertex shader");
        return false;
    }
    if (!fragShader.loadFromFile(device, "assets/shaders/shadow.frag.spv")) {
        LOG_ERROR("M2Renderer: failed to load shadow fragment shader");
        return false;
    }

    // M2 vertex layout: 18 floats = 72 bytes stride
    // loc0=pos(off0), loc1=normal(off12), loc2=texCoord0(off24), loc5=texCoord1(off32),
    // loc3=boneWeights(off40), loc4=boneIndices(off56)
    // Shadow shader locations: 0=aPos, 1=aTexCoord, 2=aBoneWeights, 3=aBoneIndicesF
    // useBones=0 so locations 2,3 are never used
    VkVertexInputBindingDescription vertBind{};
    vertBind.binding = 0;
    vertBind.stride = 18 * sizeof(float);
    vertBind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    std::vector<VkVertexInputAttributeDescription> vertAttrs = {
        {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT,    .offset = 0},                     // aPos       -> position
        {.location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,       .offset = 6 * sizeof(float)},     // aTexCoord  -> texCoord0
        {.location = 2, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 10 * sizeof(float)},    // aBoneWeights
        {.location = 3, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 14 * sizeof(float)},    // aBoneIndicesF
    };

    shadowPipeline_ = buildShadowPipeline(
        device, vkCtx_->getPipelineCache(),
        vertShader.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
        fragShader.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT),
        vertBind, vertAttrs, shadowPipelineLayout_, shadowRenderPass,
        vkCtx_->useDynamicRendering());

    vertShader.destroy();
    fragShader.destroy();

    if (!shadowPipeline_) {
        LOG_ERROR("M2Renderer: failed to create shadow pipeline");
        return false;
    }
    LOG_INFO("M2Renderer shadow pipeline initialized");
    return true;
}

void M2Renderer::renderShadow(VkCommandBuffer cmd, const glm::mat4& lightSpaceMatrix,
                              const glm::vec3& /*shadowCenter*/, float shadowRadius) {
    if (!shadowPipeline_ || !shadowParams_.set) return;
    if (instances.empty() || models.empty()) return;

    // Reset this frame slot's texture descriptor pool (safe: fence was waited on in beginFrame)
    const uint32_t frameIdx = vkCtx_->getCurrentFrame();
    VkDescriptorPool curShadowTexPool = shadowTexPool_[frameIdx];
    if (curShadowTexPool) {
        vkResetDescriptorPool(vkCtx_->getDevice(), curShadowTexPool, 0);
    }
    // Cache: texture imageView -> allocated descriptor set (avoids duplicates within frame)
    // Reuse persistent map - pool reset already invalidated the sets.
    shadowTexSetCache_.clear();
    auto& texSetCache = shadowTexSetCache_;

    auto getTexDescSet = [&](VkTexture* tex) -> VkDescriptorSet {
        VkImageView iv = tex->getImageView();
        auto cacheIt = texSetCache.find(iv);
        if (cacheIt != texSetCache.end()) return cacheIt->second;

        VkDescriptorSet set = VK_NULL_HANDLE;
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = curShadowTexPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &shadowParams_.layout;
        if (vkAllocateDescriptorSets(vkCtx_->getDevice(), &ai, &set) != VK_SUCCESS) {
            return shadowParams_.set; // fallback to white texture
        }
        VkDescriptorImageInfo imgInfo{};
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imgInfo.imageView = iv;
        imgInfo.sampler = tex->getSampler();
        VkDescriptorBufferInfo bufInfo{};
        bufInfo.buffer = shadowParams_.ubo;
        bufInfo.offset = 0;
        bufInfo.range = sizeof(ShadowParamsUBO);
        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = set;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &imgInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = set;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[1].pBufferInfo = &bufInfo;
        vkUpdateDescriptorSets(vkCtx_->getDevice(), 2, writes, 0, nullptr);
        texSetCache[iv] = set;
        return set;
    };

    // Cull once, grouped by model.
    //
    // This walked every instance in the world twice - once per pass - doing a
    // light-space transform for each, then drew them in whatever order they
    // sat in: the vertex and index buffers were rebound whenever consecutive
    // instances came from different models, which unsorted is most of them,
    // and a batch's texture was bound once per instance rather than once per
    // batch. With tens of thousands of instances resident that is the cost of
    // this pass, and none of it is drawing.
    shadowCasters_.clear();
    for (uint32_t i = 0; i < instances.size(); ++i) {
        const auto& instance = instances[i];
        if (!instance.cachedIsValid ||
            instance.cachedIsInvisibleTrap || instance.retiring) continue;
        if (!instance.cachedModel) continue;
        const M2ModelGPU& model = *instance.cachedModel;

        // Cull casters against the light-space ortho footprint, not a
        // world-space sphere around the player. The shadow frustum extends
        // ~2000 units toward the sun, so a distant tree can legitimately
        // cast across the whole view while sitting far outside any player
        // sphere - sphere culling made such shadows pop on/off at the cull
        // boundary as the player moved (large-area flicker at low sun).
        const glm::vec4 clip = lightSpaceMatrix * glm::vec4(instance.position, 1.0f);
        // Orthographic projection: w == 1, NDC directly comparable.
        // Inflate by the model's bounding sphere converted to NDC
        // (shadowRadius ≈ frustum half-extent; overshoot is harmless).
        const float margin = (model.boundRadius * instance.scale) / shadowRadius * 1.5f;
        if (std::abs(clip.x) > 1.0f + margin || std::abs(clip.y) > 1.0f + margin) continue;
        if (clip.z < -margin || clip.z > 1.0f + margin) continue;

        shadowCasters_.emplace_back(instance.modelId, i);
    }
    std::sort(shadowCasters_.begin(), shadowCasters_.end());

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline_);
    VkDescriptorSet boundSet = shadowParams_.set;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipelineLayout_,
        0, 1, &boundSet, 0, nullptr);

    for (std::size_t g = 0; g < shadowCasters_.size();) {
        const uint32_t modelId = shadowCasters_[g].first;
        std::size_t groupEnd = g;
        while (groupEnd < shadowCasters_.size() && shadowCasters_[groupEnd].first == modelId) ++groupEnd;

        const auto& firstInstance = instances[shadowCasters_[g].second];
        if (!firstInstance.cachedModel) { g = groupEnd; continue; }
        const M2ModelGPU& model = *firstInstance.cachedModel;

        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &model.vertexBuffer, &offset);
        vkCmdBindIndexBuffer(cmd, model.indexBuffer, 0, VK_INDEX_TYPE_UINT16);

        for (const auto& batch : model.batches) {
            if (batch.submeshLevel > 0) continue;
            if (batch.projected) continue;  // laid on the ground, casts nothing
            // A batch casts the coverage its main pass draws: the alpha-key
            // leaves of a tree and a DXT1 punch-through card by their texture
            // alpha, an opaque trunk as solid. The material decides, as it
            // does in the main pass, whatever the model is called.
            const bool cutout = batch.blendMode == M2_BLEND_OPAQUE
                                    ? batch.alphaIsSilhouette
                                    : batch.hasAlpha;
            // A cut-out batch with no texture is a shadow with no caster.
            //
            // The white set's alpha test passes everywhere, so a batch that
            // arrived without its texture would cast the shadow of a solid
            // quad - while the main pass skips that same batch for want of a
            // material set. The result is a black rectangle on the ground under
            // a tree that looks fine, coming and going as the texture cache
            // lets the sheet back in.
            if (cutout && !batch.texture) {
                if (!warnedShadowNoTexture_) {
                    warnedShadowNoTexture_ = true;
                    LOG_WARNING("Shadow pass: a cut-out batch of ", model.name,
                                " has no texture; skipping it rather than casting a "
                                "solid quad. The texture cache is the usual reason.");
                }
                continue;
            }
            // Once per batch: the texture is the batch's, and every instance
            // of this model shares it.
            const VkDescriptorSet wanted = cutout ? getTexDescSet(batch.texture) : shadowParams_.set;
            if (wanted != boundSet) {
                boundSet = wanted;
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    shadowPipelineLayout_, 0, 1, &boundSet, 0, nullptr);
            }
            // What this draw is, carried with each draw rather than written
            // into a buffer all draws share: a uniform buffer is read when a
            // draw executes, not when it is recorded.
            const glm::ivec4 drawFlags{cutout ? 1 : 0, cutout ? 1 : 0, 0, 0};

            for (std::size_t k = g; k < groupEnd; ++k) {
                const auto& instance = instances[shadowCasters_[k].second];
                ShadowPush push{
                    .lightSpaceModel = lightSpaceMatrix * instance.modelMatrix,
                    .flags = drawFlags};
                vkCmdPushConstants(cmd, shadowPipelineLayout_, kSharedShadowPushStages,
                                   0, sizeof(ShadowPush), &push);
                vkCmdDrawIndexed(cmd, batch.indexCount, 1, batch.indexStart, 0, 0);
            }
        }
        g = groupEnd;
    }
}

} // namespace rendering
} // namespace wowee
