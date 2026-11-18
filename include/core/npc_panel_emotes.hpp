#pragma once

#include <cassert>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wowee::core {

// Emotes a gossip text or quest greeting plays on its NPC when the panel
// opens, as native 2.4.3 does (measured on GM Island: Marshal Dughan npc_text
// 16211 (5 ms, EXCLAMATION), Challe (0, TALK), Gryan Stoutmantle greeting
// (0, TALK), Mizli Crankwheel (0, TALK), (3, EXCLAMATION); none for all-zero
// emotes or a vendor-only NPC). The first emote starts after its delay; each
// later one waits until the previous animation has finished, then its delay.
// Emote 0 (ONESHOT_NONE) entries are skipped.
// Unverified: native started Mizli's second emote about 0.6 s after the
// talk animation ended, not 3 ms.
class NpcPanelEmotes {
public:
    struct Step {
        uint32_t delayMs;
        uint32_t emoteId;
    };

    // A new panel replaces whatever sequence the NPC was still playing.
    void start(uint64_t guid, const std::vector<std::pair<uint32_t, uint32_t>>& delayEmotes) {
        Sequence sequence;
        for (const auto& [delay, emote] : delayEmotes)
            if (emote != 0) sequence.steps.push_back({delay, emote});
        if (sequence.steps.empty()) {
            sequences_.erase(guid);
            return;
        }
        sequence.waitMs = static_cast<float>(sequence.steps.front().delayMs);
        sequences_[guid] = std::move(sequence);
    }

    // stillPlaying(guid, emoteId): whether the emote started last for this
    // NPC is still its current animation. play(guid, emoteId) starts one.
    template <typename StillPlaying, typename Play>
    void advance(float dt, StillPlaying&& stillPlaying, Play&& play) {
        assert(std::isfinite(dt) && dt >= 0.0f);
        for (auto it = sequences_.begin(); it != sequences_.end();) {
            Sequence& s = it->second;
            if (s.next > 0 && s.waitingForEnd) {
                if (stillPlaying(it->first, s.steps[s.next - 1].emoteId)) { ++it; continue; }
                s.waitingForEnd = false;
                s.waitMs = static_cast<float>(s.steps[s.next].delayMs);
            }
            s.waitMs -= dt * 1000.0f;
            if (s.waitMs > 0.0f) { ++it; continue; }
            play(it->first, s.steps[s.next].emoteId);
            ++s.next;
            s.waitingForEnd = true;
            if (s.next == s.steps.size()) it = sequences_.erase(it);
            else ++it;
        }
    }

    void erase(uint64_t guid) { sequences_.erase(guid); }
    [[nodiscard]] bool pending(uint64_t guid) const { return sequences_.contains(guid); }

private:
    struct Sequence {
        std::vector<Step> steps;
        size_t next = 0;
        float waitMs = 0.0f;
        bool waitingForEnd = false;
    };
    std::unordered_map<uint64_t, Sequence> sequences_;
};

} // namespace wowee::core
