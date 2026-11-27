#pragma once

#include "rendering/animation/animation_ids.hpp"
#include "rendering/spell_visual_system.hpp"

#include <algorithm>
#include <cstdint>

namespace wowee::core {

struct CreatureSpawnPoseChoice {
    uint32_t animationId = rendering::anim::STAND;
    bool loop = true;
    uint32_t nextAnimationId = 0;
    bool holdFinalPose = false;
};

/// Select the initial animation for a living creature without a persistent NPC emote.
///
/// Native TBC 2.4.3 plays authored Birth (127) or Spawn (145) emergence sequences
/// only when the unit is freshly spawned into the world (UPDATETYPE_CREATE_OBJECT2,
/// `unitNewlySpawned == true`) and has no active aura StateKit overriding its pose.
/// Units already present in the world that stream into visibility range via
/// UPDATETYPE_CREATE_OBJECT (`unitNewlySpawned == false`, e.g. returning into range
/// of Ragnaros 11502 or Murmur 18708) or swap displays in place (`displayChanged`)
/// start directly in their idle / active aura StateKit pose instead of replaying a
/// multi-second underground emergence animation.
inline constexpr CreatureSpawnPoseChoice selectCreatureSpawnPose(
    bool displayChanged,
    bool unitNewlySpawned,
    bool hasBirthAnim,
    bool hasSpawnAnim,
    uint32_t idleAnim,
    uint32_t auraStateAnim,
    bool auraStateNonLooping) noexcept {
    const bool hasAuraStateOverride =
        auraStateAnim != rendering::SpellVisualSystem::kNoAnimation;
    if (!displayChanged && unitNewlySpawned && !hasAuraStateOverride) {
        if (hasBirthAnim) {
            return {rendering::anim::BIRTH, /*loop=*/false, idleAnim, /*holdFinalPose=*/false};
        }
        if (hasSpawnAnim) {
            return {rendering::anim::SPAWN, /*loop=*/false, idleAnim, /*holdFinalPose=*/false};
        }
    }

    const bool holdAuraOneShot =
        hasAuraStateOverride && idleAnim == auraStateAnim && auraStateNonLooping;
    if (holdAuraOneShot) {
        return {idleAnim, /*loop=*/false, 0, /*holdFinalPose=*/true};
    }
    return {idleAnim, /*loop=*/true, 0, /*holdFinalPose=*/false};
}

/// Resolve swimming immersion depth (0.75 * height * scale) safely across all TBC
/// creature displays. 228 of 773 TBC CreatureModelData.dbc rows (Infernal, Squirrel,
/// SandWorm, GiantSlime, TwinEmperor, MountedDemonKnight, EyeOfKathune, etc.) store
/// CollisionHeight = 0.0; fall back to the loaded M2 collision box top (`modelBoundMaxZ`)
/// and then a 2.0-yard default rather than aborting.
inline constexpr float resolveUnitSwimmingDepth(
    float dbcCollisionHeight,
    float modelBoundMaxZ,
    float instanceScale) noexcept {
    constexpr float kFallbackCollisionHeight = 2.0f;
    const float baseHeight = (dbcCollisionHeight > 0.01f)
        ? dbcCollisionHeight
        : ((modelBoundMaxZ > 0.01f) ? modelBoundMaxZ : kFallbackCollisionHeight);
    return 0.75f * baseHeight * std::max(0.001f, instanceScale);
}

} // namespace wowee::core
