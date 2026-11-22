#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wowee::rendering {

// Only static world/WMO doodad placements participate in M2Renderer's
// position deduplication map (instanceDedupMap_). Transient spell visual
// effects, spell-effect models, server game objects, ground detail, and
// externally posed emitters must never share an instance ID or overwrite/erase
// a static doodad's dedup entry: Mind Flay (visual 277) spawns
// Spells\ManaFunnel_Impact_Chest.m2 at SpellLeftHand (21) in both CastKit
// (429, 1.067 s) and ChannelKit (430, 4.0 s) on the same tick, so deduplicating
// them caused CastKit's 1 s expiry to retire ChannelKit's hand glow mid-channel.
[[nodiscard]] constexpr bool m2ModelUsesPositionDedup(bool allowPositionDedup,
                                                      bool isGroundDetail,
                                                      bool isSpellEffect) noexcept {
    return allowPositionDedup && !isGroundDetail && !isSpellEffect;
}

[[nodiscard]] constexpr bool m2InstanceUsesPositionDedup(bool isGroundDetail,
                                                         bool isSpellEffect,
                                                         bool isSpellVisual,
                                                         bool isGameObject,
                                                         bool externalPose) noexcept {
    return !isGroundDetail && !isSpellEffect && !isSpellVisual &&
           !isGameObject && !externalPose;
}

[[nodiscard]] constexpr float channelDurationSeconds(uint32_t durationMs) noexcept {
    return durationMs == std::numeric_limits<uint32_t>::max()
        ? std::numeric_limits<float>::infinity()
        : static_cast<float>(durationMs) / 1000.0f;
}

struct FieldChannelEntry {
    uint64_t casterGuid = 0;
    uint32_t casterInstanceId = 0;
    uint32_t visualId = 0;
    size_t sourceIndex = 0;
};

struct FieldChannelSyncPlan {
    std::vector<size_t> liveChannelIndices;
    std::vector<uint32_t> instancesToStop;
    std::vector<size_t> channelsToStart;
};

// Reconcile server UNIT_CHANNEL_SPELL state with active client channel visuals:
// - Prunes endedChannels once the server clears UNIT_CHANNEL_SPELL for that GUID,
//   and suppresses stale UNIT_CHANNEL_SPELL rows while endedChannels still holds the GUID.
// - Never tears down a newer MSG_CHANNEL_START visual whose UNIT_CHANNEL_SPELL
//   update has not arrived yet (activeVisualByInstance holds a different visualId,
//   or fieldChannels has not observed the new visualId yet).
// - Spawns infinite-duration ChannelKit visuals for units entering AOI mid-channel
//   (no MSG_CHANNEL_START) as soon as casterInstanceId becomes available, and stops
//   them when UNIT_CHANNEL_SPELL clears or changes.
inline FieldChannelSyncPlan reconcileFieldChannels(
    const std::vector<FieldChannelEntry>& rawEntries,
    const std::unordered_map<uint32_t, uint32_t>& activeVisualByInstance,
    const std::unordered_set<uint32_t>& instancesWithChannelEffects,
    std::unordered_map<uint32_t, uint32_t>& fieldChannels,
    std::unordered_set<uint32_t>& heldChannelKits,
    std::unordered_set<uint64_t>& endedChannels) {
    FieldChannelSyncPlan plan;

    std::erase_if(endedChannels, [&](uint64_t guid) {
        return std::none_of(rawEntries.begin(), rawEntries.end(),
                            [&](const FieldChannelEntry& e) { return e.casterGuid == guid; });
    });

    std::unordered_map<uint32_t, uint32_t> current;
    for (const auto& entry : rawEntries) {
        if (endedChannels.contains(entry.casterGuid)) continue;
        plan.liveChannelIndices.push_back(entry.sourceIndex);
        if (entry.casterInstanceId != 0) {
            current[entry.casterInstanceId] = entry.visualId;
        }
    }

    for (auto it = fieldChannels.begin(); it != fieldChannels.end();) {
        const uint32_t inst = it->first;
        const uint32_t oldVisualId = it->second;
        const auto now = current.find(inst);
        if (now == current.end() || now->second != oldVisualId) {
            const bool wasHeld = heldChannelKits.erase(inst) > 0;
            const auto activeIt = activeVisualByInstance.find(inst);
            const bool stillPlayingOld =
                activeIt != activeVisualByInstance.end() && activeIt->second == oldVisualId;
            it = fieldChannels.erase(it);
            if (wasHeld || stillPlayingOld) {
                plan.instancesToStop.push_back(inst);
            }
        } else {
            ++it;
        }
    }

    for (const size_t idx : plan.liveChannelIndices) {
        const auto& entry = rawEntries[idx];
        if (entry.casterInstanceId == 0 || fieldChannels.contains(entry.casterInstanceId)) {
            continue;
        }
        const bool stoppedThisTick =
            std::find(plan.instancesToStop.begin(), plan.instancesToStop.end(),
                      entry.casterInstanceId) != plan.instancesToStop.end();
        if (!stoppedThisTick) {
            const auto activeIt = activeVisualByInstance.find(entry.casterInstanceId);
            const bool playing = activeIt != activeVisualByInstance.end() ||
                                 instancesWithChannelEffects.contains(entry.casterInstanceId);
            if (playing) {
                if (activeIt != activeVisualByInstance.end() &&
                    activeIt->second == entry.visualId) {
                    fieldChannels[entry.casterInstanceId] = entry.visualId;
                }
                continue;
            }
        }
        plan.channelsToStart.push_back(idx);
    }

    return plan;
}

}  // namespace wowee::rendering
