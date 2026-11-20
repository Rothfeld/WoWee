#pragma once

#include "rendering/frustum.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

#include <glm/glm.hpp>

namespace wowee::rendering {

constexpr uint32_t kWmoGroupFlagOutdoor = 0x8u;
constexpr uint32_t kWmoGroupFlagExteriorLit = 0x40u;
constexpr uint32_t kWmoGroupFlagShowExteriorSky = 0x100u;
constexpr uint32_t kWmoGroupFlagIndoor = 0x2000u;

struct PortalGroupInfo {
    uint32_t groupFlags = 0;
    uint16_t portalStart = 0;
    uint16_t portalCount = 0;
};

struct PortalPlaneInfo {
    uint16_t startVertex = 0;
    uint16_t vertexCount = 0;
    glm::vec3 normal{0.0f};
    float distance = 0.0f;
};

struct PortalRefInfo {
    uint16_t portalIndex = 0;
    uint16_t groupIndex = 0;
    int16_t side = 0;
};

/// Whether a WMO group itself exposes the outdoor world (exterior group,
/// exterior-lit courtyard/walkway, skylight group, or non-indoor group).
[[nodiscard]] inline bool wmoGroupSeesExterior(uint32_t groupFlags) {
    constexpr uint32_t kExteriorMask =
        kWmoGroupFlagOutdoor | kWmoGroupFlagExteriorLit | kWmoGroupFlagShowExteriorSky;
    return (groupFlags & kExteriorMask) != 0 || (groupFlags & kWmoGroupFlagIndoor) == 0;
}

/// Walk in-frustum, front-facing, screen-rect-clipped portals from `startGroup`
/// to determine whether any exterior-seeing WMO group is visible from the camera.
///
/// Build 8606 skips outdoor terrain, sky, grass, exterior WMOs, and ADT MDDF
/// doodads when the camera stands in an indoor WMO group whose visible portal
/// tree never reaches an exterior group. Without this, an outdoor tree standing
/// next to a keep wall (e.g. Shadowfang Keep shadowfang_27_32.adt MDDF uid
/// 219225 silverpinetree03.mdx at scale 1.49) clips its canopy right through
/// the enclosed indoor staircase behind Fenrus's room.
[[nodiscard]] inline bool isExteriorVisibleThroughPortals(
    const std::vector<PortalGroupInfo>& groups,
    const std::vector<PortalPlaneInfo>& portals,
    const std::vector<glm::vec3>& portalVertices,
    const std::vector<PortalRefInfo>& portalRefs,
    uint32_t startGroup,
    const glm::vec3& localCamPos,
    const Frustum& frustum,
    const glm::mat4& viewProj,
    const glm::mat4& modelMatrix) {
    if (startGroup >= groups.size()) return true;
    if (wmoGroupSeesExterior(groups[startGroup].groupFlags)) return true;
    if (portals.empty() || portalRefs.empty()) return false;

    using Rect = glm::vec4;  // (minX, minY, maxX, maxY) in NDC
    constexpr Rect kFullScreen(-1.0f, -1.0f, 1.0f, 1.0f);
    constexpr Rect kEmptyRect(1.0f, 1.0f, -1.0f, -1.0f);

    const glm::mat4 localToClip = viewProj * modelMatrix;

    auto portalInFrustum = [&](const PortalPlaneInfo& portal) -> bool {
        if (portal.vertexCount < 3) return false;
        if (static_cast<size_t>(portal.startVertex) + portal.vertexCount > portalVertices.size()) {
            return false;
        }
        glm::vec3 pMin = portalVertices[portal.startVertex];
        glm::vec3 pMax = pMin;
        for (uint16_t i = 1; i < portal.vertexCount; ++i) {
            const glm::vec3& v = portalVertices[portal.startVertex + i];
            pMin = glm::min(pMin, v);
            pMax = glm::max(pMax, v);
        }
        const glm::vec3 corners[8] = {
            {pMin.x, pMin.y, pMin.z}, {pMax.x, pMin.y, pMin.z},
            {pMin.x, pMax.y, pMin.z}, {pMax.x, pMax.y, pMin.z},
            {pMin.x, pMin.y, pMax.z}, {pMax.x, pMin.y, pMax.z},
            {pMin.x, pMax.y, pMax.z}, {pMax.x, pMax.y, pMax.z},
        };
        glm::vec3 worldMin(std::numeric_limits<float>::max());
        glm::vec3 worldMax(std::numeric_limits<float>::lowest());
        for (const auto& c : corners) {
            const glm::vec3 wc(modelMatrix * glm::vec4(c, 1.0f));
            worldMin = glm::min(worldMin, wc);
            worldMax = glm::max(worldMax, wc);
        }
        return frustum.intersectsAABB(worldMin, worldMax);
    };

    auto portalRect = [&](const PortalPlaneInfo& portal) -> Rect {
        Rect r = kEmptyRect;
        for (uint16_t i = 0; i < portal.vertexCount; ++i) {
            const glm::vec4 clip = localToClip * glm::vec4(portalVertices[portal.startVertex + i], 1.0f);
            if (clip.w <= 1e-3f) return kFullScreen;
            const glm::vec2 ndc = glm::vec2(clip) / clip.w;
            r.x = std::min(r.x, ndc.x);
            r.y = std::min(r.y, ndc.y);
            r.z = std::max(r.z, ndc.x);
            r.w = std::max(r.w, ndc.y);
        }
        return r;
    };

    auto portalStraddlesCamera = [&](const PortalPlaneInfo& portal, float pad) -> bool {
        if (portal.vertexCount < 3) return false;
        if (static_cast<size_t>(portal.startVertex) + portal.vertexCount > portalVertices.size()) {
            return false;
        }
        glm::vec3 pMin = portalVertices[portal.startVertex];
        glm::vec3 pMax = pMin;
        for (uint16_t i = 1; i < portal.vertexCount; ++i) {
            const glm::vec3& v = portalVertices[portal.startVertex + i];
            pMin = glm::min(pMin, v);
            pMax = glm::max(pMax, v);
        }
        return glm::all(glm::greaterThanEqual(localCamPos, pMin - glm::vec3(pad))) &&
               glm::all(glm::lessThanEqual(localCamPos, pMax + glm::vec3(pad)));
    };

    struct QueueEntry {
        uint32_t group;
        Rect rect;
    };
    std::vector<Rect> seenRect(groups.size(), kEmptyRect);
    std::vector<QueueEntry> queue;
    queue.reserve(groups.size() * 2);
    seenRect[startGroup] = kFullScreen;
    queue.push_back({startGroup, kFullScreen});

    // Small threshold tolerance for the camera's own room so standing right in
    // a doorway plane still sees out through that portal even when the portal
    // lies slightly behind the near clip plane.
    constexpr float kStartRoomPlaneEpsilon = -0.35f;
    constexpr float kDoorwayStraddlePad = 0.35f;
    const size_t maxExpansions = groups.size() * 4 + 64;

    for (size_t qi = 0; qi < queue.size() && qi < maxExpansions; ++qi) {
        const uint32_t current = queue[qi].group;
        const Rect parent = queue[qi].rect;
        const auto& grp = groups[current];
        const float minSide = (current == startGroup) ? kStartRoomPlaneEpsilon : 0.0f;

        for (uint16_t pi = 0; pi < grp.portalCount; ++pi) {
            const uint32_t refIdx = static_cast<uint32_t>(grp.portalStart) + pi;
            if (refIdx >= portalRefs.size()) break;
            const auto& ref = portalRefs[refIdx];
            if (ref.groupIndex >= groups.size() || ref.portalIndex >= portals.size()) continue;

            const auto& portal = portals[ref.portalIndex];
            const float sideDist =
                (glm::dot(portal.normal, localCamPos) + portal.distance) * static_cast<float>(ref.side);
            if (sideDist <= minSide) continue;
            const bool straddling = (current == startGroup) && portalStraddlesCamera(portal, kDoorwayStraddlePad);
            if (!straddling && !portalInFrustum(portal)) continue;

            const Rect through = straddling ? kFullScreen : portalRect(portal);
            const Rect clipped(std::max(through.x, parent.x), std::max(through.y, parent.y),
                               std::min(through.z, parent.z), std::min(through.w, parent.w));
            if (clipped.x >= clipped.z || clipped.y >= clipped.w) continue;

            if (wmoGroupSeesExterior(groups[ref.groupIndex].groupFlags)) {
                return true;
            }

            Rect& prev = seenRect[ref.groupIndex];
            const bool emptyPrev = (prev.x >= prev.z || prev.y >= prev.w);
            const bool expands = emptyPrev ||
                                 clipped.x < prev.x - 1e-4f || clipped.y < prev.y - 1e-4f ||
                                 clipped.z > prev.z + 1e-4f || clipped.w > prev.w + 1e-4f;
            if (!expands) continue;

            if (emptyPrev) {
                prev = clipped;
            } else {
                prev = Rect(std::min(prev.x, clipped.x), std::min(prev.y, clipped.y),
                            std::max(prev.z, clipped.z), std::max(prev.w, clipped.w));
            }
            queue.push_back({ref.groupIndex, clipped});
        }
    }

    return false;
}

}  // namespace wowee::rendering
