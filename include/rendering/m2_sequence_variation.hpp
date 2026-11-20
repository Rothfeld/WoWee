#pragma once

#include "pipeline/m2_loader.hpp"

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace wowee::rendering {

/// Collect all playable variation sequence indices sharing the same animation ID
/// as `baseSequenceIndex`.
///
/// WoW M2 models chain idle/stand variations through `M2Sequence::nextAnimation`
/// (terminated by -1) with per-variation 15-bit `frequency` weights. If the
/// linked chain contains only the base sequence, fall back to scanning all
/// sequences with the same animation ID and positive duration.
inline std::vector<int> collectSequenceVariations(
    std::span<const pipeline::M2Sequence> sequences,
    int baseSequenceIndex) {
    std::vector<int> variations;
    const int count = static_cast<int>(sequences.size());
    if (baseSequenceIndex < 0 || baseSequenceIndex >= count) return variations;

    const uint32_t targetId = sequences[baseSequenceIndex].id;
    std::vector<uint8_t> visited(sequences.size(), 0);

    int cur = baseSequenceIndex;
    while (cur >= 0 && cur < count && !visited[cur]) {
        visited[cur] = 1;
        if (sequences[cur].id == targetId && sequences[cur].duration > 0) {
            variations.push_back(cur);
        }
        cur = sequences[cur].nextAnimation;
    }

    if (variations.size() <= 1) {
        variations.clear();
        for (int i = 0; i < count; ++i) {
            if (sequences[i].id == targetId && sequences[i].duration > 0) {
                variations.push_back(i);
            }
        }
    }

    return variations;
}

/// Select the next variation index for a looping sequence using the authored
/// `M2Sequence::frequency` weights. Falls back to uniform selection if all
/// candidate weights are zero or negative.
inline int selectSequenceVariation(
    std::span<const pipeline::M2Sequence> sequences,
    int baseSequenceIndex,
    uint32_t roll) {
    const std::vector<int> candidates = collectSequenceVariations(sequences, baseSequenceIndex);
    if (candidates.empty()) return baseSequenceIndex;
    if (candidates.size() == 1) return candidates[0];

    uint64_t totalWeight = 0;
    for (int idx : candidates) {
        if (sequences[idx].frequency > 0) {
            totalWeight += static_cast<uint64_t>(sequences[idx].frequency);
        }
    }

    if (totalWeight == 0) {
        return candidates[roll % candidates.size()];
    }

    const uint64_t target = static_cast<uint64_t>(roll) % totalWeight;
    uint64_t cumulative = 0;
    for (int idx : candidates) {
        if (sequences[idx].frequency > 0) {
            cumulative += static_cast<uint64_t>(sequences[idx].frequency);
            if (target < cumulative) {
                return idx;
            }
        }
    }
    return candidates.back();
}

} // namespace wowee::rendering
