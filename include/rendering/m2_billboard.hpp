#pragma once

#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace wowee::rendering {

/// A bone marked spherical billboard carries geometry that must always face
/// the camera: glow cards, flame sprites, the halo on a lamp. Orgrimmar's
/// bonfire keeps its glow on bone 0, flagged 0x8. Cylindrical billboards
/// (0x40) turn about model Z only: quest question marks.
inline constexpr uint32_t kM2BoneSphericalBillboard = 0x8;
inline constexpr uint32_t kM2BoneCylindricalBillboardZ = 0x40;

inline bool isBillboardBone(uint32_t flags) {
    return (flags & (kM2BoneSphericalBillboard | kM2BoneCylindricalBillboardZ)) != 0;
}

/// Model-space matrix of a billboard bone: its +X turned toward the camera.
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
                                     const glm::vec3& cameraModel) {
    const glm::vec3 turnedPivot(parent * glm::vec4(pivot + translation, 1.0f));
    const glm::mat3 inherited(parent);
    const glm::vec3 inheritedNormal = glm::cross(inherited[1], inherited[2]);
    glm::vec3 toCamera = cameraModel - turnedPivot;
    if (flags & kM2BoneCylindricalBillboardZ) toCamera.z = 0.0f;
    const float len2 = glm::dot(toCamera, toCamera);
    glm::mat3 orient(1.0f);
    if (len2 > 1e-8f && glm::dot(inheritedNormal, inheritedNormal) > 0.0f) {
        toCamera *= 1.0f / std::sqrt(len2);
        // Up is model +Z unless the view is nearly along it, where the
        // cross product collapses and the card would spin.
        glm::vec3 up(0.0f, 0.0f, 1.0f);
        if (std::abs(toCamera.z) > 0.999f) up = glm::vec3(0.0f, 1.0f, 0.0f);
        const glm::vec3 right = glm::normalize(glm::cross(up, toCamera));
        const glm::vec3 trueUp = glm::cross(toCamera, right);
        const glm::vec3 oldForward = glm::normalize(inheritedNormal);
        const glm::vec3 oldRight = glm::normalize(inherited[1]);
        const glm::vec3 oldUp = glm::cross(oldForward, oldRight);
        orient = glm::mat3(toCamera, right, trueUp) * glm::transpose(glm::mat3(oldForward, oldRight, oldUp));
    }
    return glm::translate(glm::mat4(1.0f), turnedPivot) * glm::mat4(orient * inherited) *
           glm::scale(glm::mat4(1.0f), scale) * glm::translate(glm::mat4(1.0f), -pivot);
}

} // namespace wowee::rendering
