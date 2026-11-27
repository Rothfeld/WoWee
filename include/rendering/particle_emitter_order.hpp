#pragma once

#include "pipeline/m2_loader.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace wowee::rendering {

/// True when an M2 particle emitter with signed `priorityPlane` belongs to the
/// `BeforeTransparent` pass (`priorityPlane < 0`) vs `BeforeWaterDepth` /
/// `AfterTransparent` (`priorityPlane >= 0`).
inline bool particleEmitterInPhase(int16_t priorityPlane, bool beforeTransparent) {
    return (priorityPlane < 0) == beforeTransparent;
}

/// Strict ordering for emitters within a single M2 model and particle phase:
/// lower signed `priorityPlane` draws first (underneath), higher `priorityPlane`
/// draws later (on top); ties preserve authored M2 emitter array order.
inline bool particleEmitterDrawsBefore(const std::vector<pipeline::M2ParticleEmitter>& emitters,
                                       size_t indexA, size_t indexB) {
    assert(indexA < emitters.size());
    assert(indexB < emitters.size());
    const int16_t planeA = emitters[indexA].priorityPlane;
    const int16_t planeB = emitters[indexB].priorityPlane;
    if (planeA != planeB) {
        return planeA < planeB;
    }
    return indexA < indexB;
}

/// Populate `outIndices` with all emitter indices of `emitters` belonging to
/// `beforeTransparent`, ordered by ascending `(priorityPlane, emitterIndex)`.
inline void sortedPhaseParticleEmitterIndices(
    const std::vector<pipeline::M2ParticleEmitter>& emitters,
    bool beforeTransparent,
    std::vector<size_t>& outIndices) {
    outIndices.clear();
    outIndices.reserve(emitters.size());
    for (size_t ei = 0; ei < emitters.size(); ++ei) {
        if (!particleEmitterInPhase(emitters[ei].priorityPlane, beforeTransparent)) {
            continue;
        }
        outIndices.push_back(ei);
    }
    std::sort(outIndices.begin(), outIndices.end(),
              [&emitters](size_t a, size_t b) {
                  return particleEmitterDrawsBefore(emitters, a, b);
              });
}

/// Scene-wide draw key for an M2 particle emitter within a `ParticlePhase`,
/// mirroring `M2TransparentBatchKey` (`m2_transparent_sort.hpp`):
/// 1. Ascending signed `priorityPlane` (higher planes composite over lower planes
///    across both a single model's emitters and overlapping spell/creature M2s).
/// 2. Descending `emitterDistanceSq` from camera (far-to-near back-to-front within
///    each priority plane, measured at the emitter's world origin so wide offset
///    emitters such as `Creature_SpellPortalLarge_Purple.m2` side pillars sort at
///    their actual world position rather than the model root).
/// 3. Ascending instance `registrationOrder` and authored `emitterIndex` on ties.
struct SceneParticleEmitterDrawKey {
    size_t visibleSlot = 0;
    size_t emitterIndex = 0;
    int16_t priorityPlane = 0;
    float emitterDistanceSq = 0.0f;
    size_t registrationOrder = 0;
};

inline bool sceneParticleEmitterDrawsBefore(const SceneParticleEmitterDrawKey& a,
                                            const SceneParticleEmitterDrawKey& b) {
    assert(std::isfinite(a.emitterDistanceSq));
    assert(std::isfinite(b.emitterDistanceSq));
    if (a.priorityPlane != b.priorityPlane) {
        return a.priorityPlane < b.priorityPlane;
    }
    if (a.emitterDistanceSq != b.emitterDistanceSq) {
        return a.emitterDistanceSq > b.emitterDistanceSq;
    }
    if (a.registrationOrder != b.registrationOrder) {
        return a.registrationOrder < b.registrationOrder;
    }
    return a.emitterIndex < b.emitterIndex;
}

} // namespace wowee::rendering
