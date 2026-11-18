#pragma once

#include "game/world_packets.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace wowee::game {

inline uint32_t gameObjectLockId(const GameObjectQueryResponseData* info) {
    if (!info || !info->hasData) return 0;
    switch (info->type) {
        case 0: // Door
        case 1: // Button
            return info->data[1];
        case 2: case 3: case 6: case 10: case 12: case 13: case 24: case 26:
            return info->data[0];
        default:
            return 0;
    }
}

/// Lock.dbc slot kinds (Type column).
inline constexpr uint32_t kLockKeyItem = 1;   ///< Index is an item id
inline constexpr uint32_t kLockKeySkill = 2;  ///< Index is a LockType id

/// One Lock.dbc row: Type, Index, Skill and Action of its eight slots.
struct LockRecord {
    std::array<uint32_t, 8> type{}, index{}, skill{}, action{};
};

/// Read row `row` of Lock.dbc (columns 1-8 Type, 9-16 Index, 17-24 Skill,
/// 25-32 Action).
template <typename Dbc>
LockRecord readLockRecord(const Dbc& locks, uint32_t row) {
    LockRecord lock;
    for (uint32_t slot = 0; slot < 8; ++slot) {
        lock.type[slot] = locks.getUInt32(row, 1 + slot);
        lock.index[slot] = locks.getUInt32(row, 9 + slot);
        lock.skill[slot] = locks.getUInt32(row, 17 + slot);
        lock.action[slot] = locks.getUInt32(row, 25 + slot);
    }
    return lock;
}

/// The key items that open a lock, in slot order. A GO_FLAG_LOCKED object
/// opens only by its Action 1 slots; an unflagged one by any. Original 2.4.3:
/// Dire Maul's Door 177198 (flagged, Lock 1562) names the Crescent Key from
/// its Action 1 slot; the unflagged Shield Generator 185051 (Lock 1718, key
/// Tainted Core 31088, Action 0) shows "Requires Tainted Core" and refuses a
/// player without one (pair 17).
inline std::vector<uint32_t> lockKeyItems(const LockRecord& lock, bool flaggedLocked) {
    std::vector<uint32_t> keys;
    for (uint32_t slot = 0; slot < 8; ++slot) {
        if (lock.type[slot] != kLockKeyItem || lock.index[slot] == 0) continue;
        if (flaggedLocked && lock.action[slot] != 1) continue;
        keys.push_back(lock.index[slot]);
    }
    return keys;
}

/// Spell effects that open a lock.
inline constexpr uint32_t kSpellEffectOpenLock = 33;
inline constexpr uint32_t kSpellEffectOpenLockItem = 59;

/// The item spell slot (0-4) a key opens its lock with: its first on-use
/// spell with an OPEN_LOCK or OPEN_LOCK_ITEM effect. Keys carry Opening 3366
/// (OPEN_LOCK_ITEM): Crescent Key 18249 in slot 0, Tainted Core 31088 in
/// slot 1 behind Throw Key 38134. CMSG_USE_ITEM names the slot and CMaNGOS
/// casts that spell alone (Player::CastItemUseSpell); the server opens a key
/// lock only for a spell cast from the key itself (Spell::CanOpenLock).
/// `effectsOf(spellId)` gives the spell's three effects.
template <typename EffectsOf>
std::optional<uint8_t> keyOpeningSpellIndex(
        const std::array<ItemQueryResponseData::ItemSpell, 5>& spells, EffectsOf&& effectsOf) {
    constexpr uint32_t kTriggerOnUse = 0;
    for (uint8_t i = 0; i < spells.size(); ++i) {
        if (spells[i].spellId == 0 || spells[i].spellTrigger != kTriggerOnUse) continue;
        for (const uint32_t effect : effectsOf(spells[i].spellId))
            if (effect == kSpellEffectOpenLock || effect == kSpellEffectOpenLockItem) return i;
    }
    return std::nullopt;
}

} // namespace wowee::game
