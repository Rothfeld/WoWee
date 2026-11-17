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

/// Opacity `elapsed` seconds into a fade-out that began at `startOpacity`.
inline float fadeOutOpacity(float startOpacity, float elapsed, float duration) {
    if (duration <= 0.0f) return 0.0f;
    return std::clamp(startOpacity * (1.0f - elapsed / duration), 0.0f, 1.0f);
}

} // namespace wowee::rendering
