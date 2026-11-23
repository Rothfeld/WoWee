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

/// Whether a WMO group is strictly interior-only (indoor flag 0x2000 without
/// outdoor flag 0x8).
[[nodiscard]] inline bool wmoGroupIsIndoorOnly(uint32_t groupFlags) {
    return (groupFlags & kWmoGroupFlagIndoor) != 0 &&
           (groupFlags & kWmoGroupFlagOutdoor) == 0;
}

/// Whether a ray hitting triangle (v0, v1, v2) along `rayDir` struck its
/// back-face (normal pointing along `rayDir`, away from the ray origin).
/// Indoor WMO group geometry has inward-facing normals, so rays cast from
/// inside a room hit front-faces while rays from outside hit back-faces.
[[nodiscard]] inline bool isRayHitBackface(
    const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2,
    const glm::vec3& rayDir) {
    const glm::vec3 triNormal = glm::cross(v1 - v0, v2 - v0);
    return glm::dot(triNormal, rayDir) > 0.0f;
}

/// Whether a downward ray hitting triangle (v0, v1, v2) struck its back-face
/// (normal pointing along localDown, away from the ray origin). An indoor-only
/// room's ceiling has its front-face pointing down into the room, so a downward
/// ray from above the ceiling hits its back-face first.
[[nodiscard]] inline bool isDownwardHitInteriorCeilingBackface(
    const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2,
    const glm::vec3& localDown) {
    return isRayHitBackface(v0, v1, v2, localDown);
}

enum class RaySurfaceHit : uint8_t {
    None,
    FrontFace,
    BackFace,
};

/// Whether a point with no WMO floor below it (standing on ADT terrain inside
/// an indoor-only WMO group's vertex AABB) is actually enclosed by that group's
/// geometry rather than standing outdoors above its ceiling or in an empty
/// corner of its AABB (e.g. Bloodvenom River terrain above/beside Felwood's
/// underground `barrowdens_c.wmo` Group 16).
///
/// - `downHit`: nearest downward ray hit in this group; BackFace means the
///   point is above the group's ceiling (outside).
/// - `upHit`: nearest upward ray hit in this group; FrontFace means directly
///   beneath the group's ceiling (inside), BackFace means beneath its floor (outside).
/// - `horizFrontHits`, `horizBackHits`: counts of cardinal horizontal rays
///   (+X, -X, +Y, -Y) whose nearest hit in this group is front-facing vs
///   back-facing. Open-sky ADT-floored city valleys (e.g. Orgrimmar's Valley of
///   Wisdom group 134, Godan's Runeworks group 12) surround the point with
///   inward-facing walls, whereas outdoor terrain outside an underground cave
///   hits zero front-facing walls.
[[nodiscard]] inline bool isTerrainPointEnclosedByIndoorGroup(
    RaySurfaceHit downHit,
    RaySurfaceHit upHit,
    int horizFrontHits,
    int horizBackHits) {
    if (downHit == RaySurfaceHit::BackFace) return false;
    if (upHit == RaySurfaceHit::FrontFace) return true;
    if (upHit == RaySurfaceHit::BackFace) return false;
    return horizFrontHits >= 2 && horizFrontHits > horizBackHits;
}

/// Prefer the nearer downward surface hit; on coplanar ties (within 1mm),
/// prefer a front-facing floor over a back-facing ceiling/collision triangle.
[[nodiscard]] inline bool preferDownwardSurfaceHit(
    float candDist, bool candBackface, float bestDist, bool bestBackface) {
    constexpr float kTieEps = 1e-3f;
    if (candDist < bestDist - kTieEps) return true;
    if (candDist <= bestDist + kTieEps && !candBackface && bestBackface) return true;
    return false;
}

/// Whether a WMO group itself exposes the outdoor world (exterior group,
/// exterior-lit courtyard/walkway, skylight group, or non-indoor group).
[[nodiscard]] inline bool wmoGroupSeesExterior(uint32_t groupFlags) {
    constexpr uint32_t kExteriorMask =
        kWmoGroupFlagOutdoor | kWmoGroupFlagExteriorLit | kWmoGroupFlagShowExteriorSky;
    return (groupFlags & kExteriorMask) != 0 || (groupFlags & kWmoGroupFlagIndoor) == 0;
}

/// Whether a downward ray hit in `groupFlags` may classify a point as belonging
/// to that group.
///
/// Strictly enclosed indoor groups (`!wmoGroupSeesExterior`) must not claim a
/// viewer standing outside every enclosed indoor group's geometry bounds. For
/// example, in `ogrimmar.wmo`, the open-air Valley of Strength (`bazzaar02`,
/// group 134, 0x0a841 = indoor | mount | exterior-lit) encloses the Wind Rider
/// Tower top above the open stairwell of `IntHouseGBottom01` (group 108,
/// 0x02805, vertex zMax = 30.811 at Portal 98). A downward ray from a camera
/// above the open stairwell passes through the hole in group 134 and hits the
/// spiral ramp floor of group 108 below; without this check, the camera is
/// misclassified into group 108 on the exterior (+Z) side of Portal 98, which
/// culls Portal 98 as back-facing and hides the entire outdoor world.
///
/// When the point IS inside an enclosed indoor group's geometry bounds (e.g.
/// high inside Ironforge's Great Forge 0x0a000 over a lower room), downward
/// hits into enclosed rooms below remain valid.
[[nodiscard]] inline bool canDownwardHitClassifyWmoGroup(
    uint32_t groupFlags, bool pointInGroupGeometry, bool pointInAnyEnclosedIndoorGeometry) {
    if (wmoGroupSeesExterior(groupFlags)) {
        return true;
    }
    return pointInGroupGeometry || pointInAnyEnclosedIndoorGeometry;
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

/// Walk in-frustum, front-facing, screen-rect-clipped portals from `seedGroups`
/// and return every WMO group index visible through the portal graph.
///
/// Each hop clips the child portal's NDC rectangle against the parent's seen
/// rectangle and checks the MOPR side plane, matching `getVisibleGroupsFromOutside`
/// and `isExteriorVisibleThroughPortals`. Without screen-rect clipping and
/// front-facing plane tests, walking a city WMO such as `ogrimmar.wmo` from
/// Valley of Honor (`grp 53`) or Hall of the Brave (`grp 105`) leaks through
/// occluded tunnel portals (`53 -> 33 -> 32 -> 31 -> 30 -> 29 -> 34 -> 11 -> 0`)
/// and draws distant interior canyon arches (`BigCanyon01..03`, `0x00a905`)
/// across the open sky behind the palisade wall.
[[nodiscard]] inline std::vector<uint32_t> collectVisibleGroupsThroughPortals(
    const std::vector<PortalGroupInfo>& groups,
    const std::vector<PortalPlaneInfo>& portals,
    const std::vector<glm::vec3>& portalVertices,
    const std::vector<PortalRefInfo>& portalRefs,
    const std::vector<uint32_t>& seedGroups,
    uint32_t cameraSeedGroup,
    const glm::vec3& localCamPos,
    const Frustum& frustum,
    const glm::mat4& viewProj,
    const glm::mat4& modelMatrix) {
    using Rect = glm::vec4;  // (minX, minY, maxX, maxY) in NDC
    constexpr Rect kFullScreen(-1.0f, -1.0f, 1.0f, 1.0f);
    constexpr Rect kEmptyRect(1.0f, 1.0f, -1.0f, -1.0f);

    if (groups.empty()) return {};

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

    for (uint32_t gi : seedGroups) {
        if (gi >= groups.size()) continue;
        if (seenRect[gi].x < seenRect[gi].z) continue;
        seenRect[gi] = kFullScreen;
        queue.push_back({gi, kFullScreen});
    }

    if (!portals.empty() && !portalRefs.empty()) {
        constexpr float kStartRoomPlaneEpsilon = -0.35f;
        constexpr float kDoorwayStraddlePad = 0.35f;
        const size_t maxExpansions = groups.size() * 4 + 64;

        for (size_t qi = 0; qi < queue.size() && qi < maxExpansions; ++qi) {
            const uint32_t current = queue[qi].group;
            const Rect parent = queue[qi].rect;
            const auto& grp = groups[current];
            const float minSide = (current == cameraSeedGroup) ? kStartRoomPlaneEpsilon : 0.0f;

            for (uint16_t pi = 0; pi < grp.portalCount; ++pi) {
                const uint32_t refIdx = static_cast<uint32_t>(grp.portalStart) + pi;
                if (refIdx >= portalRefs.size()) break;
                const auto& ref = portalRefs[refIdx];
                if (ref.groupIndex >= groups.size() || ref.portalIndex >= portals.size()) continue;

                const auto& portal = portals[ref.portalIndex];
                const float sideDist =
                    (glm::dot(portal.normal, localCamPos) + portal.distance) * static_cast<float>(ref.side);
                if (sideDist <= minSide) continue;
                const bool straddling =
                    (current == cameraSeedGroup) && portalStraddlesCamera(portal, kDoorwayStraddlePad);
                if (!straddling && !portalInFrustum(portal)) continue;

                const Rect through = straddling ? kFullScreen : portalRect(portal);
                const Rect clipped(std::max(through.x, parent.x), std::max(through.y, parent.y),
                                   std::min(through.z, parent.z), std::min(through.w, parent.w));
                if (clipped.x >= clipped.z || clipped.y >= clipped.w) continue;

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
    }

    std::vector<uint32_t> visible;
    visible.reserve(groups.size());
    for (uint32_t gi = 0; gi < static_cast<uint32_t>(groups.size()); ++gi) {
        if (seenRect[gi].x < seenRect[gi].z && seenRect[gi].y < seenRect[gi].w) {
            visible.push_back(gi);
        }
    }
    return visible;
}

/// Whether any point of a WMO group's world AABB `[gMin, gMax]` lies within the
/// active `viewDistance` (`farclip`), optionally clamped to `maxGroupDistance`
/// when `clampToMaxGroupDistance` is enabled.
///
/// Matches TerrainRenderer and M2Renderer view-distance culling so distant WMOs
/// and their doodads (e.g. Durotar's OrcZeppelinTower.wmo at ~625 yd when
/// `farclip` is 477 yd) do not draw silhouettes past `viewDistance`.
[[nodiscard]] inline bool wmoGroupWithinViewDistance(
    const glm::vec3& gMin,
    const glm::vec3& gMax,
    const glm::vec3& camPos,
    float viewDistance,
    bool clampToMaxGroupDistance = false,
    float maxGroupDistance = 500.0f) {
    const float effectiveDistance =
        clampToMaxGroupDistance ? std::min(viewDistance, maxGroupDistance) : viewDistance;
    const glm::vec3 closestPoint = glm::clamp(camPos, gMin, gMax);
    const glm::vec3 delta = closestPoint - camPos;
    return glm::dot(delta, delta) <= effectiveDistance * effectiveDistance;
}

}  // namespace wowee::rendering
