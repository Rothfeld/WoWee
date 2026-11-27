#pragma once

#include "pipeline/m2_loader.hpp"
#include "rendering/footfall_window.hpp"

#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace wowee::core {

/// Summary of M2 character audio events crossed during a single sequence step.
struct UnitAudioStepMask {
    bool sequenceHasEvents = false; ///< Sequence authors >= 1 character audio timestamp.
    bool crossedAny = false;        ///< At least one timestamp crossed in [fromMs, toMs].
    bool needsSurface = false;      ///< A Footstep or DeathThud timestamp crossed.
    bool needsBridge = false;       ///< A combat or emote bridge timestamp crossed.
};

[[nodiscard]] constexpr bool isSpawnerDispatchedAudioEvent(pipeline::M2AudioEventType type) noexcept {
    using T = pipeline::M2AudioEventType;
    switch (type) {
        case T::Footstep:
        case T::DeathThud:
        case T::ArmorFoley:
        case T::CustomSound:
        case T::SpellCastDirected:
        case T::Submerge:
        case T::Submerged:
        case T::Sound:
        case T::DoodadSoundOneShot:
            return true;
        default:
            return false;
    }
}

[[nodiscard]] constexpr bool isSpawnerBridgeAudioEvent(pipeline::M2AudioEventType type) noexcept {
    using T = pipeline::M2AudioEventType;
    switch (type) {
        case T::CombatSwing:
        case T::CombatAttackHit:
        case T::ArmorHit0:
        case T::ArmorHit1:
        case T::ArmorHit2:
        case T::ArmorHit3:
        case T::BowPull:
        case T::BowRelease:
        case T::SheatheLeft:
        case T::SheatheRight:
        case T::EmoteSound:
            return true;
        default:
            return false;
    }
}

/// Evaluate whether stepping a character animation layer across [ioPrevTimeMs, toMs]
/// crosses any M2 audio event timestamps, matching FootstepManager::stepCharacterAudioEvents
/// cursor rules bit-for-bit so surface raycasts and bridge callbacks run only on ticks
/// that actually cross an event.
[[nodiscard]] inline UnitAudioStepMask evaluateUnitLayerAudioStep(
    const pipeline::M2Model& model,
    int seqIdx,
    float toMs,
    bool reversed,
    bool isUpperLayer,
    bool skipFootstep,
    bool hasCustomBridge,
    bool muteEvents,
    int& ioPrevSeq,
    float& ioPrevTimeMs) noexcept {
    UnitAudioStepMask out{};
    if (seqIdx < 0 || static_cast<size_t>(seqIdx) >= model.sequences.size()) {
        ioPrevSeq = -1;
        return out;
    }

    for (const auto& ev : model.audioEvents) {
        const bool bridgeEv = isSpawnerBridgeAudioEvent(ev.type);
        if (!bridgeEv && !isSpawnerDispatchedAudioEvent(ev.type)) continue;
        if (static_cast<size_t>(seqIdx) < ev.times.size() && !ev.times[seqIdx].empty()) {
            out.sequenceHasEvents = true;
            break;
        }
    }
    if (!out.sequenceHasEvents) {
        ioPrevSeq = -1;
        return out;
    }

    const float seqDur = static_cast<float>(model.sequences[seqIdx].duration);
    float fromMs = -1.0f;
    if (ioPrevSeq == seqIdx) {
        fromMs = ioPrevTimeMs;
    } else if (toMs > 250.0f && (!reversed || (seqDur - toMs) > 250.0f)) {
        ioPrevSeq = seqIdx;
        ioPrevTimeMs = toMs;
        return out;
    }
    ioPrevSeq = seqIdx;
    ioPrevTimeMs = toMs;
    if (fromMs == toMs || muteEvents) {
        return out;
    }

    const auto mapEventTime = [&](uint32_t eventMs) -> uint32_t {
        if (!reversed || seqDur <= 0.0f) return eventMs;
        const float rev = std::max(0.0f, seqDur - static_cast<float>(eventMs));
        return static_cast<uint32_t>(std::lround(rev));
    };

    for (const auto& ev : model.audioEvents) {
        const bool bridgeEv = isSpawnerBridgeAudioEvent(ev.type);
        if (!bridgeEv && !isSpawnerDispatchedAudioEvent(ev.type)) continue;
        if (bridgeEv && !hasCustomBridge) continue;
        if (ev.type == pipeline::M2AudioEventType::Footstep && skipFootstep) continue;
        if (isUpperLayer &&
            (ev.type == pipeline::M2AudioEventType::Footstep ||
             ev.type == pipeline::M2AudioEventType::ArmorFoley)) {
            continue;
        }
        if (static_cast<size_t>(seqIdx) >= ev.times.size()) continue;
        const auto& stamps = ev.times[seqIdx];
        if (stamps.empty()) continue;

        bool crossed = false;
        for (const uint32_t rawT : stamps) {
            const uint32_t mappedT = mapEventTime(rawT);
            const bool hit = reversed
                ? rendering::footfallPassed(
                      mappedT,
                      fromMs < 0.0f ? -1.0f : std::max(0.0f, seqDur - fromMs),
                      std::max(0.0f, seqDur - toMs))
                : rendering::footfallPassed(mappedT, fromMs, toMs);
            if (hit) {
                crossed = true;
                break;
            }
        }
        if (!crossed) continue;

        out.crossedAny = true;
        if (bridgeEv) {
            out.needsBridge = true;
        }
        if ((ev.type == pipeline::M2AudioEventType::Footstep && !skipFootstep) ||
            ev.type == pipeline::M2AudioEventType::DeathThud) {
            out.needsSurface = true;
        }
    }
    return out;
}

/// Whether a cached unit surface / water sample at `cachedPos` must be refreshed
/// for `currentPos` (threshold defaults to 0.20 yards for surface, 0.50 yards for water).
[[nodiscard]] inline bool shouldRefreshCachedPositionSample(
    bool valid,
    const glm::vec3& cachedPos,
    const glm::vec3& currentPos,
    float thresholdYards) noexcept {
    if (!valid) return true;
    const glm::vec3 d = currentPos - cachedPos;
    return glm::dot(d, d) > (thresholdYards * thresholdYards);
}

} // namespace wowee::core
