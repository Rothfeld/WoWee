#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace wowee {
namespace game {

/**
 * Aura slot data for buff/debuff tracking
 */
struct AuraSlot {
    uint32_t spellId = 0;
    uint8_t flags = 0;         // Active, positive/negative, etc.
    uint8_t level = 0;
    uint8_t charges = 0;
    int32_t durationMs = -1;
    int32_t maxDurationMs = -1;
    uint64_t casterGuid = 0;
    uint64_t receivedAtMs = 0; // Client timestamp (ms) when durationMs was set
    /// The player cast it: the server sent SMSG_SET_EXTRA_AURA_INFO, which goes
    /// to the caster only. 2.4.3's UnitBuff answers duration and timeLeft for
    /// these alone (0, 0 for a permanent one such as Devotion Aura), nothing
    /// for a totem's Strength of Earth or a party member's Totem of Wrath.
    bool playerCast = false;

    [[nodiscard]] bool isEmpty() const { return spellId == 0; }
    // Remaining duration in ms, counting down from when the packet was received
    [[nodiscard]] int32_t getRemainingMs(uint64_t nowMs) const {
        if (durationMs < 0) return -1;
        uint64_t elapsed = (nowMs > receivedAtMs) ? (nowMs - receivedAtMs) : 0;
        int32_t remaining = durationMs - static_cast<int32_t>(elapsed);
        return (remaining > 0) ? remaining : 0;
    }
};

/**
 * Action bar slot
 */
struct ActionBarSlot {
    enum Type : uint8_t { EMPTY = 0, SPELL = 1, ITEM = 2, MACRO = 3 };
    Type type = EMPTY;
    uint32_t id = 0;              // spellId, itemId, or macroId
    float cooldownRemaining = 0.0f;
    float cooldownTotal = 0.0f;

    [[nodiscard]] bool isReady() const { return cooldownRemaining <= 0.0f; }
    [[nodiscard]] bool isEmpty() const { return type == EMPTY; }
};

/**
 * Floating combat text entry
 */
struct CombatTextEntry {
    enum Type : uint8_t {
        MELEE_DAMAGE, SPELL_DAMAGE, HEAL, MISS, DODGE, PARRY, BLOCK,
        EVADE, CRIT_DAMAGE, CRIT_HEAL, PERIODIC_DAMAGE, PERIODIC_HEAL, ENVIRONMENTAL,
        ENERGIZE, POWER_DRAIN, XP_GAIN, IMMUNE, ABSORB, RESIST, DEFLECT, REFLECT, PROC_TRIGGER,
        DISPEL, STEAL, INTERRUPT, INSTAKILL, HONOR_GAIN, GLANCING, CRUSHING
    };
    Type type;
    int32_t amount = 0;
    uint32_t spellId = 0;
    float age = 0.0f;           // Seconds since creation (for fadeout)
    bool isPlayerSource = false; // True if player dealt this
    uint8_t powerType = 0;      // For ENERGIZE/POWER_DRAIN: 0=mana,1=rage,2=focus,3=energy,6=runicpower
    uint64_t srcGuid = 0;       // Source entity (attacker/caster)
    uint64_t dstGuid = 0;       // Destination entity (victim/target) - used for world-space positioning
    /// The part of the hit just before it that was absorbed, resisted or
    /// blocked, when that hit still did damage. The combat log and
    /// COMBAT_TEXT_UPDATE report it; nothing is drawn for it over the unit.
    bool reduction = false;

    static constexpr float LIFETIME = 1.5f;
    [[nodiscard]] float lifetime() const {
        return type == XP_GAIN || type == HONOR_GAIN ? 4.5f : LIFETIME;
    }
    [[nodiscard]] bool isExpired() const { return age >= lifetime(); }
};

/// Rage (1) and Runic Power (6) are transmitted in tenths on the wire
/// (e.g. 100 raw wire units = 10 Rage in SMSG_SPELLENERGIZELOG and
/// SMSG_PERIODICAURALOG). Combat text and combat log events report whole points.
[[nodiscard]] inline int32_t displayPowerAmount(uint8_t powerType, int32_t rawAmount) {
    return (powerType == 1 || powerType == 6) ? (rawAmount / 10) : rawAmount;
}

/// Spell.dbc PowerType value for health-cost spells (int32 -2, e.g. Health Funnel, Bloodrage).
inline constexpr uint8_t kPowerHealth = 254;

/// Rage (1) and Runic Power (6) spell costs are stored in Spell.dbc (and sent
/// in SMSG_SET_FLAT_SPELL_MODIFIER) in tenths (e.g. 150 = 15 Rage, 100 = 10 Rage).
/// Tooltips and Lua APIs (GetSpellInfo, GetSpellPowerCost) report whole points.
[[nodiscard]] inline uint32_t displayPowerCost(uint8_t powerType, uint32_t rawCost) {
    return (powerType == 1 || powerType == 6) ? (rawCost / 10) : rawCost;
}

/**
 * Persistent combat log entry (stored in a rolling deque, survives beyond floating-text lifetime)
 */
struct CombatLogEntry {
    CombatTextEntry::Type type = CombatTextEntry::MELEE_DAMAGE;
    int32_t  amount       = 0;
    uint32_t spellId      = 0;
    bool     isPlayerSource = false;
    uint8_t  powerType    = 0;   // For ENERGIZE/DRAIN: power type; for ENVIRONMENTAL: env damage type
    time_t   timestamp    = 0;   // Wall-clock time (std::time(nullptr))
    std::string sourceName;      // Resolved display name of attacker/caster
    std::string targetName;      // Resolved display name of victim/target
};

/**
 * Spell cooldown entry received from server
 */
struct SpellCooldownEntry {
    uint32_t spellId;
    uint16_t itemId;
    uint16_t categoryId;
    uint32_t cooldownMs;
    uint32_t categoryCooldownMs;
};

// ── SpellEffect - SMSG_SPELLLOGEXECUTE effectType field ───────────────────
// The servers' SpellEffects enum, the same numbers in every expansion. Only the
// effects this client reads from the log.
namespace SpellEffect {
    constexpr uint32_t POWER_DRAIN               = 8;
    constexpr uint32_t HEALTH_LEECH              = 9;
    constexpr uint32_t CREATE_ITEM               = 24;
    constexpr uint32_t POWER_BURN                = 62;
    constexpr uint32_t INTERRUPT_CAST            = 68;
    constexpr uint32_t FEED_PET                  = 101;
    constexpr uint32_t CREATE_ITEM2              = 157;
} // namespace SpellEffect

// ── SpellMissInfo - SMSG_SPELLLOGMISS / SMSG_SPELL_GO miss type ─
// The servers' own numbering, the same from 1.12 to 3.3.5 (AzerothCore and
// cmangos SharedDefines.h).
namespace SpellMissInfo {
    constexpr uint8_t NONE      = 0;
    constexpr uint8_t MISS      = 1;
    constexpr uint8_t RESIST    = 2;
    constexpr uint8_t DODGE     = 3;
    constexpr uint8_t PARRY     = 4;
    constexpr uint8_t BLOCK     = 5;
    constexpr uint8_t EVADE     = 6;
    constexpr uint8_t IMMUNE    = 7;
    constexpr uint8_t IMMUNE2   = 8;
    constexpr uint8_t DEFLECT   = 9;
    constexpr uint8_t ABSORB    = 10;
    constexpr uint8_t REFLECT   = 11;
} // namespace SpellMissInfo

} // namespace game
} // namespace wowee
