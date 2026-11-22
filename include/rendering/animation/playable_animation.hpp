#pragma once

#include "pipeline/m2_loader.hpp"
#include "rendering/animation/animation_ids.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace wowee::rendering {

// The animation a model shows for a requested AnimationData id it has no
// sequence for, and how.
struct PlayableStandIn {
    uint32_t animationId = 0;
    bool reversed = false;    // played from its last frame to its first
    bool frozenAtEnd = false; // shown still on its last frame
};

// A model's own playable animation lookup decides first (2.4.3 models carry
// one entry per AnimationData id 0..202). Native build 8606 follows it where
// it disagrees with AnimationData.dbc's Fallback column: the nether serpent
// (NetherDragon.m2, JAB Nether Serpent 21329) has ReadyUnarmed but no parry;
// its lookup maps ParryUnarmed (20) and ShieldBlock (24) to Stand, the DBC
// chain to StandWound -> ReadyUnarmed, and `.npc playemote 39` / `43` left it
// in Stand (_experiments/20261118T205000Z.anim_fallback, ns1). A lookup
// target the model lacks is looked up again.
//
// Ids past the lookup, and models without one (Wrath), follow the
// AnimationData Fallback chain; its Flags give the same play mode the lookup
// records: 0x20 frozen on the last frame (Dead -> Death), 0x10 backwards when
// the stand-in is the direct Fallback (Walkbackwards -> Walk, LootUp -> Loot).
// The lookup flags reproduce that rule on 2.56 M of the 2.58 M entries whose
// stand-in the chain reaches; most of the rest are SwimBackwards falling past
// Swim and Walk.
inline std::optional<PlayableStandIn> resolvePlayableStandIn(
        uint32_t requested,
        const std::vector<pipeline::M2Model::PlayableAnimation>& lookup,
        const std::function<bool(uint32_t)>& hasSequence,
        const std::function<std::optional<uint32_t>(uint32_t)>& dbcFallback,
        const std::function<uint32_t(uint32_t)>& dbcFlags) {
    constexpr int kMaxHops = 16;
    std::optional<PlayableStandIn> first;
    uint32_t wanted = requested;
    for (int hop = 0; hop < kMaxHops && wanted < lookup.size(); ++hop) {
        const auto& entry = lookup[wanted];
        if (!first) first = PlayableStandIn{entry.fallbackId, (entry.flags & 0x3u) == 0x1u, (entry.flags & 0x3u) == 0x3u};
        if (entry.fallbackId == wanted) break;
        wanted = entry.fallbackId;
        if (hasSequence(wanted)) {
            first->animationId = wanted;
            return first;
        }
    }
    if (first) return std::nullopt;  // the lookup names nothing this model has

    constexpr uint32_t kFlagReverse = 0x10, kFlagHoldLast = 0x20;
    const uint32_t flags = dbcFlags(requested);
    wanted = requested;
    for (int hop = 0; hop < kMaxHops; ++hop) {
        const auto next = dbcFallback(wanted);
        if (!next || *next == wanted) return std::nullopt;
        wanted = *next;
        if (hasSequence(wanted))
            return PlayableStandIn{wanted, (flags & kFlagHoldLast) == 0 && (flags & kFlagReverse) && hop == 0,
                                   (flags & kFlagHoldLast) != 0};
    }
    return std::nullopt;
}

/// True when a stand-in animation reached via `resolvePlayableStandIn` should
/// draw weighted M2 sequence variations (e.g. `AttackUnarmedOff 117 -> AttackOff 87`
/// on `Brutallus.m2`, which authors two `AttackOff` variations at frequency
/// 16383 each). Excludes `STAND` (so `Ready* -> Stand` on beasts holds plain
/// primary `Stand` without rearing fidgets), frozen poses (`Dead -> Death`),
/// and reversed clips (`WalkBackwards -> Walk`).
inline constexpr bool standInAllowsVariationDraw(const PlayableStandIn& standIn) {
    return !standIn.frozenAtEnd && !standIn.reversed && standIn.animationId != anim::STAND;
}

} // namespace wowee::rendering
