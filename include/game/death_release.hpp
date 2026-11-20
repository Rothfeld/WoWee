#pragma once

// The automatic spirit-release timer the death popup counts down.
//
// The server tells the client which death popup to show through byte 0 of
// PLAYER_FIELD_BYTES (CMaNGOS Player.h PlayerFieldByteFlags):
//   PLAYER_FIELD_BYTE_RELEASE_TIMER     0x08  "Display time till auto release spirit"
//   PLAYER_FIELD_BYTE_NO_RELEASE_WINDOW 0x10  "Display no release spirit window at all"
// Player::KillPlayer sets RELEASE_TIMER outside instances and starts its own
// 6-minute m_deathTimer at the same moment; Player::Update repops the corpse
// when it runs out. The client counts the same 6 minutes from the death.
//
// FrameXML (UIParent PLAYER_DEAD, StaticPopup DEATH) reads the answer of
// GetReleaseTimeRemaining: > 0 shows DEATH_RELEASE_TIMER ("%d %s until
// release", minutes rounded up), -1 shows DEATH_RELEASE_NOTIMER ("You have
// died. Release to the nearest graveyard?"), 0 shows no popup.

#include <cstdint>

namespace wowee::game {

inline constexpr uint8_t PLAYER_FIELD_BYTE_RELEASE_TIMER = 0x08;
inline constexpr uint8_t PLAYER_FIELD_BYTE_NO_RELEASE_WINDOW = 0x10;
inline constexpr int32_t DEATH_RELEASE_TIMER_MS = 6 * 60 * 1000;  // CMaNGOS KillPlayer m_deathTimer
inline constexpr int32_t DEATH_RELEASE_NO_TIMER = -1;

/// Milliseconds from death to automatic release, 0 for no popup, or
/// DEATH_RELEASE_NO_TIMER for a popup without a countdown.
inline constexpr int32_t releaseTimerAtDeathMs(uint32_t playerFieldBytes) {
    const auto flags = static_cast<uint8_t>(playerFieldBytes & 0xFFu);
    if (flags & PLAYER_FIELD_BYTE_NO_RELEASE_WINDOW) return 0;
    if (flags & PLAYER_FIELD_BYTE_RELEASE_TIMER) return DEATH_RELEASE_TIMER_MS;
    return DEATH_RELEASE_NO_TIMER;
}

/// GetReleaseTimeRemaining's answer in seconds from the state latched at death.
/// waitingToRelease: true only while dead and not yet a ghost; alive or ghost
/// players always get 0.0 (matching 2.4.3).
/// noTimer: the latch chose DEATH_RELEASE_NO_TIMER. deadlineMs: 0 if never latched.
inline constexpr double releaseTimeRemainingSec(bool waitingToRelease,
                                                bool noTimer,
                                                uint64_t deadlineMs,
                                                uint64_t nowMs) {
    if (!waitingToRelease) return 0.0;
    if (noTimer) return -1.0;
    if (deadlineMs == 0 || deadlineMs <= nowMs) return 0.0;
    return static_cast<double>(deadlineMs - nowMs) / 1000.0;
}

} // namespace wowee::game
