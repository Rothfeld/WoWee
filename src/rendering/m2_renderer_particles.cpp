#include "rendering/particle_emission.hpp"
#include "rendering/m2_renderer.hpp"
#include <unordered_set>
#include "rendering/m2_renderer_internal.h"
#include "rendering/vk_context.hpp"
#include "rendering/vk_buffer.hpp"
#include "rendering/vk_texture.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_utils.hpp"
#include "rendering/camera.hpp"
#include "pipeline/asset_manager.hpp"
#include "core/logger.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <random>
#include <limits>
#include <cassert>
#include <set>
#include <stdexcept>

namespace wowee {
namespace rendering {

// --- M2 Particle Emitter Helpers ---

float M2Renderer::interpFloat(const pipeline::M2AnimationTrack& track, float animTime,
                                float globalTime, int seqIdx,
                                const std::vector<uint32_t>& globalSeqDurations) {
    return m2_track::sampleFloat(track, seqIdx, animTime, globalTime,
                                 globalSeqDurations, 0.0f);
}

// Interpolate an M2 FBlock (particle lifetime curve) at a given life ratio [0..1].
// FBlocks store per-lifetime keyframes for particle color, alpha, and scale.
// NOTE: interpFBlockFloat and interpFBlockVec3 share identical interpolation logic -
// if you fix a bug in one, update the other to match.
float M2Renderer::interpFBlockFloat(const pipeline::M2FBlock& fb, float lifeRatio) {
    if (fb.floatValues.empty()) return 1.0f;
    if (fb.floatValues.size() == 1 || fb.timestamps.empty()) return fb.floatValues[0];
    lifeRatio = glm::clamp(lifeRatio, 0.0f, 1.0f);
    for (size_t i = 0; i < fb.timestamps.size() - 1; i++) {
        if (lifeRatio <= fb.timestamps[i + 1]) {
            float t0 = fb.timestamps[i];
            float t1 = fb.timestamps[i + 1];
            float dur = t1 - t0;
            float frac = (dur > 0.0f) ? (lifeRatio - t0) / dur : 0.0f;
            size_t v0 = std::min(i, fb.floatValues.size() - 1);
            size_t v1 = std::min(i + 1, fb.floatValues.size() - 1);
            return glm::mix(fb.floatValues[v0], fb.floatValues[v1], frac);
        }
    }
    return fb.floatValues.back();
}

glm::vec3 M2Renderer::interpFBlockVec3(const pipeline::M2FBlock& fb, float lifeRatio) {
    if (fb.vec3Values.empty()) return glm::vec3(1.0f);
    if (fb.vec3Values.size() == 1 || fb.timestamps.empty()) return fb.vec3Values[0];
    lifeRatio = glm::clamp(lifeRatio, 0.0f, 1.0f);
    for (size_t i = 0; i < fb.timestamps.size() - 1; i++) {
        if (lifeRatio <= fb.timestamps[i + 1]) {
            float t0 = fb.timestamps[i];
            float t1 = fb.timestamps[i + 1];
            float dur = t1 - t0;
            float frac = (dur > 0.0f) ? (lifeRatio - t0) / dur : 0.0f;
            size_t v0 = std::min(i, fb.vec3Values.size() - 1);
            size_t v1 = std::min(i + 1, fb.vec3Values.size() - 1);
            return glm::mix(fb.vec3Values[v0], fb.vec3Values[v1], frac);
        }
    }
    return fb.vec3Values.back();
}

void M2Renderer::emitParticles(M2Instance& inst, const M2ModelGPU& gpu, float dt) {

    if (inst.emitterAccumulators.size() != gpu.particleEmitters.size()) {
        inst.emitterAccumulators.resize(gpu.particleEmitters.size(), 0.0f);
    }
    if (inst.emitterPreviousBirth.size() != gpu.particleEmitters.size()) {
        inst.emitterPreviousBirth.assign(gpu.particleEmitters.size(), glm::mat4(1.0f));
        for (size_t ei = 0; ei < gpu.particleEmitters.size(); ++ei) {
            const auto& em = gpu.particleEmitters[ei];
            inst.emitterPreviousBirth[ei] = particleBirthTransform(em.flags, inst.modelMatrix,
                em.bone < inst.boneMatrices.size() ? inst.boneMatrices[em.bone] : glm::mat4(1.0f));
        }
    }

    // A held last frame is a stopped clock: nothing is born (see
    // characterEmitterClockHeld). Particles already alive finish their lives.
    if (inst.externalPose && inst.externalClockHeld) return;

    std::uniform_real_distribution<float> distN(-1.0f, 1.0f);

    for (size_t ei = 0; ei < gpu.particleEmitters.size(); ei++) {
        const auto& em = gpu.particleEmitters[ei];
        // Emitter pose now and at the previous step, recorded every frame so
        // an emitter that resumes does not reach back to a stale pose.
        const glm::mat4 boneXform = em.bone < inst.boneMatrices.size()
            ? inst.boneMatrices[em.bone] : glm::mat4(1.0f);
        const glm::mat4 birthTransform = particleBirthTransform(
            em.flags, inst.modelMatrix, boneXform);
        const glm::mat4 previousBirth = inst.emitterPreviousBirth[ei];
        inst.emitterPreviousBirth[ei] = birthTransform;
        if (!em.enabled) continue;
        if (m2_track::sampleFloat(em.enabledIn, inst.currentSequenceIndex, inst.animTime,
                                  inst.globalSequenceTime, gpu.globalSequenceDurations, 1.0f) <= 0.0f)
            continue;

        float rate = interpFloat(em.emissionRate, inst.animTime, inst.globalSequenceTime,
                                 inst.currentSequenceIndex, gpu.globalSequenceDurations);
        if (m2_track::pastLastStepKey(em.emissionRate, inst.currentSequenceIndex, inst.animTime,
                                      inst.globalSequenceTime, gpu.globalSequenceDurations))
            rate = 0.0f;
        float life = interpFloat(em.lifespan, inst.animTime, inst.globalSequenceTime,
                                 inst.currentSequenceIndex, gpu.globalSequenceDurations);
        // What the player asked to see of it, before the floor below. The order
        // is the whole point: thinning first and flooring second lets a low
        // setting take smoke, dust and spell effects down while a candle is
        // pulled back up to the handful of particles that still reads as fire.
        rate *= particleDensity_;

        if (rate <= 0.0f || life <= 0.0f) {
            // Diagnostic: a lamp or flame whose emitter never fires produces a
            // fixture that glows but shows no flame. Report each model once so a
            // default-level log says whether emission is the cause.
            if (gpu.isLanternLike || gpu.isTorch || gpu.isBrazierOrFire) {
                static std::unordered_set<std::string> reported;
                if (reported.insert(gpu.name).second) {
                    LOG_WARNING("Flame emitter idle: '", gpu.name, "' emitter=", ei,
                                " rate=", rate, " life=", life,
                                " animTime=", inst.animTime,
                                " seqIdx=", inst.currentSequenceIndex,
                                " gsTime=", inst.globalSequenceTime,
                                " rateSeqs=", em.emissionRate.sequences.size(),
                                " lifeSeqs=", em.lifespan.sequences.size(),
                                " rateGlobalSeq=", em.emissionRate.globalSequence);
                }
            }
            continue;
        }

        inst.emitterAccumulators[ei] += rate * dt;

        while (inst.emitterAccumulators[ei] >= 1.0f && inst.particles.size() < MAX_M2_PARTICLES) {
            inst.emitterAccumulators[ei] -= 1.0f;

            M2Particle p;
            p.emitterIndex = static_cast<int>(ei);
            // The accumulator locates this birth inside the frame. Aging every
            // newborn by the full frame can erase short sparks before rendering.
            p.life = inst.emitterAccumulators[ei] / rate;
            p.maxLife = life;
            if (p.life >= p.maxLife) continue;

            // Position: emitter position transformed by bone matrix
            glm::vec3 localPos = em.position;
            glm::vec3 sphereDirection(0.0f);
            if (em.emitterType == 1) {
                const float length = interpFloat(em.emissionAreaLength, inst.animTime,
                    inst.globalSequenceTime, inst.currentSequenceIndex, gpu.globalSequenceDurations);
                const float width = interpFloat(em.emissionAreaWidth, inst.animTime,
                    inst.globalSequenceTime, inst.currentSequenceIndex, gpu.globalSequenceDurations);
                localPos.x += distN(particleRng_) * length * 0.5f;
                localPos.y += distN(particleRng_) * width * 0.5f;
            }
            if (em.emitterType == 2) {
                const float length = interpFloat(em.emissionAreaLength, inst.animTime,
                    inst.globalSequenceTime, inst.currentSequenceIndex, gpu.globalSequenceDurations);
                const float width = interpFloat(em.emissionAreaWidth, inst.animTime,
                    inst.globalSequenceTime, inst.currentSequenceIndex, gpu.globalSequenceDurations);
                const float vertical = interpFloat(em.verticalRange, inst.animTime,
                    inst.globalSequenceTime, inst.currentSequenceIndex, gpu.globalSequenceDurations);
                const float horizontal = interpFloat(em.horizontalRange, inst.animTime,
                    inst.globalSequenceTime, inst.currentSequenceIndex, gpu.globalSequenceDurations);
                const auto emission = sphericalParticleEmission(length, width,
                    distN(particleRng_) * vertical, distN(particleRng_) * horizontal,
                    (distN(particleRng_) + 1.0f) * 0.5f, em.flags);
                localPos += emission.offset;
                sphereDirection = emission.direction;
            }
            if (em.emitterType == 3 && !em.splinePoints.empty()) {
                const float length = interpFloat(em.emissionAreaLength, inst.animTime,
                    inst.globalSequenceTime, inst.currentSequenceIndex, gpu.globalSequenceDurations);
                const float width = interpFloat(em.emissionAreaWidth, inst.animTime,
                    inst.globalSequenceTime, inst.currentSequenceIndex, gpu.globalSequenceDurations);
                localPos += splineEmissionPoint(em.splinePoints, splineBirthParameter(length, width,
                    particleTrackVaries(em.emissionAreaWidth, inst.currentSequenceIndex),
                    (distN(particleRng_) + 1.0f) * 0.5f));
            }
            // Born p.life ago: where the emitter was then, between its pose at
            // the previous step and now. Placing every birth of a frame at the
            // current pose bunches a moving emitter's trail into one bead per
            // frame (DR_Draenei_HoloRunes02 sparks at 8 fps).
            p.position = glm::vec3(particleBirthPosition(previousBirth, birthTransform,
                                                         localPos, p.life, dt));
            p.birthPosition = p.position;
            p.gravityDirection = particleGravityDirection(em.flags, inst.modelMatrix);
            p.birthBasis = glm::mat3(birthTransform);
            p.twinkleSeed = static_cast<uint32_t>(particleRng_());
            if (!em.geometryModel.empty()) {
                for (int axis = 0; axis < 3; ++axis)
                    p.tumbleRate[axis] = glm::mix(em.tumbleMin[axis], em.tumbleMax[axis],
                                                  (distN(particleRng_) + 1.0f) * 0.5f);
            }

            // Velocity: emission speed in upward direction + random spread
            float speed = interpFloat(em.emissionSpeed, inst.animTime, inst.globalSequenceTime,
                                      inst.currentSequenceIndex, gpu.globalSequenceDurations);
            float vRange = interpFloat(em.verticalRange, inst.animTime, inst.globalSequenceTime,
                                       inst.currentSequenceIndex, gpu.globalSequenceDurations);
            float hRange = interpFloat(em.horizontalRange, inst.animTime, inst.globalSequenceTime,
                                       inst.currentSequenceIndex, gpu.globalSequenceDurations);

            // Ranges are angles, not Cartesian velocity components. Zero
            // inclination stays vertical even with a full azimuth range.
            const float inclination = distN(particleRng_) * vRange;
            const float azimuth = distN(particleRng_) * hRange;
            const glm::vec3 dir(std::sin(inclination) * std::cos(azimuth),
                                std::sin(inclination) * std::sin(azimuth),
                                std::cos(inclination));
            const float variation = interpFloat(em.speedVariation, inst.animTime,
                inst.globalSequenceTime, inst.currentSequenceIndex, gpu.globalSequenceDurations);
            speed *= 1.0f + distN(particleRng_) * variation;
            const glm::mat3 rotMat = particleWorldVelocityBasis(em.flags, birthTransform);
            p.velocity = rotMat * (em.emitterType == 2 ? sphereDirection : dir) * speed;

            const float gravity = interpFloat(em.gravity, inst.animTime,
                inst.globalSequenceTime, inst.currentSequenceIndex, gpu.globalSequenceDurations);
            p.position += p.velocity * p.life;
            p.position += p.gravityDirection * (0.5f * gravity * p.life * p.life);
            p.velocity += p.gravityDirection * (gravity * p.life);
            inst.particles.push_back(p);

            // Diagnostic: log first particle birth per spell effect instance
            if (gpu.isSpellEffect && inst.particles.size() == 1) {
                LOG_INFO("SpellEffect: first particle for '", gpu.name,
                         "' pos=(", p.position.x, ",", p.position.y, ",", p.position.z,
                         ") rate=", rate, " life=", life,
                         " bone=", em.bone, " boneCount=", inst.boneMatrices.size(),
                         " globalSeqs=", gpu.globalSequenceDurations.size());
            }
        }
        // Cap accumulator to avoid bursts after lag
        if (inst.emitterAccumulators[ei] > 2.0f) {
            inst.emitterAccumulators[ei] = 0.0f;
        }
    }
}

void M2Renderer::updateParticles(M2Instance& inst, float dt) {
    if (!inst.cachedModel) return;
    const auto& gpu = *inst.cachedModel;

    // Hoist per-emitter gravity out of the per-particle loop. Gravity
    // depends only on the emitter and animation time -
    // not on the particle itself - so interpFloat was being re-evaluated for
    // every particle even when 100s of particles share one emitter.
    constexpr size_t kMaxStackEmitters = 16;
    float emitterGravStack[kMaxStackEmitters];
    std::vector<float> emitterGravHeap;
    const size_t numEm = gpu.particleEmitters.size();
    float* emitterGrav = nullptr;
    if (numEm > 0) {
        if (numEm <= kMaxStackEmitters) {
            emitterGrav = emitterGravStack;
        } else {
            emitterGravHeap.resize(numEm);
            emitterGrav = emitterGravHeap.data();
        }
        for (size_t e = 0; e < numEm; ++e) {
            const auto& pem = gpu.particleEmitters[e];
            float grav = interpFloat(pem.gravity,
                                      inst.animTime, inst.globalSequenceTime,
                                      inst.currentSequenceIndex, gpu.globalSequenceDurations);
            // Zero is authored zero acceleration, including LootFX.
            emitterGrav[e] = grav;
        }
    }

    for (size_t i = 0; i < inst.particles.size(); ) {
        auto& p = inst.particles[i];
        p.life += dt;
        if (p.life >= p.maxLife) {
            // Swap-and-pop removal
            inst.particles[i] = inst.particles.back();
            inst.particles.pop_back();
            continue;
        }
        const bool known = p.emitterIndex >= 0 && static_cast<size_t>(p.emitterIndex) < numEm;
        const float drag = known ? gpu.particleEmitters[p.emitterIndex].drag : 0.0f;
        p.position += p.velocity * particleDragTravel(drag, dt);
        p.velocity *= std::exp(-drag * dt);
        if (known) {
            const float gravity = emitterGrav[p.emitterIndex];
            p.position += p.gravityDirection * (0.5f * gravity * dt * dt);
            p.velocity += p.gravityDirection * (gravity * dt);
        }
        i++;
    }
}

// ---------------------------------------------------------------------------
// Ribbon emitter simulation
// ---------------------------------------------------------------------------
void M2Renderer::updateRibbons(M2Instance& inst, const M2ModelGPU& gpu, float dt) {

    const auto& emitters = gpu.ribbonEmitters;
    if (emitters.empty()) return;

    // Grow per-instance state arrays if needed
    if (inst.ribbonEdges.size() != emitters.size()) {
        inst.ribbonEdges.resize(emitters.size());
    }
    if (inst.ribbonEdgeAccumulators.size() != emitters.size()) {
        inst.ribbonEdgeAccumulators.resize(emitters.size(), 0.0f);
    }

    for (size_t ri = 0; ri < emitters.size(); ri++) {
        const auto& em = emitters[ri];
        auto& edges    = inst.ribbonEdges[ri];
        auto& accum    = inst.ribbonEdgeAccumulators[ri];

        // Determine bone world position for spine
        glm::vec3 spineWorld = inst.position;
        // Use referenced bone; fall back to bone 0 if out of range (common for spell effects
        // where ribbon bone fields may be unset/garbage, e.g. bone=4294967295)
        uint32_t boneIdx = em.bone;
        if (boneIdx >= inst.boneMatrices.size() && !inst.boneMatrices.empty()) {
            boneIdx = 0;
        }
        if (boneIdx < inst.boneMatrices.size()) {
            glm::vec4 local(em.position.x, em.position.y, em.position.z, 1.0f);
            spineWorld = glm::vec3(inst.modelMatrix * inst.boneMatrices[boneIdx] * local);
        } else {
            glm::vec4 local(em.position.x, em.position.y, em.position.z, 1.0f);
            spineWorld = glm::vec3(inst.modelMatrix * local);
        }

        // Skip emitters that produce NaN positions (garbage bone/position data)
        if (std::isnan(spineWorld.x) || std::isnan(spineWorld.y) || std::isnan(spineWorld.z))
            continue;

        // Evaluate animated tracks (use first available sequence key, or fallback value)
        auto getFloatVal = [&](const pipeline::M2AnimationTrack& track, float fallback) -> float {
            for (const auto& seq : track.sequences) {
                if (!seq.floatValues.empty()) return seq.floatValues[0];
            }
            return fallback;
        };
        auto getVec3Val = [&](const pipeline::M2AnimationTrack& track, glm::vec3 fallback) -> glm::vec3 {
            for (const auto& seq : track.sequences) {
                if (!seq.vec3Values.empty()) return seq.vec3Values[0];
            }
            return fallback;
        };

        float visibility  = getFloatVal(em.visibilityTrack, 1.0f);
        float heightAbove = getFloatVal(em.heightAboveTrack, 0.5f);
        float heightBelow = getFloatVal(em.heightBelowTrack, 0.5f);
        glm::vec3 color   = getVec3Val(em.colorTrack, glm::vec3(1.0f));
        float alpha       = getFloatVal(em.alphaTrack, 1.0f);

        // Age existing edges and remove expired ones
        for (auto& e : edges) {
            e.age += dt;
            // Apply gravity
            if (em.gravity != 0.0f) {
                e.worldPos.z -= em.gravity * dt * dt * 0.5f;
            }
        }
        while (!edges.empty() && edges.front().age >= em.edgeLifetime) {
            edges.pop_front();
        }

        // Emit new edges based on edgesPerSecond
        if (visibility > 0.5f) {
            accum += em.edgesPerSecond * dt;
            while (accum >= 1.0f) {
                accum -= 1.0f;
                M2Instance::RibbonEdge e;
                e.worldPos    = spineWorld;
                e.color       = color;
                e.alpha       = alpha;
                // Scaled into world units here, where the instance is at hand.
                // The spine is a world position the model matrix produced, and
                // these two were model-space lengths added straight to it - so
                // a doodad placed at any scale but 1 got a trail the right
                // length and the wrong width.
                e.heightAbove = heightAbove * inst.scale;
                e.heightBelow = heightBelow * inst.scale;
                e.age         = 0.0f;
                edges.push_back(e);

                // Diagnostic: log first ribbon edge per spell effect instance+emitter
                if (gpu.isSpellEffect && edges.size() == 1) {
                    LOG_INFO("SpellEffect: ribbon edge[0] for '", gpu.name,
                             "' emitter=", ri, " pos=(", spineWorld.x, ",", spineWorld.y,
                             ",", spineWorld.z, ") hA=", heightAbove, " hB=", heightBelow,
                             " vis=", visibility, " eps=", em.edgesPerSecond,
                             " edgeLife=", em.edgeLifetime, " bone=", em.bone);
                }

                // Cap trail length
                if (edges.size() > 128) edges.pop_front();
            }
        } else {
            accum = 0.0f;
        }
    }
}

// ---------------------------------------------------------------------------
// Ribbon rendering
// ---------------------------------------------------------------------------
void M2Renderer::renderParticleMeshes(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) {
    // A geometry-model emitter (Whirlwind_State_Base #0/#1: WhirlwindGeoWhite)
    // draws that model at each particle, in the emitter's orientation at its
    // birth, scaled by the particle's lifecycle size and tinted by its colour
    // and opacity, with the emitter's texture and blend, turning at its tumble
    // rate. Original 2.4.3 shows
    // flat red swirls rising and widening around a whirlwinding unit;
    // billboarding CLOUDS.BLP instead drew a wall of red squares.
    const uint32_t frame = vkCtx_->getCurrentFrame();
    if (!particleMeshPipeline_ || !particleMeshAdditivePipeline_) return;
    size_t required = 0;
    for (size_t idx : particleInstanceIndices_) {
        if (idx >= instances.size()) continue;
        const auto& inst = instances[idx];
        if (!inst.cachedModel || inst.particles.empty()) continue;
        const auto& geometry = inst.cachedModel->particleGeometry;
        for (const auto& p : inst.particles)
            if (p.emitterIndex >= 0 && static_cast<size_t>(p.emitterIndex) < geometry.size())
                required += geometry[p.emitterIndex].indices.size();
    }
    if (required == 0) return;
    if (required > particleMeshVertexCapacity_[frame]) {
        const size_t capacity = std::max(required, particleMeshVertexCapacity_[frame] * 2);
        VkBufferCreateInfo info{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = capacity * 9 * sizeof(float);
        info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        VmaAllocationCreateInfo allocationInfo{};
        allocationInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        allocationInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
        ::VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VmaAllocationInfo mapped{};
        const auto allocator = vkCtx_->getAllocator();
        const VkResult result = vmaCreateBuffer(allocator, &info, &allocationInfo, &buffer, &allocation, &mapped);
        if (result != VK_SUCCESS) {
            LOG_ERROR("Particle mesh vertex buffer allocation failed: ", result, " vertices=", capacity);
            throw std::runtime_error("Particle mesh vertex buffer allocation failed");
        }
        const ::VkBuffer previousBuffer = particleMeshVB_[frame];
        const VmaAllocation previousAllocation = particleMeshVBAlloc_[frame];
        if (previousBuffer) {
            vkCtx_->deferAfterAllFrameFences([allocator, previousBuffer, previousAllocation] {
                vmaDestroyBuffer(allocator, previousBuffer, previousAllocation);
            });
        }
        particleMeshVB_[frame] = buffer;
        particleMeshVBAlloc_[frame] = allocation;
        particleMeshVBMapped_[frame] = mapped.pMappedData;
        particleMeshVertexCapacity_[frame] = capacity;
    }

    float* const dst = static_cast<float*>(particleMeshVBMapped_[frame]);
    size_t written = 0;
    ribbonDraws_.clear();
    for (size_t idx : particleInstanceIndices_) {
        if (idx >= instances.size()) continue;
        const auto& inst = instances[idx];
        if (!inst.cachedModel || inst.particles.empty()) continue;
        const auto& gpu = *inst.cachedModel;
        const glm::vec3 distance = inst.position - cachedCamPos_;
        if (glm::dot(distance, distance) > cachedMaxRenderDistSq_) continue;
        if (outsideVisibleRegion(inst.position) || roomCulled(inst)) continue;
        for (const auto& p : inst.particles) {
            if (p.emitterIndex < 0 || static_cast<size_t>(p.emitterIndex) >= gpu.particleGeometry.size()) continue;
            const auto& mesh = gpu.particleGeometry[p.emitterIndex];
            if (mesh.indices.empty()) continue;
            const auto& em = gpu.particleEmitters[p.emitterIndex];
            const VkDescriptorSet texSet = static_cast<size_t>(p.emitterIndex) < gpu.particleTexSets.size()
                ? gpu.particleTexSets[p.emitterIndex] : VK_NULL_HANDLE;
            if (!texSet) continue;
            glm::mat4 bone(1.0f);
            if ((em.flags & kParticleNoTrail) && !gpu.bones.empty()) bone = inst.boneMatrices.at(em.bone);
            const glm::mat4 render = particleRenderTransform(em.flags, inst.modelMatrix, bone);
            const float lifeRatio = p.life / std::max(p.maxLife, 0.001f);
            const glm::vec3 color = interpFBlockVec3(em.particleColor, lifeRatio);
            const float alpha = std::min(interpFBlockFloat(em.particleAlpha, lifeRatio), 1.0f) * inst.fade;
            const float scale = interpFBlockFloat(em.particleScale, lifeRatio);
            const VkPipeline pipeline = em.blendingType >= 3 ? particleMeshAdditivePipeline_ : particleMeshPipeline_;
            // Tumbling: a constant angular velocity in the birth frame.
            glm::mat3 basis = p.birthBasis;
            const float rate = glm::length(p.tumbleRate);
            if (rate > 0.0f)
                basis = basis * glm::mat3(glm::rotate(glm::mat4(1.0f), rate * p.life, p.tumbleRate / rate));
            const uint32_t first = static_cast<uint32_t>(written);
            for (const uint16_t index : mesh.indices) {
                assert(index < mesh.positions.size());
                const glm::vec3 world = glm::vec3(render * glm::vec4(
                    p.position + basis * (mesh.positions[index] * scale), 1.0f));
                float* v = dst + written * 9;
                v[0] = world.x; v[1] = world.y; v[2] = world.z;
                v[3] = color.r; v[4] = color.g; v[5] = color.b;
                v[6] = alpha;
                v[7] = mesh.uvs[index].x; v[8] = mesh.uvs[index].y;
                ++written;
            }
            if (!ribbonDraws_.empty() && ribbonDraws_.back().texSet == texSet &&
                ribbonDraws_.back().pipeline == pipeline &&
                ribbonDraws_.back().firstVertex + ribbonDraws_.back().vertexCount == first) {
                ribbonDraws_.back().vertexCount += static_cast<uint32_t>(written) - first;
            } else {
                ribbonDraws_.push_back({.texSet = texSet, .pipeline = pipeline, .firstVertex = first,
                                        .vertexCount = static_cast<uint32_t>(written) - first});
            }
        }
    }
    assert(written <= particleMeshVertexCapacity_[frame]);
    if (ribbonDraws_.empty()) return;
    VkExtent2D ext = vkCtx_->getSwapchainExtent();
    VkViewport vp{.x = 0, .y = 0, .width = static_cast<float>(ext.width),
                  .height = static_cast<float>(ext.height), .minDepth = 0.0f, .maxDepth = 1.0f};
    VkRect2D sc{.offset = {.x = 0, .y = 0}, .extent = ext};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    VkPipeline lastPipe = VK_NULL_HANDLE;
    for (const auto& dc : ribbonDraws_) {
        if (dc.pipeline != lastPipe) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, dc.pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    ribbonPipelineLayout_, 0, 1, &perFrameSet, 0, nullptr);
            lastPipe = dc.pipeline;
        }
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                ribbonPipelineLayout_, 1, 1, &dc.texSet, 0, nullptr);
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &particleMeshVB_[frame], &offset);
        vkCmdDraw(cmd, dc.vertexCount, 1, dc.firstVertex, 0);
    }
}

void M2Renderer::renderM2Ribbons(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) {
    renderParticleMeshes(cmd, perFrameSet);
    const uint32_t frame = vkCtx_->getCurrentFrame();
    if (!ribbonPipeline_ || !ribbonAdditivePipeline_ || !ribbonVB_[frame] || !ribbonVBMapped_[frame]) return;
    // Diagnostic: WOWEE_M2_NO_RIBBONS=1 drops every M2 ribbon trail draw.
    static const bool kNoRibbons = envFlagEnabled("WOWEE_M2_NO_RIBBONS");
    if (kNoRibbons) return;

    // A ribbon's width runs across the trail and across the view, and it has to
    // be computed per edge from the direction the trail is actually going.
    //
    // This used a fixed world Z, which is only right for a trail travelling
    // horizontally. A flame licks upward, so its spine ran along the very axis
    // the strip was being widened on: the top and bottom vertices of every
    // edge landed on the spine itself, the quad collapsed, and the whole trail
    // drew as one tall thin sliver with the fire texture smeared up it. That
    // is the bonfire at Grom'gol standing four times the height of the huts
    // behind it.
    const glm::vec3 camPos = cachedCamPos_;
    const glm::vec3 upWorld(0.0f, 0.0f, 1.0f);

    float* dst     = static_cast<float*>(ribbonVBMapped_[frame]);
    size_t written = 0;

    ribbonDraws_.clear();
    auto& draws = ribbonDraws_;

    for (const auto& inst : instances) {
        if (!inst.cachedModel) continue;
        if (outsideVisibleRegion(inst.position) || roomCulled(inst)) continue;
        const auto& gpu = *inst.cachedModel;
        if (gpu.ribbonEmitters.empty()) continue;

        for (size_t ri = 0; ri < gpu.ribbonEmitters.size(); ri++) {
            if (ri >= inst.ribbonEdges.size()) continue;
            const auto& edges = inst.ribbonEdges[ri];
            if (edges.size() < 2) continue;

            // Select blend pipeline based on material blend mode
            const bool additive = gpu.ribbonBlendModes.at(ri) >= 3;
            VkPipeline pipe = additive ? ribbonAdditivePipeline_ : ribbonPipeline_;

            // Descriptor set for texture
            VkDescriptorSet texSet = (ri < gpu.ribbonTexSets.size())
                                     ? gpu.ribbonTexSets[ri] : VK_NULL_HANDLE;
            if (!texSet) {
                if (gpu.isSpellEffect) {
                    static bool ribbonTexWarn = false;
                    if (!ribbonTexWarn) {
                        LOG_WARNING("SpellEffect: ribbon[", ri, "] for '", gpu.name,
                                    "' has null texSet - descriptor pool may be exhausted");
                        ribbonTexWarn = true;
                    }
                }
                continue;
            }

            uint32_t firstVert = static_cast<uint32_t>(written);

            // Emit triangle strip: 2 verts per edge (top + bottom)
            for (size_t ei = 0; ei < edges.size(); ei++) {
                if (written + 2 > MAX_RIBBON_VERTS) break;
                const auto& e = edges[ei];
                // No fade with edge age: the texture runs head to tail along
                // u and carries the falloff. Fading by age as well halved the
                // visible trail; the original draws Frostbolt's and Arcane
                // Missile's trails at full strength back to the casting hand.
                // The instance fade still applies: a fading owner fades its trails.
                float a = e.alpha * inst.fade;
                float u = static_cast<float>(ei) / static_cast<float>(edges.size() - 1);

                // The trail's own direction here, from the edges either side.
                const glm::vec3& prev = edges[ei > 0 ? ei - 1 : ei].worldPos;
                const glm::vec3& next = edges[ei + 1 < edges.size() ? ei + 1 : ei].worldPos;
                glm::vec3 spineDir = next - prev;
                float spineLen = glm::length(spineDir);
                // Widen across the trail and across the line of sight. Where
                // the trail doubles back on itself or points straight at the
                // eye there is no such direction, and world up is as good an
                // answer as any.
                glm::vec3 side = upWorld;
                if (spineLen > 1e-5f) {
                    glm::vec3 toEye = camPos - e.worldPos;
                    glm::vec3 cross = glm::cross(spineDir / spineLen, toEye);
                    float crossLen = glm::length(cross);
                    if (crossLen > 1e-5f) side = cross / crossLen;
                }

                // Top vertex (one side of the spine)
                glm::vec3 top = e.worldPos + side * e.heightAbove;
                dst[written * 9 + 0] = top.x;
                dst[written * 9 + 1] = top.y;
                dst[written * 9 + 2] = top.z;
                dst[written * 9 + 3] = e.color.r;
                dst[written * 9 + 4] = e.color.g;
                dst[written * 9 + 5] = e.color.b;
                dst[written * 9 + 6] = a;
                dst[written * 9 + 7] = u;
                dst[written * 9 + 8] = 0.0f; // v = top
                written++;

                // Bottom vertex (the other side)
                glm::vec3 bot = e.worldPos - side * e.heightBelow;
                dst[written * 9 + 0] = bot.x;
                dst[written * 9 + 1] = bot.y;
                dst[written * 9 + 2] = bot.z;
                dst[written * 9 + 3] = e.color.r;
                dst[written * 9 + 4] = e.color.g;
                dst[written * 9 + 5] = e.color.b;
                dst[written * 9 + 6] = a;
                dst[written * 9 + 7] = u;
                dst[written * 9 + 8] = 1.0f; // v = bottom
                written++;
            }

            uint32_t vertCount = static_cast<uint32_t>(written) - firstVert;
            if (vertCount >= 4) {
                draws.push_back({.texSet = texSet, .pipeline = pipe, .firstVertex = firstVert, .vertexCount = vertCount});
            } else {
                // Rollback if too few verts
                written = firstVert;
            }
        }
    }

    // SpellChainEffects beams: widened across their own direction and the
    // line of sight, as the trails above.
    for (const auto& strip : chainStrips_) {
        assert(strip.spine.size() == strip.u.size());
        if (strip.spine.size() < 2) continue;
        const VkDescriptorSet texSet = chainTextureSet(strip.texturePath);
        if (!texSet) continue;
        const uint32_t firstVert = static_cast<uint32_t>(written);
        for (size_t i = 0; i < strip.spine.size(); ++i) {
            if (written + 2 > MAX_RIBBON_VERTS) break;
            const glm::vec3& p = strip.spine[i];
            const glm::vec3 dir = strip.spine[i + 1 < strip.spine.size() ? i + 1 : i] - strip.spine[i > 0 ? i - 1 : i];
            glm::vec3 side = upWorld;
            const glm::vec3 cross = glm::cross(dir, camPos - p);
            if (glm::length(cross) > 1e-6f) side = glm::normalize(cross);
            for (int edge = 0; edge < 2; ++edge) {
                const glm::vec3 v = p + side * (edge == 0 ? strip.halfWidth : -strip.halfWidth);
                float* out = dst + written * 9;
                out[0] = v.x; out[1] = v.y; out[2] = v.z;
                out[3] = strip.color.r; out[4] = strip.color.g; out[5] = strip.color.b;
                out[6] = strip.alpha;
                out[7] = strip.u[i];
                out[8] = static_cast<float>(edge);
                ++written;
            }
        }
        const uint32_t vertCount = static_cast<uint32_t>(written) - firstVert;
        if (vertCount >= 4) {
            draws.push_back({.texSet = texSet, .pipeline = strip.additive ? ribbonAdditivePipeline_ : ribbonPipeline_,
                             .firstVertex = firstVert, .vertexCount = vertCount});
        } else {
            written = firstVert;
        }
    }

    // Periodic diagnostic: spell ribbon draw count
    {
        static uint32_t ribbonDiagFrame_ = 0;
        if (++ribbonDiagFrame_ % 300 == 1) {
            size_t spellRibbonDraws = 0;
            size_t spellRibbonVerts = 0;
            for (const auto& inst : instances) {
                if (!inst.cachedModel || !inst.cachedModel->isSpellEffect) continue;
                for (const auto& ribbonEdge : inst.ribbonEdges) {
                    if (ribbonEdge.size() >= 2) {
                        spellRibbonDraws++;
                        spellRibbonVerts += ribbonEdge.size() * 2;
                    }
                }
            }
            if (spellRibbonDraws > 0 || !draws.empty()) {
                LOG_INFO("SpellEffect: ", spellRibbonDraws, " spell ribbon strips (",
                         spellRibbonVerts, " verts), total draws=", draws.size(),
                         " written=", written);
            }
        }
    }

    if (draws.empty() || written == 0) return;

    VkExtent2D ext = vkCtx_->getSwapchainExtent();
    VkViewport vp{};
    vp.x = 0; vp.y = 0;
    vp.width  = static_cast<float>(ext.width);
    vp.height = static_cast<float>(ext.height);
    vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
    VkRect2D sc{};
    sc.offset = {.x = 0, .y = 0};
    sc.extent = ext;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);

    VkPipeline lastPipe = VK_NULL_HANDLE;
    for (const auto& dc : draws) {
        if (dc.pipeline != lastPipe) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, dc.pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    ribbonPipelineLayout_, 0, 1, &perFrameSet, 0, nullptr);
            lastPipe = dc.pipeline;
        }
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                ribbonPipelineLayout_, 1, 1, &dc.texSet, 0, nullptr);
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &ribbonVB_[frame], &offset);
        vkCmdDraw(cmd, dc.vertexCount, 1, dc.firstVertex, 0);
    }
}

VkDescriptorSet M2Renderer::chainTextureSet(const std::string& path) {
    if (const auto found = chainTexSets_.find(path); found != chainTexSets_.end()) return found->second;
    VkDescriptorSet set = VK_NULL_HANDLE;
    // Repeats along the beam (u), clamps across it (v).
    VkTexture* texture = loadTexture(path, 0x1);
    if (!texture || !texture->isValid()) {
        LOG_ERROR("Chain effect texture unavailable: ", path);
    } else if (particleTexLayout_ && materialDescPool_) {
        VkDescriptorSetAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = materialDescPool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &particleTexLayout_;
        if (vkAllocateDescriptorSets(vkCtx_->getDevice(), &ai, &set) == VK_SUCCESS) {
            const VkDescriptorImageInfo image = texture->descriptorInfo();
            VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = set;
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &image;
            vkUpdateDescriptorSets(vkCtx_->getDevice(), 1, &write, 0, nullptr);
        } else {
            LOG_ERROR("Chain effect descriptor set allocation failed: ", path);
            set = VK_NULL_HANDLE;
        }
    }
    chainTexSets_[path] = set;  // a failure is reported once, not every frame
    return set;
}

void M2Renderer::renderM2Particles(VkCommandBuffer cmd, VkDescriptorSet perFrameSet,
                                   ParticlePhase phase) {
    const bool before = phase == ParticlePhase::BeforeTransparent;
    // An emitter belongs to the phase its priority plane sorts it into.
    const auto inPhase = [before](const pipeline::M2ParticleEmitter& emitter) {
        return (emitter.priorityPlane < 0) == before;
    };
    const uint32_t frame = vkCtx_->getCurrentFrame();
    if (!particlePipeline_ || !m2ParticleVB_[frame]) return;
    // Diagnostic: WOWEE_M2_NO_PARTICLES=1 drops every M2 particle draw, which
    // tells a particle artifact apart from a skinned-geometry one.
    static const bool kNoParticles = envFlagEnabled("WOWEE_M2_NO_PARTICLES");
    if (kNoParticles) return;

    // Collect all particles from all instances, grouped by texture+blend.
    // Reuse persistent map - keep the bucket structure, drop last frame's set.
    for (auto& [k, g] : particleGroups_) {
        g.preAllocSet = VK_NULL_HANDLE;
    }
    auto& groups = particleGroups_;

    // Size the upload for this frame. A fixed shared cap discarded later
    // instances (including newly created loot/spell effects) once scenery filled it.
    size_t requiredVertices = 0;
    for (size_t idx : particleInstanceIndices_) {
        if (idx >= instances.size()) continue;
        const auto& inst = instances[idx];
        if (!inst.cachedModel) continue;
        const glm::vec3 distance = inst.position - cachedCamPos_;
        if (glm::dot(distance, distance) > cachedMaxRenderDistSq_) continue;
        if (outsideVisibleRegion(inst.position) || roomCulled(inst)) continue;
        for (const auto& particle : inst.particles) {
            const auto& emitter = inst.cachedModel->particleEmitters.at(particle.emitterIndex);
            requiredVertices += emitter.headOrTail == 2 ? 2 : 1;
        }
    }
    if (requiredVertices > particleVertexCapacity_[frame]) {
        const size_t capacity = std::max(requiredVertices, particleVertexCapacity_[frame] * 2);
        VkBufferCreateInfo info{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = capacity * kParticleVertexFloats * sizeof(float);
        info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        VmaAllocationCreateInfo allocationInfo{};
        allocationInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        allocationInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
        ::VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VmaAllocationInfo mapped{};
        const auto allocator = vkCtx_->getAllocator();
        const VkResult result = vmaCreateBuffer(allocator, &info, &allocationInfo,
                                                &buffer, &allocation, &mapped);
        if (result != VK_SUCCESS) {
            LOG_ERROR("Particle vertex buffer allocation failed: ", result,
                      " vertices=", capacity);
            throw std::runtime_error("Particle vertex buffer allocation failed");
        }
        const ::VkBuffer previousBuffer = m2ParticleVB_[frame];
        const VmaAllocation previousAllocation = m2ParticleVBAlloc_[frame];
        vkCtx_->deferAfterAllFrameFences([allocator, previousBuffer, previousAllocation] {
            vmaDestroyBuffer(allocator, previousBuffer, previousAllocation);
        });
        m2ParticleVB_[frame] = buffer;
        m2ParticleVBAlloc_[frame] = allocation;
        m2ParticleVBMapped_[frame] = mapped.pMappedData;
        particleVertexCapacity_[frame] = capacity;
        LOG_INFO("Particle vertex capacity grown to ", capacity,
                 " for ", requiredVertices, " live particles");
    }

    // Written straight into the mapped buffer rather than accumulated into a
    // vector per group and copied in afterwards; see ParticleRun.
    if (!m2ParticleVBMapped_[frame]) return;
    float* const vbBase = static_cast<float*>(m2ParticleVBMapped_[frame]);
    // The second call appends after the first: both phases' draws read this
    // frame's buffer, so neither may overwrite the other's vertices.
    if (before) particleBeforeTransparentEnd_ = 0;
    uint32_t vbWritten = before ? 0 : particleBeforeTransparentEnd_;
    particleRuns_.clear();
    ParticleGroup* runGroup = nullptr;

    size_t totalParticles = 0;

    // Only the instances that carry emitters, not every instance in the world.
    //
    // particleInstanceIndices_ is built and maintained for exactly this and
    // the update path above already uses it; this pass walked all 66211
    // instances instead, asking each whether its particle vector was empty.
    // That walk was 2.0ms of a 16ms frame - two thirds of the M2 worker,
    // which is the critical path of renderWorld.
    for (size_t idx : particleInstanceIndices_) {
        if (idx >= instances.size()) continue;
        auto& inst = instances[idx];
        if (inst.particles.empty()) continue;
        if (!inst.cachedModel) continue;
        const auto& gpu = *inst.cachedModel;
        const glm::vec3 distance = inst.position - cachedCamPos_;
        if (glm::dot(distance, distance) > cachedMaxRenderDistSq_) continue;
        if (outsideVisibleRegion(inst.position) || roomCulled(inst)) continue;

        // Cache the last emitter's per-emitter state so adjacent particles
        // sharing an emitter (the common case - particles from one source
        // cluster together) skip the texture/key/map-lookup work entirely.
        int lastEmitterIdx = -1;
        VkTexture* cachedTex = nullptr;
        uint16_t cachedTilesX = 1, cachedTilesY = 1;
        uint32_t cachedTotalTiles = 1;
        uint16_t cachedBlendType = 0;
        const pipeline::M2ParticleEmitter* cachedEm = nullptr;
        ParticleGroup* cachedGroup = nullptr;
        // animFrame depends only on inst.animTime + totalTiles, so it's also
        // emitter-stable within one frame.
        glm::mat4 cachedParticleTransform(1.0f);

        // How far this instance's particles actually reach, against how far
        // the model says it extends. A fire twice the height of the hut behind
        // it is either particles outliving their authored lifespan, flying at
        // the wrong speed, or drawn at the wrong size - and none of those says
        // which model it is. This does.
        float highestParticleZ = -std::numeric_limits<float>::max();
        float widestParticle = 0.0f;
        const float placementScale = particlePlacementScale(inst.modelMatrix);
        // A doodad in an interior WMO group is lit by its MODD colour, and
        // particles of emitters without kParticleOwnColor take the same
        // colour: BFD_WaterFalls01 splash (flags 0x420, authored 190,218,220)
        // shows the blue of its placement (84,125,223) in the original client.
        // Flames keep theirs: UBRS Torch.m2 (0x21, MODD 255,153,73) burns
        // yellow-white, and BFD candle01 (0x39, MODD 113,147,204) white.
        const glm::vec3 placementLight = (inst.indoorLighting & 2u)
            ? glm::vec3((inst.indoorLighting >> 8) & 255u, (inst.indoorLighting >> 16) & 255u,
                        (inst.indoorLighting >> 24) & 255u) / 255.0f
            : glm::vec3(1.0f);

        for (const auto& p : inst.particles) {
            if (p.emitterIndex < 0 || p.emitterIndex >= static_cast<int>(gpu.particleEmitters.size())) continue;
            if (!inPhase(gpu.particleEmitters[p.emitterIndex])) continue;
            // Drawn as its geometry model in renderParticleMeshes.
            if (p.emitterIndex < static_cast<int>(gpu.particleGeometry.size()) &&
                !gpu.particleGeometry[p.emitterIndex].indices.empty()) continue;

            if (p.emitterIndex != lastEmitterIdx) {
                lastEmitterIdx = p.emitterIndex;
                cachedEm = &gpu.particleEmitters[p.emitterIndex];
                glm::mat4 bone(1.0f);
                if ((cachedEm->flags & kParticleNoTrail) && !gpu.bones.empty()) {
                    bone = inst.boneMatrices.at(cachedEm->bone);
                }
                cachedParticleTransform = particleRenderTransform(
                    cachedEm->flags, inst.modelMatrix, bone);

                cachedTex = whiteTexture_.get();
                if (p.emitterIndex < static_cast<int>(gpu.particleTextures.size())) {
                    cachedTex = gpu.particleTextures[p.emitterIndex];
                }
                cachedTilesX = std::max<uint16_t>(cachedEm->textureCols, 1);
                cachedTilesY = std::max<uint16_t>(cachedEm->textureRows, 1);
                cachedTotalTiles = static_cast<uint32_t>(cachedTilesX) *
                                   static_cast<uint32_t>(cachedTilesY);
                cachedBlendType = cachedEm->blendingType;
                ParticleGroupKey key{.texture = cachedTex, .blendType = static_cast<uint8_t>(cachedBlendType), .tilesX = cachedTilesX, .tilesY = cachedTilesY};
                cachedGroup = &groups[key];
                cachedGroup->texture = cachedTex;
                cachedGroup->blendType = cachedBlendType;
                cachedGroup->tilesX = cachedTilesX;
                cachedGroup->tilesY = cachedTilesY;
                if (cachedGroup->preAllocSet == VK_NULL_HANDLE &&
                    p.emitterIndex < static_cast<int>(gpu.particleTexSets.size())) {
                    cachedGroup->preAllocSet = gpu.particleTexSets[p.emitterIndex];
                }

            }

            const auto& em = *cachedEm;
            float lifeRatio = p.life / std::max(p.maxLife, 0.001f);
            glm::vec3 color = interpFBlockVec3(em.particleColor, lifeRatio);
            if (!(em.flags & kParticleOwnColor)) color *= placementLight;
            // Scaled by the instance fade, as its mesh is: an owner fading
            // out (a creature's death, a hidden player) takes its particles
            // with it.
            float alpha = std::min(interpFBlockFloat(em.particleAlpha, lifeRatio), 1.0f) * inst.fade;
            // Authored lifecycle size for every model. GnomereganVent smoke
            // grows 2.78 -> 4.44 -> 8.33; a former 1.5 cap on models not named
            // like spell effects shrank it to thin puffs.
            const float twinkle = particleTwinkleScale(p.twinkleSeed, p.life, em.twinkleSpeed,
                                                       em.twinkleScaleMin, em.twinkleScaleMax);
            const float scale = interpFBlockFloat(em.particleScale, lifeRatio) * twinkle;

            assert(vbWritten < particleVertexCapacity_[frame]);
            // A run per stretch of particles sharing a group. The group only
            // changes when the emitter does, and particles from one emitter
            // are adjacent, so this closes a run about once per emitter.
            if (cachedGroup != runGroup) {
                runGroup = cachedGroup;
                particleRuns_.push_back({.group = cachedGroup, .first = vbWritten, .count = 0});
            }
            const glm::vec3 worldPosition = glm::vec3(cachedParticleTransform * glm::vec4(p.position, 1));
            const glm::vec3 worldVelocity = glm::mat3(cachedParticleTransform) * p.velocity;
            highestParticleZ = std::max(highestParticleZ, worldPosition.z);
            widestParticle = std::max(widestParticle, scale);

            const bool xyQuad = (em.flags & kParticleXYQuad) != 0;
            const unsigned sprites = em.headOrTail == 2 ? 2 : 1;
            for (unsigned sprite = 0; sprite < sprites; ++sprite) {
                assert(vbWritten < particleVertexCapacity_[frame]);
                float* vd = vbBase + static_cast<size_t>(vbWritten) * kParticleVertexFloats;
                vd[0] = worldPosition.x;
                vd[1] = worldPosition.y;
                vd[2] = worldPosition.z;
                vd[3] = color.r;
                vd[4] = color.g;
                vd[5] = color.b;
                vd[6] = alpha;
                // XYQuad (flag 0x1000) sprites lie in the model's XY plane
                // instead of facing the camera. Marked by a negative size;
                // the tail slot carries the model X axis for the quad.
                vd[7] = xyQuad ? -particleSpriteHalfWidth(scale, placementScale)
                               : particleSpriteHalfWidth(scale, placementScale);
                const bool tail = em.headOrTail == 1 || (em.headOrTail == 2 && sprite == 1);
                vd[8] = static_cast<float>(particleCell(em, lifeRatio, tail, cachedTotalTiles));
                const glm::vec3 displacement = xyQuad ? glm::normalize(glm::vec3(inst.modelMatrix[0]))
                    : tail ? particleTailVector(em.flags, worldVelocity, em.tailLength,
                        glm::vec3(cachedParticleTransform * glm::vec4(p.birthPosition, 1)) - worldPosition)
                    : glm::vec3(0);
                vd[9] = displacement.x;
                vd[10] = displacement.y;
                vd[11] = displacement.z;
                vd[12] = particleRotation(em.spin, p.life);
                ++vbWritten;
                ++particleRuns_.back().count;
            }
            totalParticles++;
        }

        // Opt-in effect diagnostics for captured visual comparisons.
        static const char* traceModel = std::getenv("WOWEE_PARTICLE_TRACE");
        if (!before && traceModel && gpu.name == traceModel) {
            static uint32_t traceFrame = 0;
            if (++traceFrame % 60 == 1) {
                LOG_INFO("Particle trace: model=", gpu.name, " origin=", inst.position.x,
                         ",", inst.position.y, ",", inst.position.z, " count=", inst.particles.size(),
                         " highestZ=", highestParticleZ, " largest=", widestParticle,
                         " scale=", inst.scale, " time=", inst.animTime);
                for (size_t ei = 0; ei < gpu.particleEmitters.size(); ++ei) {
                    const auto& em = gpu.particleEmitters[ei];
                    LOG_INFO("Particle emitter ", ei, " color=",
                        interpFBlockVec3(em.particleColor, 0.5f).x, ",",
                        interpFBlockVec3(em.particleColor, 0.5f).y, ",",
                        interpFBlockVec3(em.particleColor, 0.5f).z,
                        " alpha=", interpFBlockFloat(em.particleAlpha, 0.5f),
                        " texture=", static_cast<const void*>(gpu.particleTextures[ei]));
                }
            }
        }

        // Said once per model, on an absolute reach rather than a ratio
        // against the model's own bounds. A bonfire's authored box is 25 to 32
        // yards tall - it has to hold the flame's full extent - so a ratio
        // test can never trip on the one model anybody is complaining about.
        // Nothing that stands on the ground should be throwing particles eight
        // yards into the air.
        if (!before && highestParticleZ > -std::numeric_limits<float>::max()) {
            const float reach = highestParticleZ - inst.position.z;
            if (reach > 8.0f) {
                static std::set<std::string> saidTall;
                if (saidTall.insert(gpu.name).second) {
                    LOG_WARNING("Particles reach ", reach, " yd above '", gpu.name,
                                "' (model bound ", gpu.boundMax.z, " yd, scale ", inst.scale,
                                ", ", gpu.particleEmitters.size(), " emitters, ",
                                inst.particles.size(), " live, largest ", widestParticle, " yd)");
                }
            }
        }
    }

    // Periodic diagnostic: spell effect particle count
    {
        static uint32_t spellParticleDiagFrame_ = 0;
        if (++spellParticleDiagFrame_ % 300 == 1) {
            size_t spellPtc = 0;
            for (const auto& inst : instances) {
                if (inst.cachedModel && inst.cachedModel->isSpellEffect)
                    spellPtc += inst.particles.size();
            }
            if (spellPtc > 0) {
                LOG_INFO("SpellEffect: rendering ", spellPtc, " spell particles (",
                         totalParticles, " total)");
            }
        }
    }

    if (before) particleBeforeTransparentEnd_ = vbWritten;
    if (totalParticles == 0) return;

    // Bind per-frame set (set 0) for particle pipeline
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            particlePipelineLayout_, 0, 1, &perFrameSet, 0, nullptr);

    VkDeviceSize vbOffset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &m2ParticleVB_[frame], &vbOffset);

    VkPipeline currentPipeline = VK_NULL_HANDLE;

    for (auto& run : particleRuns_) {
        if (run.count == 0 || !run.group) continue;
        ParticleGroup& group = *run.group;

        uint8_t blendType = group.blendType;
        VkPipeline desiredPipeline = VK_NULL_HANDLE;
        switch (particleBlendPipeline(blendType)) {
            case ParticleBlendPipeline::Alpha: desiredPipeline = particlePipeline_; break;
            case ParticleBlendPipeline::Additive: desiredPipeline = particleAdditivePipeline_; break;
            case ParticleBlendPipeline::Mod: desiredPipeline = particleModPipeline_; break;
            case ParticleBlendPipeline::Mod2x: desiredPipeline = particleMod2xPipeline_; break;
        }
        if (desiredPipeline != currentPipeline) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, desiredPipeline);
            currentPipeline = desiredPipeline;
        }

        // The emitter's own set, allocated at model load. Drawing without it
        // would sample whatever set the previous run bound.
        const VkDescriptorSet texSet = group.preAllocSet;
        if (texSet == VK_NULL_HANDLE) {
            static core::LogBudget missingSetBudget(8, "particle runs without a texture set");
            if (missingSetBudget.take()) {
                LOG_ERROR("Particle run skipped: no descriptor set for texture ",
                          static_cast<const void*>(group.texture), " blend=", int(blendType));
            }
            continue;
        }
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                particlePipelineLayout_, 1, 1, &texSet, 0, nullptr);

        // Push constants: tileCount + M2 blend mode
        struct { float tileX, tileY; int blendMode; float viewportHeight; } pc = {
            .tileX = static_cast<float>(group.tilesX), .tileY = static_cast<float>(group.tilesY),
            .blendMode = blendType,
            .viewportHeight = static_cast<float>(vkCtx_->getSwapchainExtent().height)
        };
        vkCmdPushConstants(cmd, particlePipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(pc), &pc);

        // The vertices are already in the buffer, at this run's own offset.
        // Both used to be wrong: every group copied to offset zero and drew
        // from vertex zero, so with more than one group up they all drew
        // whatever had been copied last.
        vkCmdDraw(cmd, 6, run.count, 0, run.first);
    }
}

} // namespace rendering
} // namespace wowee
