#pragma once

#include <cstdint>
#include <limits>

namespace wowee::rendering {

// Sort key for Pass 2 transparent M2 batches across all visible instances.
// priorityPlane is the M2 batch's signed priorityPlane (negative draws under
// plane 0, positive draws over plane 0). Within the same plane, viewDepth is
// the batch's transformed submesh sortCenter in view space (more negative =
// further from camera = drawn earlier, back-to-front).
struct M2TransparentBatchKey {
    int8_t priorityPlane = 0;
    float viewDepth = 0.0f;
};

inline bool m2TransparentBatchBefore(const M2TransparentBatchKey& a,
                                     const M2TransparentBatchKey& b) {
    if (a.priorityPlane != b.priorityPlane) {
        return a.priorityPlane < b.priorityPlane;
    }
    return a.viewDepth < b.viewDepth;
}

// Blended character batches live on priority plane 0. Negative-plane M2
// batches draw before any blended character; plane-0 batches interleave with
// blended characters back-to-front by viewDepth; positive-plane batches flush
// all remaining blended characters first so they composite on top.
inline bool m2CharacterFlushBeforeBatch(const M2TransparentBatchKey& key,
                                        float& flushDepth) {
    if (key.priorityPlane < 0) {
        return false;
    }
    flushDepth = (key.priorityPlane > 0)
        ? std::numeric_limits<float>::infinity()
        : key.viewDepth;
    return true;
}

} // namespace wowee::rendering
