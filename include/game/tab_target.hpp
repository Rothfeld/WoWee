// tab_target.hpp - which units Tab and TargetNearestEnemy may pick.
//
// "Enemy" here means anything the player can attack, not only what is hostile.
// Neutral wildlife counts: the wolves around Coldridge Valley carry a faction
// template with no hostility at all, so asking for hostile units alone left Tab
// with nothing to cycle and it cleared the target instead. Critters are the
// exception the real client makes, and a unit the server marks as not
// selectable, not attackable or immune to players is skipped whatever its
// faction says.
//
// Which of those Tab picks, and in what order, was measured on the original
// 2.4.3 client with creatures placed at known distances and angles
// (_experiments/20261117T165413Z.tab_target_rule in the workspace):
//
//   - only units within 30 degrees of the character's facing count. At 10 to
//     35 yards a unit 29.99 degrees off was picked and one 30.01 degrees off
//     was not; the camera turned 36 degrees away from the facing did not move
//     this cone.
//   - the unit must also be on screen: with the camera turned 36 degrees to
//     one side, units inside the cone but outside the view were skipped.
//   - up to 41 yards in 3D: 40.94 yards was picked, 41.19 was not.
//   - nearest first, then each press steps one unit farther, wrapping to the
//     nearest. Units at 40.64, 40.81 and 40.90 yards came in 3D, not 2D, order.
//   - a press more than 3 seconds after the previous one starts again at the
//     nearest: 2.94 seconds continued the cycle, 3.19 seconds restarted it.
//   - a corpse is skipped.
#pragma once

#include "game/protocol_constants.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace wowee {
namespace game {

/// Creature type 8 in CreatureType.dbc.
constexpr uint32_t CREATURE_TYPE_CRITTER = 8;

[[nodiscard]] constexpr bool isTabTargetable(bool hostile, bool friendly,
                                             bool attackingPlayer,
                                             uint32_t creatureType,
                                             uint32_t unitFlags) {
    constexpr uint32_t kUntargetable =
        UNIT_FLAG_NOT_SELECTABLE | UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_IMMUNE_TO_PC;
    if (unitFlags & kUntargetable) return false;
    if (hostile || attackingPlayer) return true;
    if (friendly) return false;
    return creatureType != CREATURE_TYPE_CRITTER;
}

inline constexpr float kTabTargetHalfAngleDeg = 30.0f;
inline constexpr float kTabTargetRange = 41.0f;
inline constexpr uint64_t kTabCycleResetMs = 3000;

/// Whether a unit lies within the cone in front of the character. Positions
/// are canonical (x north, y west) and facing is canonical yaw, atan2(-dy, dx).
[[nodiscard]] inline bool isInTabCone(const glm::vec3& player, float facing,
                                      const glm::vec3& unit) {
    constexpr float kTwoPi = 6.28318530717958647692f;
    constexpr float kHalfAngle = kTabTargetHalfAngleDeg * kTwoPi / 360.0f;
    const float bearing = std::atan2(-(unit.y - player.y), unit.x - player.x);
    return std::abs(std::remainder(bearing - facing, kTwoPi)) <= kHalfAngle;
}

[[nodiscard]] inline bool isInTabRange(const glm::vec3& player, const glm::vec3& unit) {
    return glm::distance(player, unit) <= kTabTargetRange;
}

/// Whether a render-space point projects inside the camera's view.
[[nodiscard]] inline bool isOnScreen(const glm::mat4& viewProjection, const glm::vec3& renderPos) {
    const glm::vec4 clip = viewProjection * glm::vec4(renderPos, 1.0f);
    return clip.w > 0.0f && std::abs(clip.x) <= clip.w && std::abs(clip.y) <= clip.w;
}

struct TabCandidate {
    uint64_t guid;
    float distance;
};

/// The unit a press selects among the candidates: the nearest when nothing in
/// the list is targeted or the cycle restarts, otherwise the next one in
/// distance order (the previous one going backwards), wrapping at either end.
/// Equal distances are ordered by guid. Returns 0 when there is no candidate.
[[nodiscard]] inline uint64_t pickTabTarget(std::vector<TabCandidate> candidates, uint64_t current,
                                            bool restart, bool reverse) {
    if (candidates.empty()) return 0;
    std::sort(candidates.begin(), candidates.end(), [](const TabCandidate& a, const TabCandidate& b) {
        return a.distance != b.distance ? a.distance < b.distance : a.guid < b.guid;
    });
    const auto it = std::find_if(candidates.begin(), candidates.end(),
                                 [current](const TabCandidate& c) { return c.guid == current; });
    if (restart || current == 0 || it == candidates.end()) return candidates.front().guid;
    if (reverse) return it == candidates.begin() ? candidates.back().guid : (it - 1)->guid;
    const auto next = it + 1;
    return next == candidates.end() ? candidates.front().guid : next->guid;
}

} // namespace game
} // namespace wowee
