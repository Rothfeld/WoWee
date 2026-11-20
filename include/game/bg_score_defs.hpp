#pragma once

#include <cstdint>
#include <cstddef>

namespace wowee {
namespace game {

/// What each battleground's per-player objective columns are called.
///
/// BuildObjectivesBlock sends a count and that many bare numbers - no labels.
/// Which column is which is client knowledge, and this is that knowledge for
/// the scoreboard.
///
/// Read off each battleground's own BuildObjectivesBlock, in the order it
/// writes them. The count is part of the contract: Alterac Valley sends five
/// and Eye of the Storm one, so a reader that assumes two is wrong twice.
struct BgObjectiveColumns {
    uint32_t    mapId;
    const char* labels[5];   // nullptr-terminated within the row
};

inline constexpr BgObjectiveColumns kBgObjectiveColumns[] = {
    { .mapId = 489, .labels = {"Flags Captured", "Flags Returned", nullptr, nullptr, nullptr} },
    { .mapId = 529, .labels = {"Bases Assaulted", "Bases Defended", nullptr, nullptr, nullptr} },
    {  .mapId = 30, .labels = {"Graveyards Assaulted", "Graveyards Defended",
            "Towers Assaulted", "Towers Defended", "Secondary Objectives"} },
    { .mapId = 566, .labels = {"Flags Captured", nullptr, nullptr, nullptr, nullptr} },
    { .mapId = 607, .labels = {"Demolishers Destroyed", "Gates Destroyed", nullptr, nullptr, nullptr} },
    { .mapId = 628, .labels = {"Bases Assaulted", "Bases Defended", nullptr, nullptr, nullptr} },
};

/// The label for a column, or nullptr when the map is unknown or the index is
/// past what that battleground sends.
inline const char* bgObjectiveLabel(uint32_t mapId, size_t index) {
    if (index >= 5) return nullptr;
    for (const auto& c : kBgObjectiveColumns) {
        if (c.mapId == mapId) return c.labels[index];
    }
    return nullptr;
}

}  // namespace game
}  // namespace wowee
