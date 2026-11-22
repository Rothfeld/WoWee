#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_set>

namespace wowee {
namespace game {

/// Apply UNIT_MOD_CAST_SPEED (float multiplier; < 1.0 faster, > 1.0 slower) to a
/// spell's base cast time in milliseconds. Instant spells (0 ms) and invalid or
/// non-positive multipliers leave the cast time unchanged.
inline uint32_t applyUnitModCastSpeed(uint32_t baseCastTimeMs, float modCastSpeed) {
    if (baseCastTimeMs == 0 || !std::isfinite(modCastSpeed) || modCastSpeed <= 0.0f) {
        return baseCastTimeMs;
    }
    const long scaled = std::lround(static_cast<double>(baseCastTimeMs) * static_cast<double>(modCastSpeed));
    return scaled > 0 ? static_cast<uint32_t>(scaled) : 0u;
}

/// First school index (0..6: Physical, Holy, Fire, Nature, Frost, Shadow, Arcane)
/// in a WoW SpellSchoolMask, matching CMaNGOS GetFirstSchoolInMask().
inline uint32_t firstSchoolIndexFromMask(uint32_t schoolMask) {
    for (uint32_t school = 0; school < 7u; ++school) {
        if ((schoolMask & (1u << school)) != 0u) {
            return school;
        }
    }
    return 0u;
}

/// Apply TBC/CMaNGOS unit power cost modifiers in server order:
/// 1) flat school modifier (UNIT_FIELD_POWER_COST_MODIFIER + school)
/// 2) SpellModOp::Cost (flat + pct)
/// 3) school cost multiplier (1.0f + UNIT_FIELD_POWER_COST_MULTIPLIER + school)
/// Clamps negative totals to 0. Free spells (baseCost == 0) remain 0.
inline uint32_t applyUnitPowerCostModifiers(uint32_t baseCost,
                                            int32_t flatSchoolMod,
                                            int32_t spellFlatMod,
                                            int32_t spellPctMod,
                                            float schoolMultiplier) {
    if (baseCost == 0) return 0u;
    int64_t cost = static_cast<int64_t>(baseCost) + flatSchoolMod + spellFlatMod;
    if (spellPctMod != 0) {
        const int64_t delta = cost * spellPctMod;
        cost = cost + (delta >= 0 ? (delta + 50) : (delta - 50)) / 100;
    }
    if (cost <= 0) return 0u;
    if (std::isfinite(schoolMultiplier) && schoolMultiplier != 0.0f) {
        const double factor = 1.0 + static_cast<double>(schoolMultiplier);
        if (factor <= 0.0) return 0u;
        const long scaled = std::lround(static_cast<double>(cost) * factor);
        return scaled > 0 ? static_cast<uint32_t>(scaled) : 0u;
    }
    return static_cast<uint32_t>(cost);
}

/// Decoded layout of TBC PLAYER_BYTES_3 (CMaNGOS Player.cpp):
///   bit 0:      gender (0 male, 1 female)
///   bits 1..15: m_drunk & 0xFFFE (0 sober, >0 tipsy, >=12800 drunk, >=23000 smashed)
///   bits 24..31: arena faction byte
struct DecodedPlayerBytes3 {
    uint8_t gender = 0;
    uint16_t drunkValue = 0;
    uint8_t drunkState = 0;      // 0..3 matching SMSG_CROSSED_INEBRIATION_THRESHOLD
    float intoxication = 0.0f;   // 0.0f..1.0f camera/post-process strength
    uint8_t arenaFaction = 0;
};

inline DecodedPlayerBytes3 decodePlayerBytes3(uint32_t raw) {
    DecodedPlayerBytes3 out;
    out.gender = static_cast<uint8_t>(raw & 0x01u);
    out.drunkValue = static_cast<uint16_t>(raw & 0xFFFEu);
    if (out.drunkValue >= 23000u) {
        out.drunkState = 3u;
    } else if (out.drunkValue >= 12800u) {
        out.drunkState = 2u;
    } else if (out.drunkValue > 0u) {
        out.drunkState = 1u;
    } else {
        out.drunkState = 0u;
    }
    out.intoxication = static_cast<float>(out.drunkState) / 3.0f;
    out.arenaFaction = static_cast<uint8_t>((raw >> 24) & 0xFFu);
    return out;
}

/// Synchronize one 32-bit word (wordIndex 0 => bits 0..31, wordIndex 1 => bits 32..63)
/// of PLAYER__FIELD_KNOWN_TITLES into the known-title-bits set. Returns true if the set changed.
inline bool syncKnownTitleBitsWord(std::unordered_set<uint32_t>& knownBits,
                                   uint32_t wordIndex,
                                   uint32_t mask) {
    const uint32_t baseBit = wordIndex * 32u;
    bool changed = false;
    for (uint32_t bit = 0; bit < 32u; ++bit) {
        const uint32_t titleBit = baseBit + bit;
        if ((mask & (1u << bit)) != 0u) {
            if (knownBits.insert(titleBit).second) changed = true;
        } else {
            if (knownBits.erase(titleBit) > 0u) changed = true;
        }
    }
    return changed;
}

/// Normalize server GAMEOBJECT_ANIMPROGRESS (0..255) to [0.0f, 1.0f].
inline float normalizeGameObjectAnimProgress(uint32_t rawProgress) {
    return static_cast<float>(std::min(rawProgress, 255u)) / 255.0f;
}

} // namespace game
} // namespace wowee
