#pragma once

#include "rendering/animation/animation_ids.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace wowee::rendering {

// A unit on a mount (UNIT_FIELD_MOUNTDISPLAYID) sits in Mount (91) while the
// mount model walks, runs and flies under it. Observed in the original
// (2.4.3.8606), Dragonmaw Wind Reaver 23330 fighting with drawn spells and
// melee: the rider's legs stay in the seat in every frame; its attacks and
// casts move only the upper body (_experiments/20261118T150000Z.npc_mount,
// burst-fight5). Its idle, locomotion and melee-ready stances are the seat
// itself: between swings the rider sat still.
//
// True for the animations a rider shows as Mount; the others (attacks,
// casts and their holds, wounds, emotes) play above the spine.
inline constexpr bool riderKeepsSeat(uint32_t animation) {
    switch (animation) {
        case anim::STAND:
        case anim::WALK:
        case anim::RUN:
        case anim::SPRINT:
        case anim::WALK_BACKWARDS:
        case anim::SHUFFLE_LEFT:
        case anim::SHUFFLE_RIGHT:
        case anim::SWIM_IDLE:
        case anim::SWIM:
        case anim::SWIM_LEFT:
        case anim::SWIM_RIGHT:
        case anim::SWIM_BACKWARDS:
        case anim::JUMP_START:
        case anim::JUMP:
        case anim::JUMP_END:
        case anim::FALL:
        case anim::FLY_FORWARD:
        case anim::FLY_IDLE:
        case anim::READY_UNARMED:
        case anim::READY_1H:
        case anim::READY_2H:
        case anim::READY_2H_LOOSE:
        case anim::MOUNT:
            return true;
        default:
            return false;
    }
}

// The mount model's scale: its own display's (CreatureDisplayInfo scale x
// CreatureModelData scale) times the rider's drawn scale. Original: the
// Wind Reaver's rider display 21546 is authored at 2.0 and its nether drake
// (mount display 21548, 1.0) is drawn twice the size of a player's; the
// same unit given rider display 1139 (1.0) by .npc setmodel draws the drake
// at 1.0 (reaver4 / reaver5-model1139 in the experiment above).
inline constexpr float remoteMountScale(float riderScale, float mountDisplayScale,
                                        float mountModelScale) {
    return riderScale * mountDisplayScale * mountModelScale;
}

// Where a rider is drawn: its origin on the mount's MountMain (M2 attachment
// 0) as the mount's current pose carries it - the attachment's bone matrix
// times its offset, in the mount's world transform (mount position, facing
// and scale) - and turned with that bone. The rider's own scale is its own.
// Original (2.4.3.8606), Tyrande Whisperwind 17948 (display 17873, 2.0) on
// white tiger 6080 (RidingFrostSabre, 1.5): the tiger's Stand pose carries
// MountMain from its bind position (-0.28, 0, 1.24) to (-0.66, 0.04, 1.03),
// tilted 6 degrees, and she sits there, on the saddle behind the shoulders
// (_experiments/20261118T221740.rider_seat in the workspace).
//
// `rotation` is CharacterRenderer's Euler triple: x pitch, y roll, z yaw,
// composed as yaw(Z) * pitch(X) * roll(Y).
struct RiderSeatPose {
    glm::vec3 position{0.0f};
    glm::vec3 rotation{0.0f};
};
inline RiderSeatPose riderSeatPose(const glm::mat4& mountAttachment) {
    const glm::vec3 x = glm::normalize(glm::vec3(mountAttachment[0]));
    const glm::vec3 y = glm::normalize(glm::vec3(mountAttachment[1]));
    const glm::vec3 z = glm::normalize(glm::vec3(mountAttachment[2]));
    // R = Rz(yaw) Rx(pitch) Ry(roll): R21 = sin(pitch), R20 = -cos(pitch)
    // sin(roll), R22 = cos(pitch) cos(roll), R01 = -sin(yaw) cos(pitch),
    // R11 = cos(yaw) cos(pitch); glm indexes [column][row].
    return {.position = glm::vec3(mountAttachment[3]),
            .rotation = glm::vec3(std::asin(std::clamp(y.z, -1.0f, 1.0f)),
                                  std::atan2(-x.z, z.z),
                                  std::atan2(-y.x, y.y))};
}

// What a dismount does to the rider's render instance. A map change clears the
// character renderer before the spawner forgets its units, so the spawner can
// hold a rider id whose instance is gone: nothing is left to unseat (touching
// it crashed). A rider that died keeps its corpse pose (CMaNGOS unmounts on
// death); a living one leaves the seat for its idle.
struct RiderDismount {
    bool unseat = false;
    bool playIdle = false;
};
inline constexpr RiderDismount riderDismount(bool riderInstanceExists, bool riderDead) {
    if (!riderInstanceExists) return {};
    return {.unseat = true, .playIdle = !riderDead};
}

} // namespace wowee::rendering
