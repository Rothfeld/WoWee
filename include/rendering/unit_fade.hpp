#pragma once

// Whole-model fades of units drawn by CharacterRenderer.
//
// A unit fades in when it appears and a corpse fades out when the server
// removes it (SMSG_DESTROY_OBJECT or out of range; ledger #75c). Both use one
// duration. It is not taken from game data: no DBC, M2 or FrameXML value
// governs it, and the fade-in this client already had used 0.5 s. A native
// recording of a corpse despawning is what would replace it.

#include <algorithm>

namespace wowee::rendering {

inline constexpr float kUnitFadeSeconds = 0.5f;

/// A game object the server removes fades out over this long. Original 2.4.3
/// (pair 17, _experiments/20261118T133634Z.vashj_bridge_console,
/// fadewatch-native-g1.json): a used Shield Generator 185051 went from full to
/// gone in about 1.5 s (frames 0.2 s apart), starting seconds after the use,
/// when the server destroys it. WoWee removed it in one frame.
inline constexpr float kGameObjectFadeSeconds = 1.5f;

/// Opacity `elapsed` seconds into a fade-out that began at `startOpacity`.
inline float fadeOutOpacity(float startOpacity, float elapsed, float duration) {
    if (duration <= 0.0f) return 0.0f;
    return std::clamp(startOpacity * (1.0f - elapsed / duration), 0.0f, 1.0f);
}

} // namespace wowee::rendering
