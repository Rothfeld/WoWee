// Indoor-to-exterior WMO portal visibility traversal
// (rendering::isExteriorVisibleThroughPortals).

#include <catch_amalgamated.hpp>

#include "rendering/wmo_portal_visibility.hpp"

#include <glm/gtc/matrix_transform.hpp>

using wowee::rendering::Frustum;
using wowee::rendering::PortalGroupInfo;
using wowee::rendering::PortalPlaneInfo;
using wowee::rendering::PortalRefInfo;
using wowee::rendering::canDownwardHitClassifyWmoGroup;
using wowee::rendering::isDownwardHitInteriorCeilingBackface;
using wowee::rendering::isExteriorVisibleThroughPortals;
using wowee::rendering::kWmoGroupFlagExteriorLit;
using wowee::rendering::kWmoGroupFlagIndoor;
using wowee::rendering::kWmoGroupFlagOutdoor;
using wowee::rendering::kWmoGroupFlagShowExteriorSky;
using wowee::rendering::preferDownwardSurfaceHit;
using wowee::rendering::wmoGroupIsIndoorOnly;
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

TEST_CASE("Downward ceiling backface test distinguishes floors from ceilings hit from above", "[wmo][portal]") {
    const glm::vec3 down(0.0f, 0.0f, -1.0f);

    // CCW floor in XY plane: normal = (0, 0, +1), faces up toward ray origin -> NOT backface.
    const glm::vec3 f0(0.0f, 0.0f, 0.0f);
    const glm::vec3 f1(4.0f, 0.0f, 0.0f);
    const glm::vec3 f2(0.0f, 4.0f, 0.0f);
    CHECK_FALSE(isDownwardHitInteriorCeilingBackface(f0, f1, f2, down));

    // CW ceiling in XY plane: normal = (0, 0, -1), faces down into room -> backface when hit from above.
    const glm::vec3 c0(0.0f, 0.0f, 5.0f);
    const glm::vec3 c1(0.0f, 4.0f, 5.0f);
    const glm::vec3 c2(4.0f, 0.0f, 5.0f);
    CHECK(isDownwardHitInteriorCeilingBackface(c0, c1, c2, down));

    // Indoor-only classification excludes outdoor and mixed groups.
    CHECK(wmoGroupIsIndoorOnly(kWmoGroupFlagIndoor));
    CHECK_FALSE(wmoGroupIsIndoorOnly(kWmoGroupFlagOutdoor));
    CHECK_FALSE(wmoGroupIsIndoorOnly(kWmoGroupFlagIndoor | kWmoGroupFlagOutdoor));
    CHECK_FALSE(wmoGroupIsIndoorOnly(0u));

    // Tie-breaking prefers a front-facing floor over a coplanar back-facing triangle,
    // while strictly preferring a nearer ceiling hit (above room) over a deeper floor hit.
    CHECK(preferDownwardSurfaceHit(2.0f, false, 2.0005f, true));
    CHECK_FALSE(preferDownwardSurfaceHit(2.0005f, true, 2.0f, false));
    CHECK(preferDownwardSurfaceHit(8.27f, true, 19.97f, false));
    CHECK_FALSE(preferDownwardSurfaceHit(19.97f, false, 8.27f, true));
}

TEST_CASE("Shadow Grave horizontal stairwell portal visibility matches crypt geometry", "[wmo][portal]") {
    // Exact portal 0 of TBC md_cryptonerm.wmo (Shadow Grave, Deathknell):
    // horizontal plane at local Z = +0.615 with normal (0, 0, +1), dist = -0.615,
    // connecting group 0 (outdoor, 0x809) and group 1 (Crypt, indoor, 0x2a05).
    const std::vector<glm::vec3> verts = {
        {-0.63f, -2.95f, 0.615f},
        { 8.58f, -2.95f, 0.615f},
        { 8.58f,  2.72f, 0.615f},
        {-0.63f,  2.72f, 0.615f},
    };
    const std::vector<PortalPlaneInfo> portals = {
        {0, 4, glm::vec3(0.0f, 0.0f, 1.0f), -0.615f},
    };
    const std::vector<PortalRefInfo> refs = {
        {0, 1,  1},  // group 0 -> group 1
        {0, 0, -1},  // group 1 -> group 0
    };
    const std::vector<PortalGroupInfo> groups = {
        {kWmoGroupFlagOutdoor | 0x801u, 0, 1},  // group 0: outdoor entrance
        {kWmoGroupFlagIndoor  | 0x0a05u, 1, 1}, // group 1: Crypt stairs
    };

    // Climbing stairs inside group 1 looking up toward the outdoor doorway -> exterior visible.
    {
        const glm::vec3 cam(4.0f, 0.0f, -2.5f);
        const glm::mat4 vp = makeViewProj(cam, glm::vec3(0.0f, 0.0f, 1.0f));
        CHECK(isExteriorVisibleThroughPortals(
            groups, portals, verts, refs, 1, cam, makeFrustum(vp), vp, glm::mat4(1.0f)));
    }
    // Near the top of the stairs straddling Portal 0 -> exterior stays visible even looking horizontally.
    {
        const glm::vec3 cam(1.1f, 0.5f, 0.4f);
        const glm::mat4 vp = makeViewProj(cam, glm::vec3(-5.0f, 0.5f, 0.4f));
        CHECK(isExteriorVisibleThroughPortals(
            groups, portals, verts, refs, 1, cam, makeFrustum(vp), vp, glm::mat4(1.0f)));
    }
    // Deep inside group 1 looking away/down into the dark crypt -> exterior hidden.
    {
        const glm::vec3 cam(14.0f, -5.0f, -10.0f);
        const glm::mat4 vp = makeViewProj(cam, glm::vec3(18.0f, -10.0f, -14.0f));
        CHECK_FALSE(isExteriorVisibleThroughPortals(
            groups, portals, verts, refs, 1, cam, makeFrustum(vp), vp, glm::mat4(1.0f)));
    }
}

TEST_CASE("Downward group classification rejects enclosed indoor rooms from exterior-lit airspace", "[wmo][portal]") {
    constexpr uint32_t kOrgrimmarValleyFlags =
        kWmoGroupFlagIndoor | kWmoGroupFlagExteriorLit | 0x8801u;  // ogrimmar_134.wmo (bazzaar02, 0x0a841)
    constexpr uint32_t kOrgrimmarTowerInteriorFlags =
        kWmoGroupFlagIndoor | 0x0805u;  // ogrimmar_108.wmo (IntHouseGBottom01, 0x02805)
    constexpr uint32_t kIronforgeGreatForgeFlags =
        kWmoGroupFlagIndoor | 0x8000u;  // ironforge_064.wmo (The Great Forge, 0x0a000)

    // Exterior-seeing groups (outdoor, exterior-lit city districts, skylight) can always be hit from above.
    CHECK(canDownwardHitClassifyWmoGroup(kWmoGroupFlagOutdoor, false, false));
    CHECK(canDownwardHitClassifyWmoGroup(kOrgrimmarValleyFlags, true, false));
    CHECK(canDownwardHitClassifyWmoGroup(kOrgrimmarValleyFlags, false, false));

    // Strictly enclosed indoor room below an exterior-lit open stairwell (Orgrimmar Wind Rider Tower top):
    // viewer is in bazzaar02 (exterior-lit), outside every enclosed indoor box -> tower interior must NOT claim viewer.
    CHECK_FALSE(canDownwardHitClassifyWmoGroup(kOrgrimmarTowerInteriorFlags, false, false));

    // Inside the tower's own geometry box -> valid indoor classification.
    CHECK(canDownwardHitClassifyWmoGroup(kOrgrimmarTowerInteriorFlags, true, true));

    // High inside an enclosed cavern (Ironforge Great Forge 0x0a000) above a smaller enclosed room:
    // viewer IS inside an enclosed indoor box -> downward hit into lower enclosed room remains valid.
    CHECK(canDownwardHitClassifyWmoGroup(kIronforgeGreatForgeFlags, true, true));
    CHECK(canDownwardHitClassifyWmoGroup(kOrgrimmarTowerInteriorFlags, false, true));
}

TEST_CASE("Orgrimmar Wind Rider Tower portal visibility matches shipped TBC ogrimmar.wmo", "[wmo][portal]") {
    // Exact portals of TBC ogrimmar.wmo connecting group 108 (IntHouseGBottom01,
    // 0x02805, vertex z in [0.915, 30.811]) to group 134 (bazzaar02, 0x0a841):
    //   - Portal 58 (bottom doorway): n=(-0.985, -0.174, 0), d=-168.588, side=+1 from grp 108
    //   - Portal 98 (top horizontal stairwell opening): n=(0, 0, +1), d=-30.811, side=-1 from grp 108
    const std::vector<glm::vec3> verts = {
        // Portal 58 (bottom doorway, 4 verts)
        {-177.38f, 35.12f, 1.97f},
        {-176.33f, 29.18f, 1.97f},
        {-176.33f, 29.18f, 6.79f},
        {-177.38f, 35.12f, 6.79f},
        // Portal 98 (top horizontal stairwell pentagon at local z = 30.811, 5 verts)
        {-182.82f, 25.63f, 30.811f},
        {-184.44f, 34.04f, 30.811f},
        {-195.56f, 36.22f, 30.811f},
        {-198.88f, 26.66f, 30.811f},
        {-192.72f, 19.82f, 30.811f},
    };
    const std::vector<PortalPlaneInfo> portals = {
        {0, 4, glm::vec3(-0.985f, -0.174f, 0.0f), -168.588f},
        {4, 5, glm::vec3(0.0f, 0.0f, 1.0f), -30.811f},
    };
    const std::vector<PortalRefInfo> refs = {
        {0, 1,  1},  // grp 108 -> grp 134 via Portal 58
        {1, 1, -1},  // grp 108 -> grp 134 via Portal 98
    };
    const std::vector<PortalGroupInfo> groups = {
        {kWmoGroupFlagIndoor | 0x0805u, 0, 2},                              // local idx 0: grp 108 (tower interior)
        {kWmoGroupFlagIndoor | kWmoGroupFlagExteriorLit | 0x8801u, 0, 0},   // local idx 1: grp 134 (bazzaar02)
    };

    // 1. Standing on top of the Wind Rider Tower over the open stairwell (local z = 34.5 > 30.811):
    //    grp 108 must not claim the camera (canDownwardHitClassifyWmoGroup == false),
    //    and grp 134 itself sees exterior unconditionally.
    const glm::vec3 towerTopCam(-196.0f, 24.0f, 34.5f);
    CHECK_FALSE(canDownwardHitClassifyWmoGroup(groups[0].groupFlags, false, false));
    CHECK(wmoGroupSeesExterior(groups[1].groupFlags));
    {
        const glm::mat4 vp = makeViewProj(towerTopCam, glm::vec3(-204.0f, 13.0f, 31.0f));
        CHECK(isExteriorVisibleThroughPortals(
            groups, portals, verts, refs, 1, towerTopCam, makeFrustum(vp), vp, glm::mat4(1.0f)));
    }

    // 2. Walking up near the top of the spiral ramp inside grp 108 looking out through Portal 98 -> exterior visible.
    {
        const glm::vec3 cam(-195.0f, 25.0f, 29.0f);
        const glm::mat4 vp = makeViewProj(cam, glm::vec3(-190.0f, 28.0f, 32.0f));
        CHECK(isExteriorVisibleThroughPortals(
            groups, portals, verts, refs, 0, cam, makeFrustum(vp), vp, glm::mat4(1.0f)));
    }

    // 3. Straddling Portal 98 at the top lip of the stairwell looking horizontally -> exterior stays visible.
    {
        const glm::vec3 cam(-195.0f, 25.0f, 30.6f);
        const glm::mat4 vp = makeViewProj(cam, glm::vec3(-205.0f, 25.0f, 30.6f));
        CHECK(isExteriorVisibleThroughPortals(
            groups, portals, verts, refs, 0, cam, makeFrustum(vp), vp, glm::mat4(1.0f)));
    }

    // 4. Mid-ramp inside the enclosed tower looking down/into the spiral wall (neither portal in view) -> exterior hidden.
    {
        const glm::vec3 cam(-194.0f, 24.0f, 16.0f);
        const glm::mat4 vp = makeViewProj(cam, glm::vec3(-198.0f, 20.0f, 12.0f));
        CHECK_FALSE(isExteriorVisibleThroughPortals(
            groups, portals, verts, refs, 0, cam, makeFrustum(vp), vp, glm::mat4(1.0f)));
    }

    // 5. Bottom of the tower inside grp 108 looking out through Portal 58 -> exterior visible.
    {
        const glm::vec3 cam(-184.0f, 30.0f, 4.0f);
        const glm::mat4 vp = makeViewProj(cam, glm::vec3(-175.0f, 32.0f, 4.0f));
        CHECK(isExteriorVisibleThroughPortals(
            groups, portals, verts, refs, 0, cam, makeFrustum(vp), vp, glm::mat4(1.0f)));
    }
}

TEST_CASE("Terrain-floored indoor WMO group enclosure excludes outdoor terrain over underground caves", "[wmo][portal]") {
    using wowee::rendering::isRayHitBackface;
    using wowee::rendering::isTerrainPointEnclosedByIndoorGroup;
    using wowee::rendering::RaySurfaceHit;

    // Inward-facing +X wall at x=10 (normal points -X toward room interior):
    const glm::vec3 w0(10.0f, 0.0f, 0.0f);
    const glm::vec3 w1(10.0f, 0.0f, 4.0f);
    const glm::vec3 w2(10.0f, 4.0f, 0.0f);
    // cross(w1-w0, w2-w0) = cross((0,0,4), (0,4,0)) = (-16, 0, 0) -> points -X.
    // Ray from inside room (+X) hits front-face; ray from outside (-X) hits back-face.
    CHECK_FALSE(isRayHitBackface(w0, w1, w2, glm::vec3(1.0f, 0.0f, 0.0f)));
    CHECK(isRayHitBackface(w0, w1, w2, glm::vec3(-1.0f, 0.0f, 0.0f)));

    // Outdoor terrain in empty corner of underground cave AABB
    // (e.g. Bloodvenom River Tainted Ooze inside barrowdens_c.wmo Group 16 AABB):
    // no floor below, no ceiling above, no inward-facing walls around -> outdoors.
    CHECK_FALSE(isTerrainPointEnclosedByIndoorGroup(
        RaySurfaceHit::None, RaySurfaceHit::None, 0, 0));

    // Outdoor terrain directly above an underground cave ceiling (downward ray hits ceiling backface):
    CHECK_FALSE(isTerrainPointEnclosedByIndoorGroup(
        RaySurfaceHit::BackFace, RaySurfaceHit::None, 0, 0));
    CHECK_FALSE(isTerrainPointEnclosedByIndoorGroup(
        RaySurfaceHit::BackFace, RaySurfaceHit::FrontFace, 4, 0));

    // Outdoor terrain beside curved underground cave hitting exterior wall backfaces -> outdoors.
    CHECK_FALSE(isTerrainPointEnclosedByIndoorGroup(
        RaySurfaceHit::None, RaySurfaceHit::None, 0, 2));
    CHECK_FALSE(isTerrainPointEnclosedByIndoorGroup(
        RaySurfaceHit::None, RaySurfaceHit::None, 1, 1));

    // Point beneath an indoor floor (upward ray hits floor backface) -> outdoors.
    CHECK_FALSE(isTerrainPointEnclosedByIndoorGroup(
        RaySurfaceHit::None, RaySurfaceHit::BackFace, 4, 0));

    // Terrain directly beneath an indoor room ceiling (upward ray hits ceiling front-face) -> indoors.
    CHECK(isTerrainPointEnclosedByIndoorGroup(
        RaySurfaceHit::None, RaySurfaceHit::FrontFace, 0, 0));

    // Open-sky ADT-floored city valley (e.g. Orgrimmar Valley of Wisdom group 134):
    // surrounded by inward-facing canyon walls -> indoors.
    CHECK(isTerrainPointEnclosedByIndoorGroup(
        RaySurfaceHit::None, RaySurfaceHit::None, 4, 0));
    CHECK(isTerrainPointEnclosedByIndoorGroup(
        RaySurfaceHit::None, RaySurfaceHit::None, 3, 0));
    CHECK(isTerrainPointEnclosedByIndoorGroup(
        RaySurfaceHit::None, RaySurfaceHit::None, 2, 0));
    // Central pillar backface in 1 direction, 3 inward canyon walls -> still indoors.
    CHECK(isTerrainPointEnclosedByIndoorGroup(
        RaySurfaceHit::None, RaySurfaceHit::None, 3, 1));
    // Single glancing wall hit without enclosure -> outdoors.
    CHECK_FALSE(isTerrainPointEnclosedByIndoorGroup(
        RaySurfaceHit::None, RaySurfaceHit::None, 1, 0));
}

TEST_CASE("collectVisibleGroupsThroughPortals clips multi-hop portals to seen screen rect", "[wmo][portal]") {
    using wowee::rendering::collectVisibleGroupsThroughPortals;

    // Group 0 (valley) --p0 (left side of view)--> Group 1 (tunnel) --p1 (right side of view)--> Group 2 (distant canyon)
    // Both p0 and p1 lie inside the camera frustum, but their projected screen rects do not overlap,
    // so Group 2 is occluded behind the wall between p0 and p1 and must NOT be collected.
    std::vector<PortalPlaneInfo> portals;
    std::vector<glm::vec3> verts;
    const uint16_t p0 = addXPortal(portals, verts, 10.0f,  4.0f,  7.0f, 0.0f, 4.0f);  // left-of-center
    const uint16_t p1 = addXPortal(portals, verts, 20.0f, -8.0f, -4.0f, 0.0f, 4.0f);  // right-of-center
    const uint16_t p2 = addXPortal(portals, verts, 20.0f,  7.0f, 13.0f, 0.0f, 4.0f);  // aligned behind p0

    const std::vector<PortalRefInfo> refs = {
        {p0, 1, -1},  // grp 0 -> grp 1 (camera at x < 10 -> side < 0 -> side * -1 > 0)
        {p1, 2, -1},  // grp 1 -> grp 2 (non-overlapping with p0)
        {p2, 3, -1},  // grp 1 -> grp 3 (overlaps p0 in screen space)
    };
    const std::vector<PortalGroupInfo> groups = {
        {kWmoGroupFlagIndoor | kWmoGroupFlagExteriorLit, 0, 1},
        {kWmoGroupFlagIndoor, 1, 2},
        {kWmoGroupFlagIndoor | kWmoGroupFlagShowExteriorSky, 0, 0},
        {kWmoGroupFlagIndoor, 0, 0},
    };

    const glm::vec3 cam(0.0f, 0.0f, 2.0f);
    const glm::mat4 vp = makeViewProj(cam, glm::vec3(20.0f, 0.0f, 2.0f));
    const auto visible = collectVisibleGroupsThroughPortals(
        groups, portals, verts, refs, {0}, 0, cam, makeFrustum(vp), vp, glm::mat4(1.0f));

    auto has = [&](uint32_t g) {
        return std::find(visible.begin(), visible.end(), g) != visible.end();
    };
    CHECK(has(0));
    CHECK(has(1));
    CHECK_FALSE(has(2));
    CHECK(has(3));
}

TEST_CASE("collectVisibleGroupsThroughPortals rejects back-facing portals and preserves straddled doorway", "[wmo][portal]") {
    using wowee::rendering::collectVisibleGroupsThroughPortals;

    // Group 0 (room) --p0 (at x=10)--> Group 1 (hall) --p1 (back-facing at x=15)--> Group 2 (hidden)
    std::vector<PortalPlaneInfo> portals;
    std::vector<glm::vec3> verts;
    const uint16_t p0 = addXPortal(portals, verts, 10.0f, -2.0f, 2.0f, 0.0f, 4.0f);
    const uint16_t p1 = addXPortal(portals, verts, 15.0f, -2.0f, 2.0f, 0.0f, 4.0f);

    const std::vector<PortalRefInfo> refs = {
        {p0, 1, -1},  // front-facing from x < 10
        {p1, 2,  1},  // back-facing from x < 15 (MOPR side +1 expects camera at x > 15)
    };
    const std::vector<PortalGroupInfo> groups = {
        {kWmoGroupFlagIndoor, 0, 1},
        {kWmoGroupFlagIndoor, 1, 1},
        {kWmoGroupFlagIndoor, 0, 0},
    };

    // Inside Group 0 looking through p0: Group 1 visible, Group 2 rejected because p1 is back-facing.
    {
        const glm::vec3 cam(5.0f, 0.0f, 2.0f);
        const glm::mat4 vp = makeViewProj(cam, glm::vec3(20.0f, 0.0f, 2.0f));
        const auto visible = collectVisibleGroupsThroughPortals(
            groups, portals, verts, refs, {0}, 0, cam, makeFrustum(vp), vp, glm::mat4(1.0f));
        CHECK(std::find(visible.begin(), visible.end(), 1u) != visible.end());
        CHECK(std::find(visible.begin(), visible.end(), 2u) == visible.end());
    }

    // Camera straddling doorway p0 slightly past x=10 (x=10.2, within 0.35 tolerance): Group 1 stays visible.
    {
        const glm::vec3 cam(10.2f, 0.0f, 2.0f);
        const glm::mat4 vp = makeViewProj(cam, glm::vec3(20.0f, 0.0f, 2.0f));
        const auto visible = collectVisibleGroupsThroughPortals(
            groups, portals, verts, refs, {0}, 0, cam, makeFrustum(vp), vp, glm::mat4(1.0f));
        CHECK(std::find(visible.begin(), visible.end(), 1u) != visible.end());
    }
}

TEST_CASE("wmoGroupWithinViewDistance culls distant WMO groups past farclip while keeping occupied/straddling groups", "[wmo][farclip]") {
    using wowee::rendering::wmoGroupWithinViewDistance;

    const glm::vec3 cam(1962.0f, -4752.7f, 56.2f);
    // Camera inside or straddling group AABB -> distance 0, always visible
    CHECK(wmoGroupWithinViewDistance(
        glm::vec3(1900.0f, -4800.0f, 20.0f), glm::vec3(2000.0f, -4700.0f, 90.0f), cam, 477.0f));

    // Durotar Zeppelin tower (~625 yd west of Valley of Honor): culled at TBC default farclip=477,
    // visible at TBC max farclip=777 and WoWee default viewDistance=1900.
    const glm::vec3 zepMin(1940.0f, -4145.0f, 30.0f);
    const glm::vec3 zepMax(1980.0f, -4105.0f, 120.0f);
    CHECK_FALSE(wmoGroupWithinViewDistance(zepMin, zepMax, cam, 477.0f));
    CHECK(wmoGroupWithinViewDistance(zepMin, zepMax, cam, 777.0f));
    CHECK(wmoGroupWithinViewDistance(zepMin, zepMax, cam, 1900.0f));

    // Optional maxGroupDistance clamp when WOWEE_WMO_CULL distanceCulling is active
    CHECK_FALSE(wmoGroupWithinViewDistance(zepMin, zepMax, cam, 1900.0f, true, 500.0f));
}
