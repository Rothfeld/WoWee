#pragma once

#include <cstdint>

namespace wowee::game {

// Tracks a server-cancelled auto-repeat spell (Auto Shot 75, Wand Shoot 5019)
// across a stun so WoWee matches native TBC 2.4.3:
// - CMaNGOS sends SMSG_CANCEL_AUTO_REPEAT when Aura::HandleAuraModStun or
//   Unit::UpdateAutoRepeat (SPELL_FAILED_STUNNED) interrupts CURRENT_AUTOREPEAT_SPELL,
//   often in the same tick right before SMSG_UPDATE_OBJECT sets UNIT_FLAG_STUNNED.
// - Native 2.4.3 keeps ranged combat stance across the stun and resumes the
//   auto-repeat attack when the stun expires (unless cancelled, target changed,
//   or the player died).
struct AutoRepeatStunTracker {
    static constexpr float kStunCorrelationWindowSec = 0.5f;

    bool playerStunned = false;
    uint32_t recentCancelSpellId = 0;
    uint64_t recentCancelTargetGuid = 0;
    float recentCancelTimerSec = 0.0f;

    uint32_t suspendedSpellId = 0;
    uint64_t suspendedTargetGuid = 0;

    void onServerCancel(uint32_t spellId, uint64_t targetGuid) {
        if (spellId == 0 || targetGuid == 0) return;
        if (playerStunned) {
            suspendedSpellId = spellId;
            suspendedTargetGuid = targetGuid;
            recentCancelSpellId = 0;
            recentCancelTargetGuid = 0;
            recentCancelTimerSec = 0.0f;
        } else {
            recentCancelSpellId = spellId;
            recentCancelTargetGuid = targetGuid;
            recentCancelTimerSec = kStunCorrelationWindowSec;
        }
    }

    // Updates local stun state. When stun ends with a still-matching target,
    // returns the spell ID to resume and writes outTargetGuid; otherwise 0.
    uint32_t onStunChanged(bool nowStunned, uint64_t currentTargetGuid, uint64_t& outTargetGuid) {
        outTargetGuid = 0;
        const bool wasStunned = playerStunned;
        playerStunned = nowStunned;

        if (nowStunned && !wasStunned) {
            if (recentCancelSpellId != 0 && recentCancelTimerSec > 0.0f) {
                suspendedSpellId = recentCancelSpellId;
                suspendedTargetGuid = recentCancelTargetGuid;
            }
            recentCancelSpellId = 0;
            recentCancelTargetGuid = 0;
            recentCancelTimerSec = 0.0f;
            return 0;
        }

        if (!nowStunned && wasStunned) {
            const uint32_t spellId = suspendedSpellId;
            const uint64_t targetGuid = suspendedTargetGuid;
            suspendedSpellId = 0;
            suspendedTargetGuid = 0;
            if (spellId != 0 && targetGuid != 0 && targetGuid == currentTargetGuid) {
                outTargetGuid = targetGuid;
                return spellId;
            }
        }
        return 0;
    }

    void tick(float dt) {
        if (recentCancelTimerSec > 0.0f) {
            recentCancelTimerSec -= dt;
            if (recentCancelTimerSec <= 0.0f) {
                recentCancelTimerSec = 0.0f;
                recentCancelSpellId = 0;
                recentCancelTargetGuid = 0;
            }
        }
    }

    void onTargetChanged(uint64_t newTargetGuid) {
        if (suspendedTargetGuid != 0 && newTargetGuid != suspendedTargetGuid) {
            suspendedSpellId = 0;
            suspendedTargetGuid = 0;
        }
        if (recentCancelTargetGuid != 0 && newTargetGuid != recentCancelTargetGuid) {
            recentCancelSpellId = 0;
            recentCancelTargetGuid = 0;
            recentCancelTimerSec = 0.0f;
        }
    }

    void clear() {
        recentCancelSpellId = 0;
        recentCancelTargetGuid = 0;
        recentCancelTimerSec = 0.0f;
        suspendedSpellId = 0;
        suspendedTargetGuid = 0;
    }

    void resetAll() {
        playerStunned = false;
        clear();
    }

    [[nodiscard]] uint32_t activeSuspendedSpellId(uint64_t currentTargetGuid) const {
        if (playerStunned && suspendedSpellId != 0 &&
            suspendedTargetGuid != 0 && suspendedTargetGuid == currentTargetGuid) {
            return suspendedSpellId;
        }
        return 0;
    }
};

} // namespace wowee::game
