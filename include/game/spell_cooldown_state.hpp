#pragma once

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <utility>

namespace wowee {
namespace game {

/// Tracks active per-spell and per-category cooldowns for one unit (player or pet).
///
/// In WoW TBC 2.4.3, multi-rank spells (e.g. Shadowburn R1..R8, cat=651) and
/// shared-cooldown spell families (e.g. Earth/Flame/Frost Shock, cat=19;
/// Healing Potions, cat=4) share a cooldown via Spell.dbc Category +
/// CategoryRecoveryTime, while individual spells may also have their own
/// RecoveryTime (e.g. Earth/Fire Elemental Totem: 20m spell, 2m category).
struct SpellCooldownState {
    static constexpr float kExistingGuardSec = 0.5f;
    static constexpr float kMergeEpsilonSec = 0.05f;

    std::unordered_map<uint32_t, float> spellRemaining;
    std::unordered_map<uint32_t, float> spellTotal;
    std::unordered_map<uint32_t, float> categoryRemaining;
    std::unordered_map<uint32_t, float> categoryTotal;

    static float mergeSeconds(float current, float incoming) {
        if (incoming <= 0.0f) return 0.0f;
        if (current <= 0.0f) return incoming;
        if (incoming > current + kMergeEpsilonSec) return current;
        return incoming;
    }

    /// Seed a spell-specific cooldown from DBC (does not overwrite an active cooldown > 0.5s).
    bool seedSpell(uint32_t spellId, float seconds) {
        if (spellId == 0 || seconds <= 0.0f) return false;
        auto existing = spellRemaining.find(spellId);
        if (existing != spellRemaining.end() && existing->second > kExistingGuardSec) return false;
        spellRemaining[spellId] = seconds;
        spellTotal[spellId] = seconds;
        return true;
    }

    /// Seed a category cooldown from DBC (does not overwrite an active cooldown > 0.5s).
    bool seedCategory(uint32_t categoryId, float seconds) {
        if (categoryId == 0 || seconds <= 0.0f) return false;
        auto existing = categoryRemaining.find(categoryId);
        if (existing != categoryRemaining.end() && existing->second > kExistingGuardSec) return false;
        categoryRemaining[categoryId] = seconds;
        categoryTotal[categoryId] = seconds;
        return true;
    }

    /// Apply a server-reported spell cooldown (merges with any active value).
    void mergeSpell(uint32_t spellId, float seconds) {
        if (spellId == 0 || seconds <= 0.0f) return;
        auto it = spellRemaining.find(spellId);
        if (it == spellRemaining.end()) {
            spellRemaining[spellId] = seconds;
            spellTotal[spellId] = seconds;
        } else {
            it->second = mergeSeconds(it->second, seconds);
            float& total = spellTotal[spellId];
            total = std::max(total, it->second);
        }
    }

    /// Apply a server-reported category cooldown (merges with any active value).
    void mergeCategory(uint32_t categoryId, float seconds) {
        if (categoryId == 0 || seconds <= 0.0f) return;
        auto it = categoryRemaining.find(categoryId);
        if (it == categoryRemaining.end()) {
            categoryRemaining[categoryId] = seconds;
            categoryTotal[categoryId] = seconds;
        } else {
            it->second = mergeSeconds(it->second, seconds);
            float& total = categoryTotal[categoryId];
            total = std::max(total, it->second);
        }
    }

    /// Returns {remaining, total} for a spell considering both its own cooldown
    /// and its category cooldown (whichever has more remaining time wins).
    [[nodiscard]] std::pair<float, float> query(uint32_t spellId, uint32_t categoryId) const {
        float sRem = 0.0f;
        float sTot = 0.0f;
        if (spellId != 0) {
            if (auto it = spellRemaining.find(spellId); it != spellRemaining.end()) {
                sRem = it->second;
                auto tit = spellTotal.find(spellId);
                sTot = (tit != spellTotal.end()) ? tit->second : sRem;
            }
        }
        float cRem = 0.0f;
        float cTot = 0.0f;
        if (categoryId != 0) {
            if (auto it = categoryRemaining.find(categoryId); it != categoryRemaining.end()) {
                cRem = it->second;
                auto tit = categoryTotal.find(categoryId);
                cTot = (tit != categoryTotal.end()) ? tit->second : cRem;
            }
        }
        if (cRem > sRem) return {cRem, cTot};
        return {sRem, sTot};
    }

    /// Clear a spell cooldown and, if categoryId != 0, its category cooldown.
    void clear(uint32_t spellId, uint32_t categoryId) {
        if (spellId != 0) {
            spellRemaining.erase(spellId);
            spellTotal.erase(spellId);
        }
        if (categoryId != 0) {
            categoryRemaining.erase(categoryId);
            categoryTotal.erase(categoryId);
        }
    }

    /// Adjust an active spell and/or category cooldown by deltaSeconds.
    bool modify(uint32_t spellId, uint32_t categoryId, float deltaSeconds) {
        bool changed = false;
        if (spellId != 0) {
            if (auto it = spellRemaining.find(spellId); it != spellRemaining.end()) {
                float next = it->second + deltaSeconds;
                if (next <= 0.0f) {
                    spellTotal.erase(spellId);
                    spellRemaining.erase(it);
                } else {
                    it->second = next;
                    float& total = spellTotal[spellId];
                    total = std::max(total, next);
                }
                changed = true;
            }
        }
        if (categoryId != 0) {
            if (auto it = categoryRemaining.find(categoryId); it != categoryRemaining.end()) {
                float next = it->second + deltaSeconds;
                if (next <= 0.0f) {
                    categoryTotal.erase(categoryId);
                    categoryRemaining.erase(it);
                } else {
                    it->second = next;
                    float& total = categoryTotal[categoryId];
                    total = std::max(total, next);
                }
                changed = true;
            }
        }
        return changed;
    }

    /// Advance all active cooldowns by dt seconds. Returns true if any expired.
    bool tick(float dt) {
        bool anyExpired = false;
        for (auto it = spellRemaining.begin(); it != spellRemaining.end(); ) {
            it->second -= dt;
            if (it->second <= 0.0f) {
                spellTotal.erase(it->first);
                it = spellRemaining.erase(it);
                anyExpired = true;
            } else {
                ++it;
            }
        }
        for (auto it = categoryRemaining.begin(); it != categoryRemaining.end(); ) {
            it->second -= dt;
            if (it->second <= 0.0f) {
                categoryTotal.erase(it->first);
                it = categoryRemaining.erase(it);
                anyExpired = true;
            } else {
                ++it;
            }
        }
        return anyExpired;
    }

    void clearAll() {
        spellRemaining.clear();
        spellTotal.clear();
        categoryRemaining.clear();
        categoryTotal.clear();
    }
};

} // namespace game
} // namespace wowee
