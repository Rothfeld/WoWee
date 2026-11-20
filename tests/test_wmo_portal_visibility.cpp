// Indoor-to-exterior WMO portal visibility traversal
// (rendering::isExteriorVisibleThroughPortals).

#include <catch_amalgamated.hpp>

#include "rendering/wmo_portal_visibility.hpp"

#include <glm/gtc/matrix_transform.hpp>

using wowee::rendering::Frustum;
using wowee::rendering::PortalGroupInfo;
using wowee::rendering::PortalPlaneInfo;
using wowee::rendering::PortalRefInfo;
using wowee::rendering::isExteriorVisibleThroughPortals;
using wowee::rendering::kWmoGroupFlagExteriorLit;
using wowee::rendering::kWmoGroupFlagIndoor;
using wowee::rendering::kWmoGroupFlagOutdoor;
using wowee::rendering::kWmoGroupFlagShowExteriorSky;
using wowee::rendering::wmoGroupSeesExterior;

namespace {

glm::mat4 makeViewProj(const glm::vec3& eye, const glm::vec3& target) {
    const glm::mat4 view = glm::lookAt(eye, target, glm::vec3(0.0f, 0.0f, 1.0f));
    const glm::mat4 proj = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 500.0f);
    return proj * view;
}

Frustum makeFrustum(const glm::mat4& vp) {
    Frustum f;
    f.extractFromMatrix(vp);
    return f;
}

// Helper to append a rectangular portal quad on the plane x = planeX, y in [y0, y1], z in [z0, z1]
// with normal = (+1, 0, 0) and plane equation normal . p + distance = 0 => distance = -planeX.
uint16_t addXPortal(std::vector<PortalPlaneInfo>& portals,
                    std::vector<glm::vec3>& verts,
                    float planeX, float y0, float y1, float z0, float z1) {
    const uint16_t start = static_cast<uint16_t>(verts.size());
    verts.push_back({planeX, y0, z0});
    verts.push_back({planeX, y1, z0});
    verts.push_back({planeX, y1, z1});
    verts.push_back({planeX, y0, z1});
    const uint16_t idx = static_cast<uint16_t>(portals.size());
    portals.push_back({start, 4, glm::vec3(1.0f, 0.0f, 0.0f), -planeX});
    return idx;
}

// Helper to append a rectangular portal quad on the plane y = planeY, x in [x0, x1], z in [z0, z1]
// with normal = (0, +1, 0) and distance = -planeY.
uint16_t addYPortal(std::vector<PortalPlaneInfo>& portals,
                    std::vector<glm::vec3>& verts,
                    float planeY, float x0, float x1, float z0, float z1) {
    const uint16_t start = static_cast<uint16_t>(verts.size());
    verts.push_back({x0, planeY, z0});
    verts.push_back({x1, planeY, z0});
    verts.push_back({x1, planeY, z1});
    verts.push_back({x0, planeY, z1});
    const uint16_t idx = static_cast<uint16_t>(portals.size());
    portals.push_back({start, 4, glm::vec3(0.0f, 1.0f, 0.0f), -planeY});
    return idx;
}

}  // namespace

TEST_CASE("Group flags classify exterior-seeing groups", "[wmo][portal]") {
    CHECK(wmoGroupSeesExterior(kWmoGroupFlagOutdoor));
    CHECK(wmoGroupSeesExterior(kWmoGroupFlagIndoor | kWmoGroupFlagOutdoor));
    CHECK(wmoGroupSeesExterior(kWmoGroupFlagIndoor | kWmoGroupFlagExteriorLit));
    CHECK(wmoGroupSeesExterior(kWmoGroupFlagIndoor | kWmoGroupFlagShowExteriorSky));
    CHECK(wmoGroupSeesExterior(0u));
    CHECK_FALSE(wmoGroupSeesExterior(kWmoGroupFlagIndoor));
    CHECK_FALSE(wmoGroupSeesExterior(kWmoGroupFlagIndoor | 0x0a05u));
}

TEST_CASE("Sealed indoor room with no portals hides exterior", "[wmo][portal]") {
    const std::vector<PortalGroupInfo> groups = {
        {kWmoGroupFlagIndoor, 0, 0},
    };
    const glm::vec3 cam(0.0f, 0.0f, 2.0f);
    const glm::mat4 vp = makeViewProj(cam, cam + glm::vec3(1.0f, 0.0f, 0.0f));
    CHECK_FALSE(isExteriorVisibleThroughPortals(
        groups, {}, {}, {}, 0, cam, makeFrustum(vp), vp, glm::mat4(1.0f)));
}

TEST_CASE("Direct doorway portal exposes exterior only when facing and in view", "[wmo][portal]") {
    // Room 0 (indoor) at x < 10, Room 1 (outdoor) at x > 10, portal at x = 10.
    // Room 0 is on the negative side of normal (+1, 0, 0) => side = -1.
    std::vector<PortalPlaneInfo> portals;
    std::vector<glm::vec3> verts;
    const uint16_t p0 = addXPortal(portals, verts, 10.0f, -2.0f, 2.0f, 0.0f, 4.0f);

    const std::vector<PortalRefInfo> refs = {
        {p0, 1, -1},
    };
    const std::vector<PortalGroupInfo> groups = {
        {kWmoGroupFlagIndoor, 0, 1},
        {kWmoGroupFlagOutdoor, 0, 0},
    };

    const glm::vec3 cam(0.0f, 0.0f, 2.0f);
    {
        // Looking straight at the doorway (+X) -> exterior visible.
        const glm::mat4 vp = makeViewProj(cam, glm::vec3(10.0f, 0.0f, 2.0f));
        CHECK(isExteriorVisibleThroughPortals(
            groups, portals, verts, refs, 0, cam, makeFrustum(vp), vp, glm::mat4(1.0f)));
    }
    {
        // Looking away from the doorway (-X) -> exterior hidden.
        const glm::mat4 vp = makeViewProj(cam, glm::vec3(-10.0f, 0.0f, 2.0f));
        CHECK_FALSE(isExteriorVisibleThroughPortals(
            groups, portals, verts, refs, 0, cam, makeFrustum(vp), vp, glm::mat4(1.0f)));
    }
    {
        // Standing right on the doorway threshold (slightly past plane within epsilon) looking out.
        const glm::vec3 thresholdCam(10.1f, 0.0f, 2.0f);
        const glm::mat4 vp = makeViewProj(thresholdCam, glm::vec3(20.0f, 0.0f, 2.0f));
        CHECK(isExteriorVisibleThroughPortals(
            groups, portals, verts, refs, 0, thresholdCam, makeFrustum(vp), vp, glm::mat4(1.0f)));
    }
}

TEST_CASE("Multi-hop straight corridor sees exterior while bent corridor hides it", "[wmo][portal]") {
    // Room 0 (indoor, x < 10) -> Room 1 (indoor, 10 < x < 20) -> Room 2 (outdoor, x > 20).
    std::vector<PortalPlaneInfo> portals;
    std::vector<glm::vec3> verts;
    const uint16_t p01 = addXPortal(portals, verts, 10.0f, -2.0f, 2.0f, 0.0f, 4.0f);
    const uint16_t p12Aligned = addXPortal(portals, verts, 20.0f, -2.0f, 2.0f, 0.0f, 4.0f);
    const uint16_t p12Offset = addXPortal(portals, verts, 20.0f, 12.0f, 16.0f, 0.0f, 4.0f);

    const glm::vec3 cam(0.0f, 0.0f, 2.0f);
    const glm::mat4 vp = makeViewProj(cam, glm::vec3(20.0f, 0.0f, 2.0f));
    const Frustum frustum = makeFrustum(vp);

    {
        // Aligned second portal overlaps first portal's screen rect -> exterior visible.
        const std::vector<PortalRefInfo> refs = {
            {p01, 1, -1},
            {p12Aligned, 2, -1},
        };
        const std::vector<PortalGroupInfo> groups = {
            {kWmoGroupFlagIndoor, 0, 1},
            {kWmoGroupFlagIndoor, 1, 1},
            {kWmoGroupFlagOutdoor, 0, 0},
        };
        CHECK(isExteriorVisibleThroughPortals(
            groups, portals, verts, refs, 0, cam, frustum, vp, glm::mat4(1.0f)));
    }
    {
        // Offset second portal around a corner does not overlap first portal's screen rect -> hidden.
        const std::vector<PortalRefInfo> refs = {
            {p01, 1, -1},
            {p12Offset, 2, -1},
        };
        const std::vector<PortalGroupInfo> groups = {
            {kWmoGroupFlagIndoor, 0, 1},
            {kWmoGroupFlagIndoor, 1, 1},
            {kWmoGroupFlagOutdoor, 0, 0},
        };
        CHECK_FALSE(isExteriorVisibleThroughPortals(
            groups, portals, verts, refs, 0, cam, frustum, vp, glm::mat4(1.0f)));
    }
}

TEST_CASE("Second parallel archway to same room still exposes exterior", "[wmo][portal]") {
    // Room 0 has two archways into Room 1: left archway at y in [-8, -6], right archway at y in [6, 8].
    // Room 1 has an exterior exit aligned only with the right archway (y in [6, 8]).
    std::vector<PortalPlaneInfo> portals;
    std::vector<glm::vec3> verts;
    const uint16_t pLeft = addXPortal(portals, verts, 10.0f, -8.0f, -6.0f, 0.0f, 4.0f);
    const uint16_t pRight = addXPortal(portals, verts, 10.0f, 6.0f, 8.0f, 0.0f, 4.0f);
    const uint16_t pExit = addXPortal(portals, verts, 20.0f, 12.0f, 16.0f, 0.0f, 4.0f);

    const std::vector<PortalRefInfo> refs = {
        {pLeft, 1, -1},
        {pRight, 1, -1},
        {pExit, 2, -1},
    };
    const std::vector<PortalGroupInfo> groups = {
        {kWmoGroupFlagIndoor, 0, 2},
        {kWmoGroupFlagIndoor, 2, 1},
        {kWmoGroupFlagOutdoor, 0, 0},
    };

    const glm::vec3 cam(0.0f, 0.0f, 2.0f);
    const glm::mat4 vp = makeViewProj(cam, glm::vec3(15.0f, 0.0f, 2.0f));
    CHECK(isExteriorVisibleThroughPortals(
        groups, portals, verts, refs, 0, cam, makeFrustum(vp), vp, glm::mat4(1.0f)));
}

TEST_CASE("L-shaped corridor exit portal facing away from camera is culled", "[wmo][portal]") {
    // Room 0 (x < 10) -> Room 1 (x > 10) via portal at x = 10.
    // Room 1 has a portal at y = -5 facing -Y (Room 1 is on +Y side => side = +1),
    // but camera is at y = -10 (on the -Y side of y = -5) => sideDist < 0 => culled.
    std::vector<PortalPlaneInfo> portals;
    std::vector<glm::vec3> verts;
    const uint16_t p01 = addXPortal(portals, verts, 10.0f, -12.0f, -8.0f, 0.0f, 4.0f);
    const uint16_t p12 = addYPortal(portals, verts, -5.0f, 12.0f, 16.0f, 0.0f, 4.0f);

    const std::vector<PortalRefInfo> refs = {
        {p01, 1, -1},
        {p12, 2, 1},
    };
    const std::vector<PortalGroupInfo> groups = {
        {kWmoGroupFlagIndoor, 0, 1},
        {kWmoGroupFlagIndoor, 1, 1},
        {kWmoGroupFlagOutdoor, 0, 0},
    };

    const glm::vec3 cam(0.0f, -10.0f, 2.0f);
    const glm::mat4 vp = makeViewProj(cam, glm::vec3(14.0f, -10.0f, 2.0f));
    CHECK_FALSE(isExteriorVisibleThroughPortals(
        groups, portals, verts, refs, 0, cam, makeFrustum(vp), vp, glm::mat4(1.0f)));
}
