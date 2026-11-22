// m2_renderer_internal.h - shared helpers for the m2_renderer split files.
// All functions are inline to allow inclusion in multiple translation units.
#pragma once

#include "rendering/collision_geometry.hpp"
#include "rendering/m2_billboard.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/m2_track_sampler.hpp"
#include "pipeline/m2_loader.hpp"
#include "core/profiler.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <random>
#include <unordered_set>
#include <vector>

namespace wowee {
namespace rendering {
namespace m2_internal {

inline bool opacityVaries(const pipeline::M2AnimationTrack& track) {
    return m2_track::opacityVaries(track);
}

inline bool colorVaries(const pipeline::M2AnimationTrack& track) {
    const glm::vec3* first = nullptr;
    for (const auto& seq : track.sequences) {
        for (const auto& v : seq.vec3Values) {
            if (!first) first = &v;
            else if (v != *first) return true;
        }
    }
    return false;
}

inline glm::vec3 animatedBatchTint(const M2ModelGPU& model,
                                   const M2ModelGPU::BatchGPU& batch,
                                   const M2Instance& instance) {
    return glm::clamp(
        m2_track::sampleVec3(batch.animatedColorRGB,
            instance.currentSequenceIndex, instance.animTime, instance.globalSequenceTime,
            model.globalSequenceDurations, batch.tint),
        glm::vec3(0.0f), glm::vec3(1.0f));
}

inline uint32_t packAnimatedBatchTint(const M2ModelGPU& model,
                                      const M2ModelGPU::BatchGPU& batch,
                                      const M2Instance& instance) {
    if (batch.animatedColorRGB.sequences.empty()) return 0u;
    const auto rgb = glm::uvec3(glm::round(animatedBatchTint(model, batch, instance) * 255.0f));
    return (1u << 24) | rgb.r | (rgb.g << 8) | (rgb.b << 16);
}

inline float animatedBatchOpacity(const M2ModelGPU& model,
                                  const M2ModelGPU::BatchGPU& batch,
                                  const M2Instance& instance) {
    return m2_track::sampleFloat(batch.animatedColorAlpha,
        instance.currentSequenceIndex, instance.animTime, instance.globalSequenceTime,
        model.globalSequenceDurations, 1.0f) *
        m2_track::sampleFloat(batch.animatedTextureWeight,
        instance.currentSequenceIndex, instance.animTime, instance.globalSequenceTime,
        model.globalSequenceDurations, 1.0f);
}

// ---- RNG helpers ----
inline std::mt19937& rng() {
    static std::mt19937 gen(std::random_device{}());
    return gen;
}
inline uint32_t randRange(uint32_t maxExclusive) {
    if (maxExclusive == 0) return 0;
    return std::uniform_int_distribution<uint32_t>(0, maxExclusive - 1)(rng());
}
inline float randFloat(float lo, float hi) {
    return std::uniform_real_distribution<float>(lo, hi)(rng());
}

// World-space center of a skinned batch. Glow cards on swinging lanterns are
// bone animated; using only modelMatrix * bind-pose center leaves both their
// sprite and the pool of local light fixed at the fixture origin.
inline glm::vec3 animatedBatchWorldCenter(const M2Instance& instance,
                                          const M2ModelGPU::BatchGPU& batch) {
    if (batch.lightBoneAnchors.empty()) {
        return glm::vec3(instance.modelMatrix * glm::vec4(batch.center, 1.0f));
    }

    glm::vec4 animatedCenter(0.0f);
    for (const auto& anchor : batch.lightBoneAnchors) {
        if (anchor.bone < instance.boneMatrices.size()) {
            animatedCenter += instance.boneMatrices[anchor.bone] * anchor.weightedPoint;
        } else {
            animatedCenter += anchor.weightedPoint;
        }
    }
    return glm::vec3(instance.modelMatrix * animatedCenter);
}

// Lanterns are broad point lights, so the small physical displacement of a
// swinging bulb is almost invisible on the ground. Follow the authored hanging
// chain from its fixed suspension pivot to the animated glow tip, then project
// that direction toward the model's ground plane. The glow sprite remains at
// animatedBatchWorldCenter(); only the pool of light uses this projected center.
inline glm::vec3 animatedBatchLightWorldCenter(const M2Instance& instance,
                                               const M2ModelGPU::BatchGPU& batch) {
    glm::vec3 tip = animatedBatchWorldCenter(instance, batch);
    if (batch.lightSuspensionBone == UINT16_MAX ||
        batch.lightSuspensionBone >= instance.boneMatrices.size()) {
        return tip;
    }

    const glm::vec3 suspension = glm::vec3(
        instance.modelMatrix * instance.boneMatrices[batch.lightSuspensionBone] *
        glm::vec4(batch.lightSuspensionPoint, 1.0f));
    const glm::vec3 chain = tip - suspension;
    if (chain.z >= -0.05f) return tip;

    const float groundZ = glm::vec3(instance.modelMatrix[3]).z;
    const float groundDrop = tip.z - groundZ;
    if (groundDrop <= 0.0f) return tip;

    // Deliberately amplify the projected travel. At the authored Darkshire
    // swing amplitude a physically exact pool moves less than one terrain unit,
    // which disappears inside the broad local-light falloff.
    constexpr float kLanternPoolSwingExaggeration = 3.0f;
    glm::vec2 projectedOffset(chain.x, chain.y);
    projectedOffset *= (groundDrop / -chain.z) * kLanternPoolSwingExaggeration;
    const float offsetLength = glm::length(projectedOffset);
    const float maxOffset = std::max(0.75f, 4.0f * instance.scale);
    if (offsetLength > maxOffset) projectedOffset *= maxOffset / offsetLength;
    tip.x += projectedOffset.x;
    tip.y += projectedOffset.y;
    return tip;
}

// ---- Constants ----

// ---- Geometry / collision helpers ----

inline float computeGroundDetailDownOffset(const M2ModelGPU& model, float scale) {
    const float pivotComp = glm::clamp(std::max(0.0f, model.boundMin.z * scale), 0.0f, 0.10f);
    const float terrainSink = 0.03f;
    return pivotComp + terrainSink;
}

// Whether a model's instances stop the player or the camera. Only authored
// collision triangles (M2 header boundingTriangles) collide; the CMaNGOS vmap
// extractor reads the same triangles for server heights and line of sight.
// A model that ships none - bushes, rugs, the
// Blackrock Depths rune glyphs - is walked through, and its bounds are never
// a floor or a wall: a box invented from them lifted players onto the runes.
// Placed doodads collide by their authored triangles whatever their name:
// GnomeStructuralSpotLight02 authors 56. Spell visuals spawned at runtime
// do not block.
inline bool m2BlocksMovement(const M2ModelGPU& model) {
    return model.collision.valid() && !model.isInvisibleTrap && !model.isRuntimeSpellVisual;
}

inline void getTightCollisionBounds(const M2ModelGPU& model, glm::vec3& outMin, glm::vec3& outMax) {
    // Broad-phase bounds must contain every authored collision triangle.
    // A model without any is walked through, so its bounds only file it in
    // the spatial grid for picking and clearings: its own vertex bounds.
    if (model.collision.valid()) {
        outMin = model.collision.boundsMin;
        outMax = model.collision.boundsMax;
        return;
    }
    outMin = model.boundMin;
    outMax = model.boundMax;
}

inline bool segmentIntersectsAABB(const glm::vec3& from, const glm::vec3& to,
                                  const glm::vec3& bmin, const glm::vec3& bmax,
                                  float& outEnterT) {
    glm::vec3 d = to - from;
    float tEnter = 0.0f;
    float tExit = 1.0f;

    for (int axis = 0; axis < 3; axis++) {
        if (std::abs(d[axis]) < 1e-6f) {
            if (from[axis] < bmin[axis] || from[axis] > bmax[axis]) {
                return false;
            }
            continue;
        }

        float inv = 1.0f / d[axis];
        float t0 = (bmin[axis] - from[axis]) * inv;
        float t1 = (bmax[axis] - from[axis]) * inv;
        if (t0 > t1) std::swap(t0, t1);

        tEnter = std::max(tEnter, t0);
        tExit = std::min(tExit, t1);
        if (tEnter > tExit) return false;
    }

    outEnterT = tEnter;
    return tExit >= 0.0f && tEnter <= 1.0f;
}

inline void transformAABB(const glm::mat4& modelMatrix,
                          const glm::vec3& localMin,
                          const glm::vec3& localMax,
                          glm::vec3& outMin,
                          glm::vec3& outMax) {
    const glm::vec3 corners[8] = {
        {localMin.x, localMin.y, localMin.z},
        {localMin.x, localMin.y, localMax.z},
        {localMin.x, localMax.y, localMin.z},
        {localMin.x, localMax.y, localMax.z},
        {localMax.x, localMin.y, localMin.z},
        {localMax.x, localMin.y, localMax.z},
        {localMax.x, localMax.y, localMin.z},
        {localMax.x, localMax.y, localMax.z}
    };

    outMin = glm::vec3(std::numeric_limits<float>::max());
    outMax = glm::vec3(-std::numeric_limits<float>::max());
    for (const auto& c : corners) {
        glm::vec3 wc = glm::vec3(modelMatrix * glm::vec4(c, 1.0f));
        outMin = glm::min(outMin, wc);
        outMax = glm::max(outMax, wc);
    }
}

// Closest point on triangle to a point (Ericson, Real-Time Collision Detection §5.1.5).
inline glm::vec3 closestPointOnTriangle(const glm::vec3& p,
                                         const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
    glm::vec3 ab = b - a, ac = c - a, ap = p - a;
    float d1 = glm::dot(ab, ap), d2 = glm::dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return a;
    glm::vec3 bp = p - b;
    float d3 = glm::dot(ab, bp), d4 = glm::dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return b;
    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        float v = d1 / (d1 - d3);
        return a + v * ab;
    }
    glm::vec3 cp = p - c;
    float d5 = glm::dot(ab, cp), d6 = glm::dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return c;
    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        float w = d2 / (d2 - d6);
        return a + w * ac;
    }
    float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return b + w * (c - b);
    }
    float denom = 1.0f / (va + vb + vc);
    float v = vb * denom;
    float w = vc * denom;
    return a + ab * v + ac * w;
}

// ---- Thread-local scratch buffers for collision queries ----
// Defined in m2_renderer_instance.cpp (inline thread_local causes LLD linker
// errors on Windows ARM64, so the definitions live in the single TU that uses them).

/// M2 bone flags this renderer acts on.
///
// Persistent spell effects may author Stand intro, Hold loop, Decay outro.
// Doodad random phase and idle variations do not apply to these instances.
inline void startStateEffect(const M2ModelGPU& model, M2Instance& instance) {
    if (model.sequences.empty()) return;
    int stand = -1, hold = -1;
    for (int i=0; i<static_cast<int>(model.sequences.size()); ++i) {
        if (model.sequences[i].id == 0 && stand < 0) stand = i;
        if (model.sequences[i].id == 158 && hold < 0) hold = i;
    }
    const int first = stand >= 0 ? stand : (hold >= 0 ? hold : 0);
    instance.stateEffectAnimation = true;
    instance.currentSequenceIndex = first;
    instance.idleSequenceIndex = hold >= 0 ? hold : first;
    instance.playingVariation = first != instance.idleSequenceIndex;
    instance.holdAtEnd = false;
    instance.animDuration = static_cast<float>(model.sequences[first].duration);
    instance.animTime = 0;
    instance.animSpeed = 1;
}

using rendering::kM2BoneSphericalBillboard;
using rendering::kM2BoneCylindricalBillboardX;
using rendering::kM2BoneCylindricalBillboardY;
using rendering::kM2BoneCylindricalBillboardZ;

/// Bone transforms for one instance.
///
/// `cameraPosWorld` is what a billboard bone turns toward; pass nullptr and
/// those bones keep their authored orientation.
inline void computeBoneMatrices(const M2ModelGPU& model, M2Instance& instance,
                                const glm::vec3* cameraPosWorld = nullptr) {
    ZoneScopedN("M2::computeBoneMatrices");
    size_t numBones = std::min(model.bones.size(), size_t(kMaxBonesPerInstance));
    if (numBones == 0) return;
    instance.boneMatrices.resize(numBones);
    const auto& gsd = model.globalSequenceDurations;

    for (size_t i = 0; i < numBones; i++) {
        const auto& bone = model.bones[i];
        const float boneTime = instance.animTime;
        glm::vec3 trans = m2_track::sampleVec3(
            bone.translation, instance.currentSequenceIndex, boneTime,
            instance.globalSequenceTime, gsd, glm::vec3(0.0f));
        glm::quat rot = m2_track::sampleQuat(
            bone.rotation, instance.currentSequenceIndex, boneTime,
            instance.globalSequenceTime, gsd);
        glm::vec3 scl = m2_track::sampleVec3(
            bone.scale, instance.currentSequenceIndex, boneTime,
            instance.globalSequenceTime, gsd, glm::vec3(1.0f));

        // Zero keys collapse geometry during authored hidden phases; negative
        // axes mirror it. Only an absent track uses the identity default above.

        glm::mat4 local = glm::translate(glm::mat4(1.0f), bone.pivot);
        local = glm::translate(local, trans);
        if (cameraPosWorld && isBillboardBone(bone.flags)) {
            const glm::mat4 parent = bone.parentBone >= 0 && static_cast<size_t>(bone.parentBone) < numBones
                ? instance.boneMatrices[bone.parentBone] : glm::mat4(1.0f);
            const glm::vec3 camModel =
                glm::vec3(instance.invModelMatrix * glm::vec4(*cameraPosWorld, 1.0f));
            const glm::vec3 upModel = glm::normalize(glm::vec3(instance.invModelMatrix * glm::vec4(0, 0, 1, 0)));
            instance.boneMatrices[i] = billboardBoneMatrix(bone.flags, bone.pivot, trans, scl, parent, camModel, upModel);
            continue;
        } else {
            local *= glm::toMat4(rot);
        }
        local = glm::scale(local, scl);
        local = glm::translate(local, -bone.pivot);

        if (bone.parentBone >= 0 && static_cast<size_t>(bone.parentBone) < numBones) {
            instance.boneMatrices[i] = instance.boneMatrices[bone.parentBone] * local;
        } else {
            instance.boneMatrices[i] = local;
        }
    }
    instance.bonesDirty[0] = instance.bonesDirty[1] = true;
}

// Firelight guttering for lamps, torches and braziers.
//
// The phase is hashed from the fixture's world position so no two lights ever
// pulse together - a synchronised row of street lamps reads as a rendering
// artifact rather than firelight - and it stays stable frame to frame because
// it derives from a fixed position rather than a counter.
//
// Two detuned sines with periods of roughly 3.7 and 1.6 seconds, sharing no
// common multiple over any watchable span. Slower than this and the eye adapts
// to the change and stops seeing it at all - a 15-second swell is invisible no
// matter how deep it goes - while faster reads as a strobe.
//
// phaseSeed must be a position that does not move frame to frame - the fixture's
// placement, never an animated bone centre or a sprite offset toward the camera.
// The hash is chaotic by design, so a seed that drifts even slightly re-rolls the
// phase every frame and the result is a strobe rather than a flicker. It is also
// quantised to a one-unit grid here as insurance against a caller passing
// something that creeps.
inline float lampFlicker(const glm::vec3& phaseSeed, float seconds,
                         float base, float slowAmp, float fastAmp) {
    const glm::vec3 cell = glm::floor(phaseSeed);
    float h = std::sin(cell.x * 12.9898f + cell.y * 78.233f +
                       cell.z * 37.719f) * 43758.5453f;
    const float phase = (h - std::floor(h)) * 6.2831853f;
    return base + slowAmp * std::sin(seconds * 1.70f + phase)
                + fastAmp * std::sin(seconds * 3.90f + phase * 1.7f);
}

/// Seconds since process start, shared by the flicker animations.
inline float lampFlickerClockSeconds() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
}

} // namespace m2_internal

// Pull all symbols into the rendering namespace so existing code compiles unchanged
using namespace m2_internal;

} // namespace rendering
} // namespace wowee
