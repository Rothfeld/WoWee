#pragma once

#include <cstdint>

namespace wowee::game {

enum class GossipQuestState { None, Available, Incomplete, Completable };

// Gossip icons are protocol values, not expansion-independent quest states.
inline GossipQuestState gossipQuestState(uint32_t icon, bool tbc) {
    if (tbc) {
        switch (icon) {
            case 3: return GossipQuestState::Incomplete;
            case 4: case 7: case 8: return GossipQuestState::Completable;
            case 2: case 5: case 6: return GossipQuestState::Available;
            default: return GossipQuestState::None;
        }
    }
    switch (icon) {
        case 2: case 7: case 8: return GossipQuestState::Available;
        case 3: case 4: return GossipQuestState::Incomplete;
        case 5: case 6: case 10: return GossipQuestState::Completable;
        default: return GossipQuestState::None;
    }
}

} // namespace wowee::game
