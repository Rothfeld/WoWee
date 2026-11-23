#pragma once

// Whole-model fades of units drawn by CharacterRenderer.
//
// A unit fades in when it appears and a corpse fades out when the server
// removes it (SMSG_DESTROY_OBJECT or out of range; ledger #75c). Both use one
// duration. It is not taken from game data: no DBC, M2 or FrameXML value
// governs it, and the fade-in this client already had used 0.5 s. A native
// recording of a corpse despawning is what would replace it.

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace wowee::rendering {

inline constexpr float kUnitFadeSeconds = 0.5f;
inline constexpr float kKitOpacityTransitionSeconds = 1.0f;

/// Newly spawned/summoned units (SMSG_UPDATE_OBJECT UPDATETYPE_CREATE_OBJECT2)
/// fade in over 2.0 s with a quadratic ease-in curve on TBC 2.4.3 build 8606
/// (measured at 30 Hz on pair 13 for Summon Imp 688, Summon Voidwalker 697,
/// and Summon Felguard 30146).
inline constexpr float kUnitSpawnFadeInSeconds = 2.0f;

/// Living units removed by the server (e.g. dismissed/replaced summoned pets)
/// fade out over 1.5 s on TBC 2.4.3 build 8606 (pair 13).
inline constexpr float kUnitDespawnFadeOutSeconds = 1.5f;

/// A game object the server removes fades out over this long. Original 2.4.3
/// (pair 17, _experiments/20261118T133634Z.vashj_bridge_console,
/// fadewatch-native-g1.json): a used Shield Generator 185051 went from full to
/// gone in about 1.5 s (frames 0.2 s apart), starting seconds after the use,
/// when the server destroys it. WoWee removed it in one frame.
inline constexpr float kGameObjectFadeSeconds = 1.5f;

/// Spawn fade-in duration for a creature instance. In-place display changes
/// (shapeshift/transform IN or OUT, death revert to native display, `.npc setmodel`)
/// swap models immediately at full display alpha (0 s) on TBC 2.4.3 build 8606;
/// newly summoned/created units fade in over 2.0 s, and existing units entering
/// view fade in over 0.5 s.
inline constexpr float creatureSpawnFadeDuration(bool displayChanged, bool newlySpawned) {
    if (displayChanged) return 0.0f;
    return newlySpawned ? kUnitSpawnFadeInSeconds : kUnitFadeSeconds;
}

/// Opacity `elapsed` seconds into a spawn fade-in toward `targetOpacity`.
inline float fadeInOpacity(float targetOpacity, float elapsed, float duration) {
    const float clampedTarget = std::clamp(targetOpacity, 0.0f, 1.0f);
    if (duration <= 0.0f) return clampedTarget;
    const float u = std::clamp(elapsed / duration, 0.0f, 1.0f);
    return std::clamp(clampedTarget * u * u, 0.0f, 1.0f);
}

/// Opacity `elapsed` seconds into a fade-out that began at `startOpacity`.
inline float fadeOutOpacity(float startOpacity, float elapsed, float duration) {
    if (duration <= 0.0f) return 0.0f;
    return std::clamp(startOpacity * (1.0f - elapsed / duration), 0.0f, 1.0f);
}

/// Duration for a SpellVisualKit CharProc 14 opacity update.
/// Initial aura sync on a newly created unit instance (or visual transfer across
/// an in-place display swap/morph) applies any already-active CharProc 14 opacity
/// immediately (0 s) so a unit entering view stealthed or translucent fades
/// directly from 0 -> target instead of appearing opaque and then turning
/// translucent. Runtime aura applications and removals ease over
/// kKitOpacityTransitionSeconds (1.0 s).
inline constexpr float kitOpacityTransitionDuration(bool initialAppearanceSync) {
    return initialAppearanceSync ? 0.0f : kKitOpacityTransitionSeconds;
}

/// Opacity `elapsed` seconds into a SpellVisualKit CharProc 14 transition
/// (Stealth 0.3, Ghost 0.5, Transparency 75% 0.25, Banish, etc.). Native
/// 2.4.3 eases both ways over 1 s as the cube of elapsed/duration.
inline float kitTransitionOpacity(float fromOpacity, float targetOpacity, float elapsed,
                                  float duration = kKitOpacityTransitionSeconds) {
    const float clampedFrom = std::clamp(fromOpacity, 0.0f, 1.0f);
    const float clampedTarget = std::clamp(targetOpacity, 0.0f, 1.0f);
    if (duration <= 0.0f || elapsed >= duration) return clampedTarget;
    const float progress = std::clamp(elapsed / duration, 0.0f, 1.0f);
    return std::clamp(clampedFrom + (clampedTarget - clampedFrom) * progress * progress * progress,
                      0.0f, 1.0f);
}

/// Combined unit opacity across display/spawn alpha, close-camera fade, and
/// SpellVisualKit CharProc 14 (stealth/ghost/transparency).
inline float effectiveUnitOpacity(float opacity, float cameraOpacity, float kitOpacity) {
    return std::clamp(opacity * cameraOpacity * kitOpacity, 0.0f, 1.0f);
}

/// Schedule CharacterRenderer draw passes for one unit group (the root unit
/// plus its attached helms, shoulders, weapons, and enchant effects).
///
/// Solid pass (0) draws every member once. Blended pass runs the whole-unit
/// nearest-surface depth prepass (-1) across the root and all solid/alpha-key
/// attachments BEFORE the color blend pass (1), so overlapping body, helm,
/// shoulders, and weapons self-occlude into one coherent translucent silhouette
/// instead of double-blending at overlap seams.
inline void appendUnitRenderSteps(std::vector<std::pair<int, uint32_t>>& steps,
                                  const std::vector<uint32_t>& unitMembers,
                                  bool includeSolid, bool includeBlended) {
    if (unitMembers.empty()) return;
    if (includeSolid) {
        for (const uint32_t id : unitMembers) steps.emplace_back(0, id);
    }
    if (includeBlended) {
        for (const uint32_t id : unitMembers) steps.emplace_back(-1, id);
        for (const uint32_t id : unitMembers) steps.emplace_back(1, id);
    }
}

} // namespace wowee::rendering
