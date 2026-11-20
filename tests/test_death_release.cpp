// The death popup's release timer, from the server's PLAYER_FIELD_BYTES flags
// (CMaNGOS Player.h PlayerFieldByteFlags, Player::KillPlayer) to the answer
// FrameXML's UIParent and StaticPopup DEATH read from GetReleaseTimeRemaining.
#include <catch_amalgamated.hpp>

#include "game/death_release.hpp"

using namespace wowee::game;

TEST_CASE("release timer chosen from player field bytes", "[death]") {
    // KillPlayer outside an instance: RELEASE_TIMER, six minutes.
    CHECK(releaseTimerAtDeathMs(0x08) == 360000);
    // Only byte 0 holds these flags.
    CHECK(releaseTimerAtDeathMs(0xFFFFFF08u) == 360000);
    CHECK(releaseTimerAtDeathMs(0x0800) == DEATH_RELEASE_NO_TIMER);
    // Inside an instance the flag is clear: popup without countdown.
    CHECK(releaseTimerAtDeathMs(0x00) == DEATH_RELEASE_NO_TIMER);
    // NO_RELEASE_WINDOW: no popup at all, which FrameXML reads from 0.
    CHECK(releaseTimerAtDeathMs(0x10) == 0);
    CHECK(releaseTimerAtDeathMs(0x18) == 0);
}

TEST_CASE("GetReleaseTimeRemaining counts down in seconds while waiting to release", "[death]") {
    CHECK(releaseTimeRemainingSec(true, true, 0, 1000) == -1.0);
    CHECK(releaseTimeRemainingSec(true, false, 0, 1000) == 0.0);
    const uint64_t death = 50000;
    const uint64_t deadline = death + DEATH_RELEASE_TIMER_MS;
    CHECK(releaseTimeRemainingSec(true, false, deadline, death) == 360.0);
    CHECK(releaseTimeRemainingSec(true, false, deadline, death + 1500) == 358.5);
    CHECK(releaseTimeRemainingSec(true, false, deadline, deadline) == 0.0);
    CHECK(releaseTimeRemainingSec(true, false, deadline, deadline + 5000) == 0.0);
}

TEST_CASE("GetReleaseTimeRemaining returns 0 once alive or ghost", "[death]") {
    const uint64_t death = 50000;
    const uint64_t deadline = death + DEATH_RELEASE_TIMER_MS;
    // After releasing spirit (ghost) or reviving (alive), waitingToRelease is false:
    // neither a remaining 6-minute deadline nor an instance no-timer flag may leak.
    CHECK(releaseTimeRemainingSec(false, false, deadline, death + 1500) == 0.0);
    CHECK(releaseTimeRemainingSec(false, true, 0, death + 1500) == 0.0);
    CHECK(releaseTimeRemainingSec(false, false, 0, death) == 0.0);
}
