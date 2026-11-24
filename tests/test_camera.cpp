#include "rendering/terrain_view_distance.hpp"
// Tests for rendering::Camera setter guards and degenerate-pose math.
#include <catch_amalgamated.hpp>
#include "rendering/camera.hpp"
#include "rendering/m2_foliage_culling.hpp"
#include "rendering/placement_transform.hpp"
#include <array>
#include <cmath>
#include <limits>

using namespace wowee::rendering;

TEST_CASE("Camera::setPosition rejects NaN/inf", "[camera]") {
    Camera cam;
    glm::vec3 originalPos = cam.getPosition();

    cam.setPosition(glm::vec3(std::numeric_limits<float>::quiet_NaN(), 0, 0));
    REQUIRE(cam.getPosition() == originalPos);

    cam.setPosition(glm::vec3(std::numeric_limits<float>::infinity(), 0, 0));
    REQUIRE(cam.getPosition() == originalPos);

    cam.setPosition(glm::vec3(10, 20, 30));
    REQUIRE(cam.getPosition() == glm::vec3(10, 20, 30));
}

TEST_CASE("Camera::setRotation rejects NaN", "[camera]") {
    Camera cam;
    cam.setRotation(45.0f, 30.0f);
    cam.setRotation(std::numeric_limits<float>::quiet_NaN(), 60.0f);
    // Yaw stays at 45, pitch stays at 30 (set was rejected wholesale)
    glm::vec3 fwdAfterBad = cam.getForward();
    cam.setRotation(45.0f, 30.0f);
    REQUIRE(cam.getForward() == fwdAfterBad);
}

TEST_CASE("Camera::setFov rejects NaN/zero/180+", "[camera]") {
    Camera cam;
    float originalFov = cam.getFovDegrees();

    cam.setFov(std::numeric_limits<float>::quiet_NaN());
    REQUIRE(cam.getFovDegrees() == originalFov);

    cam.setFov(0.0f);
    REQUIRE(cam.getFovDegrees() == originalFov);

    cam.setFov(-30.0f);
    REQUIRE(cam.getFovDegrees() == originalFov);

    cam.setFov(180.0f);
    REQUIRE(cam.getFovDegrees() == originalFov);

    cam.setFov(90.0f);
    REQUIRE(cam.getFovDegrees() == 90.0f);
}

TEST_CASE("Camera::setAspectRatio rejects non-positive", "[camera]") {
    Camera cam;
    float originalAspect = cam.getAspectRatio();
    cam.setAspectRatio(0.0f);
    REQUIRE(cam.getAspectRatio() == originalAspect);
    cam.setAspectRatio(-1.0f);
    REQUIRE(cam.getAspectRatio() == originalAspect);
    cam.setAspectRatio(std::numeric_limits<float>::quiet_NaN());
    REQUIRE(cam.getAspectRatio() == originalAspect);
    cam.setAspectRatio(2.0f);
    REQUIRE(cam.getAspectRatio() == 2.0f);
}

TEST_CASE("Camera::getRight/getUp return finite at +/-89 pitch", "[camera]") {
    Camera cam;
    cam.setRotation(0.0f, 89.0f);
    glm::vec3 r = cam.getRight();
    glm::vec3 u = cam.getUp();
    REQUIRE(std::isfinite(r.x));
    REQUIRE(std::isfinite(r.y));
    REQUIRE(std::isfinite(r.z));
    REQUIRE(std::isfinite(u.x));
    REQUIRE(std::isfinite(u.y));
    REQUIRE(std::isfinite(u.z));
}

TEST_CASE("Camera::getRight/getUp degrade safely at +/-90 pitch", "[camera]") {
    Camera cam;
    // Force the degenerate pose: forward exactly = world up
    cam.setRotation(0.0f, 90.0f);
    glm::vec3 r = cam.getRight();
    glm::vec3 u = cam.getUp();
    REQUIRE(std::isfinite(r.x));
    REQUIRE(std::isfinite(r.y));
    REQUIRE(std::isfinite(r.z));
    REQUIRE(std::isfinite(u.x));
    REQUIRE(std::isfinite(u.y));
    REQUIRE(std::isfinite(u.z));
    // Right should be the +X fallback (since cross would be zero).
    REQUIRE(r == glm::vec3(1.0f, 0.0f, 0.0f));
}

TEST_CASE("one-sided M2 foliage keeps its authored front through the camera", "[camera][m2]") {
    // A foliage triangle from StormPeaks_TreeB00.skin, index offsets 3..5.
    // Its vertex normals point to the same side as cross(v1-v0, v2-v0).
    // The old diagnostic used CLOCKWISE, which culled this side after the
    // camera's projection Y flip and left only the back of the snow tree.
    const std::array<glm::vec3, 3> local = {{
        {0.92906f, -2.31908f, 3.71287f},
        {1.52553f, -2.41602f, 1.92326f},
        {1.85329f, -0.93441f, 2.71328f},
    }};
    // Triangle 433 is almost coincident but faces the other way and uses
    // different UVs. Drawing both sides can make them compete for depth.
    const std::array<glm::vec3, 3> otherSide = {{
        {1.52439f, -2.41536f, 1.91937f},
        {0.92792f, -2.31842f, 3.70898f},
        {1.85215f, -0.93375f, 2.70939f},
    }};
    const glm::vec3 authoredNormal(0.47799f, -0.61636f, 0.62580f);
    const glm::vec3 localFace = glm::cross(local[1] - local[0], local[2] - local[0]);
    REQUIRE(glm::dot(localFace, authoredNormal) > 0.0f);
    REQUIRE(glm::length(local[0] - otherSide[1]) < 0.005f);
    REQUIRE(glm::length(local[1] - otherSide[0]) < 0.005f);
    REQUIRE(glm::length(local[2] - otherSide[2]) < 0.005f);
    REQUIRE(glm::dot(localFace, glm::cross(otherSide[1] - otherSide[0],
                                           otherSide[2] - otherSide[0])) < 0.0f);

    // ADT placements translate, rotate and positively scale the original mesh.
    // Look toward its front from each placement, using the actual Camera path.
    for (float angle : {0.0f, 0.8f, 3.1f}) {
        const glm::mat4 model = placementModelMatrix({100.0f, 200.0f, 10.0f},
                                                     {0.0f, 0.0f, angle}, 1.3f);
        std::array<glm::vec3, 3> world, worldOtherSide;
        for (size_t i = 0; i < world.size(); ++i) {
            world[i] = glm::vec3(model * glm::vec4(local[i], 1.0f));
            worldOtherSide[i] = glm::vec3(model * glm::vec4(otherSide[i], 1.0f));
        }
        const glm::vec3 center = (world[0] + world[1] + world[2]) / 3.0f;
        const glm::vec3 face = glm::normalize(glm::cross(world[1] - world[0],
                                                         world[2] - world[0]));
        for (float side : {1.0f, -1.0f}) {
            const glm::vec3 forward = -face * side;
            Camera camera;
            camera.setPosition(center + face * (12.0f * side));
            camera.setRotation(glm::degrees(std::atan2(forward.y, forward.x)),
                               glm::degrees(std::asin(forward.z)));

            auto projectedArea = [&camera](const std::array<glm::vec3, 3>& triangle) {
                std::array<glm::vec2, 3> screen;
                for (size_t i = 0; i < triangle.size(); ++i) {
                    const glm::vec4 clip = camera.getViewProjectionMatrix() * glm::vec4(triangle[i], 1.0f);
                    REQUIRE(clip.w > 0.0f);
                    screen[i] = glm::vec2(clip) / clip.w;
                }
                const glm::vec2 a = screen[1] - screen[0];
                const glm::vec2 b = screen[2] - screen[0];
                // Vulkan framebuffer area is -1/2 times the usual signed
                // shoelace area. A positive-height viewport preserves the sign.
                return -0.5f * (a.x * b.y - a.y * b.x);
            };
            REQUIRE(projectedArea(world) * side > 0.0f);
            REQUIRE(projectedArea(worldOtherSide) * side < 0.0f);
            REQUIRE(kM2FoliageFrontFace == VK_FRONT_FACE_COUNTER_CLOCKWISE);
        }
    }
}

TEST_CASE("foliage culling declines reflected and degenerate transforms", "[camera][m2]") {
    const glm::mat4 identity(1.0f);
    CHECK(m2FoliageTransformPreservesWinding(identity));
    CHECK(m2FoliageTransformPreservesWinding(glm::scale(identity, glm::vec3(1.0f, 2.0f, 3.0f))));
    CHECK(m2FoliageTransformPreservesWinding(glm::scale(identity, glm::vec3(-1.0f, -2.0f, 3.0f))));
    CHECK_FALSE(m2FoliageTransformPreservesWinding(glm::scale(identity, glm::vec3(-1.0f, 2.0f, 3.0f))));
    CHECK_FALSE(m2FoliageTransformPreservesWinding(glm::scale(identity, glm::vec3(0.0f, 2.0f, 3.0f))));
    glm::mat4 invalid = identity;
    invalid[0][0] = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(m2FoliageTransformPreservesWinding(invalid));
}

TEST_CASE("terrain distance preserves intersecting mountain chunks", "[camera][terrain]") {
    using wowee::rendering::terrainWithinViewDistance;
    const glm::vec3 camera(100.0f, -200.0f, 50.0f);
    for (float distance : {450.0f, 1900.0f}) {
        const float radius = 30.0f;
        CHECK(terrainWithinViewDistance(camera + glm::vec3(distance + 20, 0, 600),
                                        radius, camera, distance));
        CHECK(terrainWithinViewDistance(camera + glm::vec3(distance + radius, 0, 0),
                                        radius, camera, distance));
        CHECK_FALSE(terrainWithinViewDistance(camera + glm::vec3(distance + radius + 1, 0, 0),
                                              radius, camera, distance));
        CHECK(terrainWithinViewDistance(camera + glm::vec3(0, -distance - 20, -600),
                                        radius, camera, distance));
        CHECK_FALSE(terrainWithinViewDistance(camera + glm::vec3(distance, distance, 0),
                                              radius, camera, distance));
        CHECK(terrainWithinViewDistance(camera, 0, camera, distance));
    }
}

TEST_CASE("transport-locked camera yaw keeps ship deck stationary in screen space across harbor turns",
          "[camera][boatcam]") {
    // Ship deck quad in ship-local coordinates (bow along -X, port/starboard +/-Y).
    const std::array<glm::vec3, 4> localDeck = {{
        {-18.0f, -6.0f, 6.1f},
        {-18.0f,  6.0f, 6.1f},
        { 12.0f, -6.0f, 6.1f},
        { 12.0f,  6.0f, 6.1f},
    }};
    const glm::vec3 localPlayer(-2.0f, 0.5f, 6.1f);
    const float pivotOffsetZ = 1.6f;
    const float zoomDist = 11.5f;
    const float localCamYawDeg = -81.021f;
    const float pitchDeg = -12.0f;

    auto projectDeck = [&](const glm::vec3& shipRenderPos, float shipYawDeg) {
        const glm::mat4 hull = glm::translate(glm::mat4(1.0f), shipRenderPos) *
            glm::rotate(glm::mat4(1.0f), glm::radians(shipYawDeg), glm::vec3(0.0f, 0.0f, 1.0f));
        const glm::vec3 playerRender = glm::vec3(hull * glm::vec4(localPlayer, 1.0f));
        const glm::vec3 pivot = playerRender + glm::vec3(0.0f, 0.0f, pivotOffsetZ);

        Camera cam;
        cam.setRotation(shipYawDeg + localCamYawDeg, pitchDeg);
        cam.setPosition(pivot - cam.getForward() * zoomDist);
        const glm::mat4 vp = cam.getViewProjectionMatrix();

        std::array<glm::vec2, 4> ndc{};
        for (size_t i = 0; i < localDeck.size(); ++i) {
            const glm::vec4 worldPt = hull * glm::vec4(localDeck[i], 1.0f);
            const glm::vec4 clip = vp * worldPt;
            REQUIRE(clip.w > 0.0f);
            ndc[i] = glm::vec2(clip) / clip.w;
        }
        return ndc;
    };

    const auto dockScreen = projectDeck({-6406.0f, 823.0f, 0.0f}, -107.092f);
    for (float turnDeg : {-90.0f, -45.0f, 0.0f, 37.5f, 118.0f, 180.0f, 245.0f}) {
        const auto turnedScreen = projectDeck(
            {-6350.0f + turnDeg, 800.0f - 0.5f * turnDeg, 0.0f},
            -107.092f + turnDeg);
        for (size_t i = 0; i < dockScreen.size(); ++i) {
            // At ~6400 world coordinate magnitude, IEEE-754 float32 ULP is ~5e-4
            // (~2e-3 after grazing perspective divide), vs >1.0 NDC swing if unlocked.
            CHECK(turnedScreen[i].x == Catch::Approx(dockScreen[i].x).margin(5e-3f));
            CHECK(turnedScreen[i].y == Catch::Approx(dockScreen[i].y).margin(5e-3f));
        }
    }
}

TEST_CASE("Camera near plane matches Native TBC 2.4.3 (~0.22 yd) with D32_SFLOAT depth precision", "[camera][nearplane]") {
    Camera cam;
    CHECK(cam.getNearPlane() == Catch::Approx(0.22f));
    CHECK(cam.getFarPlane() > 1000.0f);

    // Close geometry at 0.30 yd (previously clipped by the old 0.50 yd near plane)
    // must project cleanly inside Vulkan NDC depth [0, 1].
    cam.setPosition(glm::vec3(0.0f, 0.0f, 0.0f));
    cam.setRotation(0.0f, 0.0f);
    const glm::mat4 vp = cam.getViewProjectionMatrix();

    auto projectZ = [&](float dist) {
        const glm::vec4 clip = vp * glm::vec4(dist, 0.0f, 0.0f, 1.0f);
        REQUIRE(clip.w > 0.0f);
        return clip.z / clip.w;
    };

    const float zNear = projectZ(0.30f);
    CHECK(zNear > 0.0f);
    CHECK(zNear < 1.0f);

    // Depth stays strictly monotonic and separable in float32 at world/WMO/water distances.
    const float z100a = projectZ(100.0f);
    const float z100b = projectZ(100.05f);
    CHECK(z100b > z100a);
    const float z500a = projectZ(500.0f);
    const float z500b = projectZ(500.5f);
    CHECK(z500b > z500a);
}

TEST_CASE("Scripted MoveView orbit/zoom and FlipCameraYaw advance and stop cleanly", "[camera][lua]") {
    ScriptedViewMotion motion;
    float yaw = 180.0f;
    float pitch = -20.0f;
    float dist = 5.55f;
    bool anglesChanged = false;
    bool distChanged = false;

    CHECK_FALSE(motion.active());
    CHECK_FALSE(motion.apply("right", 1.0f, yaw));
    CHECK_FALSE(motion.apply("up", 0.5f, yaw));
    CHECK_FALSE(motion.apply("out", 1.0f, yaw));
    CHECK(motion.active());

    motion.advance(0.5f, 180.0f, 90.0f, 10.0f, -89.0f, 89.0f, 20.0f,
                   yaw, pitch, dist, anglesChanged, distChanged);
    CHECK(anglesChanged);
    CHECK(distChanged);
    CHECK(yaw == Catch::Approx(90.0f));
    CHECK(pitch == Catch::Approx(2.5f));
    CHECK(dist == Catch::Approx(10.55f));

    CHECK_FALSE(motion.apply("right", 0.0f, yaw));
    CHECK_FALSE(motion.apply("up", 0.0f, yaw));
    CHECK_FALSE(motion.apply("out", 0.0f, yaw));
    CHECK_FALSE(motion.active());

    motion.advance(0.5f, 180.0f, 90.0f, 10.0f, -89.0f, 89.0f, 20.0f,
                   yaw, pitch, dist, anglesChanged, distChanged);
    CHECK_FALSE(anglesChanged);
    CHECK_FALSE(distChanged);
    CHECK(yaw == Catch::Approx(90.0f));

    CHECK(motion.apply("flip_yaw", 180.0f, yaw));
    CHECK(yaw == Catch::Approx(270.0f));
}
