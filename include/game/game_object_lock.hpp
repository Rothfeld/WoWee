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

/// GAMEOBJECT_STATE value of an object that stands open (a raised gate, a
/// door swung back); 1 is shut.
inline constexpr uint32_t kGoStateActive = 0;
/// GAMEOBJECT_FLAGS bit of a locked object.
inline constexpr uint32_t kGoFlagLocked = 0x2;

/// The Lock.dbc Action whose slots apply to an object now: 2 (close) while
/// it stands open, otherwise 1 for a GO_FLAG_LOCKED object and 0 for the
/// rest. Original 2.4.3 (pair 22, Lock 879 = key Action 1, Lockpicking 300
/// Action 1, Quick Open 0, Quick Close 2, Blasting 1):
///   - Gauntlet Gate 175357, shut, GO_FLAG_LOCKED: "Locked" red,
///     "Requires Key to the City", gear greyed until the key is carried;
///   - Gauntlet Gate 175356, shut, unflagged: its name only, gear (Quick
///     Open, which every character knows);
///   - King's Square Gate 175352 spawned open, GO_FLAG_LOCKED: "Locked" in
///     grey and the gear (Quick Close), no key line;
///   - Coilskar Chest 184716 spawned open (Lock 1705: key Action 1, Open 0):
///     "Locked" red and nothing else; spawned Peacebloom: no Herbalism line.
inline uint32_t lockActionNow(uint32_t goState, bool flaggedLocked) {
    if (goState == kGoStateActive) return 2;
    return flaggedLocked ? 1 : 0;
}

/// The key items of the slots that apply now (lockActionNow), in slot
/// order. Shield Generator 185051 (unflagged, shut, Lock 1718: Tainted Core
/// 31088 Action 0) shows "Requires Tainted Core" with or without the core;
/// Dire Maul's Door 177198 (flagged, shut, Lock 1562) names the Crescent
/// Key of its Action 1 slot (pair 17, pair 0).
inline std::vector<uint32_t> lockKeyItems(const LockRecord& lock, uint32_t action) {
    std::vector<uint32_t> keys;
    for (uint32_t slot = 0; slot < 8; ++slot) {
        if (lock.type[slot] != kLockKeyItem || lock.index[slot] == 0) continue;
        if (lock.action[slot] != action) continue;
        keys.push_back(lock.index[slot]);
    }
    return keys;
}

/// The LockType slots (kind 2) that apply now, as slot numbers in order.
inline std::vector<uint32_t> lockSkillSlots(const LockRecord& lock, uint32_t action) {
    std::vector<uint32_t> slots;
    for (uint32_t slot = 0; slot < 8; ++slot) {
        if (lock.type[slot] != kLockKeySkill || lock.index[slot] == 0) continue;
        if (lock.action[slot] != action) continue;
        slots.push_back(slot);
    }
    return slots;
}

/// How hard a lock requirement is for the player, by their rank against the
/// lock's: impossible below it, then orange, yellow, green and trivial (grey)
/// as the rank passes it by 25, 50 and 100. Ordered easiest last.
enum class LockDifficulty { Impossible, Orange, Yellow, Green, Trivial };

inline LockDifficulty lockDifficulty(uint32_t playerRank, uint32_t requiredRank) {
    if (playerRank < requiredRank) return LockDifficulty::Impossible;
    const uint32_t over = playerRank - requiredRank;
    if (over >= 100) return LockDifficulty::Trivial;
    if (over >= 50) return LockDifficulty::Green;
    if (over >= 25) return LockDifficulty::Yellow;
    return LockDifficulty::Orange;
}

/// The colour of a requirement line; nullopt for Impossible, where the
/// caller's red applies (RED_FONT_COLOR on "Locked", pure red on "Requires
/// Herbalism"). Original 2.4.3 (pair 22): Lucius's Lockbox (Lockpicking 25,
/// Pick Lock known) "Locked" red at Lockpicking 20, orange at 30, green at
/// 120, grey at 200; Peacebloom (pair 0) "Requires Herbalism" orange at
/// Herbalism 1.
struct LockLineColour { float r, g, b; };
inline std::optional<LockLineColour> lockDifficultyColour(LockDifficulty difficulty) {
    switch (difficulty) {
        case LockDifficulty::Impossible: return std::nullopt;
        case LockDifficulty::Orange: return LockLineColour{1.0f, 0.5f, 0.25f};
        case LockDifficulty::Yellow: return LockLineColour{1.0f, 1.0f, 0.0f};
        case LockDifficulty::Green: return LockLineColour{0.25f, 0.75f, 0.25f};
        case LockDifficulty::Trivial: return LockLineColour{0.5f, 0.5f, 0.5f};
    }
    return std::nullopt;
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
