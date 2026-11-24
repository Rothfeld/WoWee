#pragma once

#include <cstdint>
#include <glm/vec3.hpp>

namespace wowee::rendering {

/// Spell.dbc AttributesEx3 bit 20 (SPELL_ATTR_EX3_DEATH_PERSISTENT):
/// auras with this flag remain active across unit death (e.g. Ghost 8326,
/// Resurrection Sickness 15007, Flasks). Non-death-persistent auras (including
/// creature template visuals like Sathrovarr's Demonic Visual 44800 and
/// Shadow Image Visual 45263) must not apply or keep SpellVisualKit state
/// effects on a corpse.
inline constexpr uint32_t kSpellAttrEx3DeathPersistent = 0x00100000u;

inline constexpr bool auraVisualPersistsOnCorpse(uint32_t attrEx3) {
    return (attrEx3 & kSpellAttrEx3DeathPersistent) != 0u;
}

/// Whether removing a non-death-persistent aura state visual plays its M2
/// Decay (159) outro animation. In TBC 2.4.3, removing an aura from a living
/// unit (.unaura / dispel / expiry) plays Decay (159) (e.g. ArcanePower_State_Chest
/// 1000 ms fade), while unit death removes dropped non-death-persistent aura
/// visuals immediately (matching Precast/Cast/Channel removal on death).
inline constexpr bool shouldPlayAuraDecayOnRemoval(bool unitDead) {
    return !unitDead;
}

/// True when the unit is genuinely dead (corpse state) so non-death-persistent
/// aura visuals and CharProc tints must be suppressed.
/// A unit with health > 0 and UNIT_DYNFLAG_DEAD is feigning death, not a corpse.
inline constexpr bool isCorpseAuraSuppressed(bool trackedDead,
                                             uint32_t health,
                                             uint32_t maxHealth,
                                             uint32_t dynamicFlags,
                                             uint32_t deadFlag,
                                             uint32_t lootableFlag) {
    if (trackedDead) return true;
    if (maxHealth > 0u && health == 0u) return true;
    if ((dynamicFlags & lootableFlag) != 0u) return true;
    if ((dynamicFlags & deadFlag) != 0u && health == 0u) return true;
    return false;
}

/// Pure state mirror for CharacterInstance's SpellVisualKit CharProc 13 light
/// tint (used for unit-level death/expiry verification).
struct KitLightState {
    glm::vec3 color{1.0f};
    float remaining = 0.0f;
};

inline void resetKitLightOnDeath(KitLightState& state) {
    state.color = glm::vec3(1.0f);
    state.remaining = 0.0f;
}

inline void tickKitLight(KitLightState& state, float deltaTime) {
    if (state.remaining > 0.0f) {
        state.remaining -= deltaTime;
        if (state.remaining <= 0.0f) {
            state.remaining = 0.0f;
            state.color = glm::vec3(1.0f);
        }
    }
}

} // namespace wowee::rendering
