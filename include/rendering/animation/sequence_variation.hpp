#pragma once

#include "pipeline/m2_loader.hpp"

#include <cassert>
#include <numeric>
#include <optional>
#include <random>
#include <vector>

namespace wowee::rendering {

// A looping animation's next sequence: one of the model's sequences with
// this animation id, drawn by M2 frequency, with the number of extra plays
// drawn from its replay range. nullopt when the model has none with weight.
// Stand fidgets and DoomGuardOutland.m2's two Walks (0: on its feet,
// 33: wings beating, replay 3-5) are such variations.
struct SequenceVariation {
    int index;
    uint32_t repeats;
};

inline std::optional<SequenceVariation> drawSequenceVariation(
        const std::vector<pipeline::M2Sequence>& sequences, uint32_t animationId, std::mt19937& random) {
    std::vector<double> weights(sequences.size(), 0.0);
    for (size_t i = 0; i < sequences.size(); ++i)
        if (sequences[i].id == animationId && sequences[i].duration > 0)
            weights[i] = std::max<int>(0, sequences[i].frequency);
    if (std::accumulate(weights.begin(), weights.end(), 0.0) <= 0.0) return std::nullopt;
    const int index = std::discrete_distribution<int>(weights.begin(), weights.end())(random);
    const auto& chosen = sequences[index];
    assert(chosen.replayMin <= chosen.replayMax);
    return SequenceVariation{index, std::uniform_int_distribution<uint32_t>(chosen.replayMin, chosen.replayMax)(random)};
}

} // namespace wowee::rendering
