#pragma once

#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace wowee::rendering {

/// A bone marked spherical billboard carries geometry that must always face
/// the camera: glow cards, flame sprites, the halo on a lamp. Orgrimmar's
/// bonfire keeps its glow on bone 0, flagged 0x8. Cylindrical billboards turn
/// about one locked axis only: X (0x10, e.g. humanwatchtower.m2 /
/// tk_arcane_door_horiz.m2), Y (0x20, e.g. stasistotem.m2 /
/// tk_factory_door.m2), or Z (0x40, e.g. quest question marks).
inline constexpr uint32_t kM2BoneSphericalBillboard = 0x8;
inline constexpr uint32_t kM2BoneCylindricalBillboardX = 0x10;
inline constexpr uint32_t kM2BoneCylindricalBillboardY = 0x20;
inline constexpr uint32_t kM2BoneCylindricalBillboardZ = 0x40;

inline bool isBillboardBone(uint32_t flags) {
    return (flags & (kM2BoneSphericalBillboard | kM2BoneCylindricalBillboardX |
                     kM2BoneCylindricalBillboardY | kM2BoneCylindricalBillboardZ)) != 0;
}

/// Model-space matrix of a billboard bone: its +X turned toward the camera,
/// its +Z toward the world's up as seen from the model (`worldUpModel`, unit
/// length, model space). A weapon hangs in the hand at any angle: Thaladred
/// the Darkener's (20064) axe Axe_2H_OutlandRaid_D_02_Red keeps the glow
/// cards of bones 1/2 (0x208) upright in build 8606; the axe's own +Z laid
/// them across the haft (_experiments/20261118T130509Z.weapon_glow_orientation,
/// _experiments/20261118T124000Z.astromancer_glow thal*.jpg). For an upright
/// world doodad world up is its +Z. A Z-cylindrical billboard (0x40) turns
/// about that up only.
///
/// Aims the inherited card plane, not a local frame that the parent rotates
/// again. Sap_State_Head billboards orbit under two animated parents. Keeps
/// that orbit and inherited stretch, then rotates the resulting plane toward
/// the camera around its animated pivot. The bone's own rotation track is not
/// used. A collapsed inherited card stays collapsed; rotation preserves
/// authored stretches and mirrored axes, including nonuniform parent scaling.
///
/// Children of the bone ride the turned frame: the black smoke of the raid
/// warlock helm (Helm_Robe_RaidWarlock_D_01_BEF.m2, emitter 0 on bone 4, 0.43
/// yd along -X of billboard bone 1) stays behind the wearer's torso as seen
/// from the front and the side in the original, on High Astromancer
/// Solarian (18805).
inline glm::mat4 billboardBoneMatrix(uint32_t flags, const glm::vec3& pivot, const glm::vec3& translation,
                                     const glm::vec3& scale, const glm::mat4& parent,
                                     const glm::vec3& cameraModel, const glm::vec3& worldUpModel) {
    const glm::vec3 turnedPivot(parent * glm::vec4(pivot + translation, 1.0f));
    const glm::mat3 inherited(parent);
    glm::vec3 toCamera = cameraModel - turnedPivot;
    glm::mat3 orient(1.0f);

    if (flags & kM2BoneCylindricalBillboardX) {
        const float axisLen2 = glm::dot(inherited[0], inherited[0]);
        const glm::vec3 oldYRaw = glm::cross(inherited[2], inherited[0]);
        if (axisLen2 > 1e-12f && glm::dot(oldYRaw, oldYRaw) > 0.0f) {
            const glm::vec3 axisX = inherited[0] * (1.0f / std::sqrt(axisLen2));
            toCamera -= axisX * glm::dot(toCamera, axisX);
            const float len2 = glm::dot(toCamera, toCamera);
            if (len2 > 1e-8f) {
                const glm::vec3 newY = toCamera * (1.0f / std::sqrt(len2));
                const glm::vec3 newZ = glm::cross(axisX, newY);
                const glm::vec3 oldY = glm::normalize(oldYRaw);
                const glm::vec3 oldZ = glm::cross(axisX, oldY);
                orient = glm::mat3(axisX, newY, newZ) * glm::transpose(glm::mat3(axisX, oldY, oldZ));
            }
        }
    } else if (flags & kM2BoneCylindricalBillboardY) {
        const float axisLen2 = glm::dot(inherited[1], inherited[1]);
        const glm::vec3 oldXRaw = glm::cross(inherited[1], inherited[2]);
        if (axisLen2 > 1e-12f && glm::dot(oldXRaw, oldXRaw) > 0.0f) {
            const glm::vec3 axisY = inherited[1] * (1.0f / std::sqrt(axisLen2));
            toCamera -= axisY * glm::dot(toCamera, axisY);
            const float len2 = glm::dot(toCamera, toCamera);
            if (len2 > 1e-8f) {
                const glm::vec3 newX = toCamera * (1.0f / std::sqrt(len2));
                const glm::vec3 newZ = glm::cross(newX, axisY);
                const glm::vec3 oldX = glm::normalize(oldXRaw);
                const glm::vec3 oldZ = glm::cross(oldX, axisY);
                orient = glm::mat3(newX, axisY, newZ) * glm::transpose(glm::mat3(oldX, axisY, oldZ));
            }
        }
    } else {
        const glm::vec3 inheritedNormal = glm::cross(inherited[1], inherited[2]);
        glm::vec3 up = worldUpModel;
        if (flags & kM2BoneCylindricalBillboardZ) toCamera -= up * glm::dot(toCamera, up);
        const float len2 = glm::dot(toCamera, toCamera);
        if (len2 > 1e-8f && glm::dot(inheritedNormal, inheritedNormal) > 0.0f) {
            toCamera *= 1.0f / std::sqrt(len2);
            // Up is the world's up seen from the model, unless the view is nearly
            // along it, where the cross product collapses and the card would spin.
            if (std::abs(glm::dot(toCamera, up)) > 0.999f)
                up = glm::normalize(glm::cross(toCamera, glm::vec3(1.0f, 0.0f, 0.0f)));
            const glm::vec3 right = glm::normalize(glm::cross(up, toCamera));
            const glm::vec3 trueUp = glm::cross(toCamera, right);
            const glm::vec3 oldForward = glm::normalize(inheritedNormal);
            const glm::vec3 oldRight = glm::normalize(inherited[1]);
            const glm::vec3 oldUp = glm::cross(oldForward, oldRight);
            orient = glm::mat3(toCamera, right, trueUp) * glm::transpose(glm::mat3(oldForward, oldRight, oldUp));
        }
    }
    return glm::translate(glm::mat4(1.0f), turnedPivot) * glm::mat4(orient * inherited) *
           glm::scale(glm::mat4(1.0f), scale) * glm::translate(glm::mat4(1.0f), -pivot);
}

} // namespace wowee::rendering
