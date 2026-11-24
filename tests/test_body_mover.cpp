// The on-foot body mover against the situations it was derived from.
//
// The geometry here is made up, in the proportions of the places native was
// measured at (see rendering/body_mover.hpp): a floor with a wall leaning 84
// degrees, a tall hull at 64.6 degrees, a 57.4 degree bump with ground behind
// it, stairs, ledges. Native's own frames and WoWee's triangles for the real
// places are replayed by sim.cpp in the workspace experiment
// _experiments/20261123T182500Z.movement_collision_parity.
#include "rendering/body_mover.hpp"
#include <catch_amalgamated.hpp>

#include <cmath>
#include <vector>

using namespace wowee::rendering::movement;

namespace {

constexpr float kRadius = 0.305555552f;   // human male, as dumped by the client
constexpr float kHeight = 2.0312655f;
constexpr float kFrameRun = 7.0f / 30.0f; // one 30 fps frame at run speed
constexpr float kGravity = 19.29110527f;

using Mesh = std::vector<BodyTriangle>;

// A quad solid from the side `front` points to: triangles are one-sided.
void quad(Mesh& mesh, glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d,
          glm::vec3 front = {0.0f, 0.0f, 1.0f}) {
    if (glm::dot(glm::cross(b - a, c - a), front) < 0.0f) { std::swap(b, d); }
    mesh.push_back({a, b, c});
    mesh.push_back({a, c, d});
}

void floorAt(Mesh& mesh, float z, float x0 = -20.0f, float x1 = 20.0f) {
    quad(mesh, {x0, -20, z}, {x1, -20, z}, {x1, 20, z}, {x0, 20, z});
}

// A face rising from the line x = foot at z = base, leaning away from -x by
// `degrees` from the horizontal, `tall` yards high, solid from the -x side.
void slope(Mesh& mesh, float foot, float base, float degrees, float tall) {
    const float run = tall / std::tan(glm::radians(degrees));
    quad(mesh, {foot, -20, base}, {foot + run, -20, base + tall},
         {foot + run, 20, base + tall}, {foot, 20, base}, {-1.0f, 0.0f, 0.2f});
}

// The controller's side of the mover, as CameraController::moveBodyOnFoot.
struct Walker {
    glm::vec3 position;
    bool grounded = true;   // held: on ground, or perched
    bool perched = false;
    float verticalVelocity = 0.0f;
    float baseZ = 0.0f;

    BodyMove frame(const Mesh& mesh, glm::vec3 run, float dt = 1.0f / 30.0f) {
        BodyMove move;
        bool walked = false;
        if (position.z < baseZ) baseZ = position.z;
        if (grounded && verticalVelocity <= 0.0f) {
            const float budget = std::clamp(baseZ + kMaxStepUp - position.z, 0.0f, kMaxStepUp);
            move = walkBody(position, run, budget, kRadius, kHeight, mesh);
            const bool steppedOn = move.stepped &&
                (move.soleOnGround || move.position.z > position.z + 1e-3f);
            if (perched && steppedOn && !move.soleOnGround && move.support == BodySupport::Walkable)
                move.support = BodySupport::Steep;
            walked = !perched || steppedOn;
        }
        if (!walked) {
            const float drop = verticalVelocity * dt - 0.5f * kGravity * dt * dt;
            verticalVelocity -= kGravity * dt;
            move = fallBody(position, run + glm::vec3(0, 0, drop), kRadius, kHeight, mesh);
        }
        position = move.position;
        grounded = move.support != BodySupport::None;
        perched = move.support == BodySupport::Steep;
        if (grounded) verticalVelocity = 0.0f;
        const bool landed = !walked && move.support == BodySupport::Walkable;
        if (move.soleOnGround || landed || position.z < baseZ) baseZ = position.z;
        return move;
    }
};

} // namespace

TEST_CASE("A wall leaning 84 degrees is slid along and never climbed", "[body_mover]") {
    // Orgrimmar (1826.75, -4245.98): native keeps its height at 7 and 3.5
    // yd/s, 30 and 60 frames per second. A lifted body clears such a wall by
    // 0.105 yd per yard of lift, so a short frame's move fits above it.
    Mesh mesh;
    floorAt(mesh, 0.0f);
    slope(mesh, 2.0f, -0.5f, 84.0f, 10.0f);
    for (const float run : {kFrameRun, kFrameRun * 0.5f, kFrameRun * 0.25f}) {
        for (const glm::vec2 heading : {glm::vec2(1.0f, 0.0f), glm::normalize(glm::vec2(1.0f, 0.4f))}) {
            Walker walker{.position = {1.0f, 0.0f, 0.0f}};
            float furthestX = walker.position.x;
            for (int i = 0; i < 90; ++i) {
                const BodyMove move = walker.frame(mesh, glm::vec3(heading * run, 0.0f));
                INFO("run " << run << " frame " << i);
                REQUIRE(move.support == BodySupport::Walkable);
                REQUIRE(std::abs(walker.position.z) < 0.003f);
                furthestX = std::max(furthestX, walker.position.x);
            }
            // Stopped by the wall, at the body's shoulder.
            REQUIRE(furthestX < 2.0f);
            REQUIRE(furthestX > 2.0f - kRadius - 0.2f);
            // An oblique approach keeps moving along it.
            if (heading.y > 0.0f) REQUIRE(walker.position.y > 1.0f);
        }
    }
}

TEST_CASE("A tall face too steep to stand on is not stepped onto", "[body_mover]") {
    // The Northshire tree hull, 64.6 degrees and 7 yd tall: native slides
    // along it for seconds without rising.
    Mesh mesh;
    floorAt(mesh, 0.0f);
    slope(mesh, 2.0f, 0.0f, 64.6f, 7.0f);
    Walker walker{.position = {1.0f, 0.0f, 0.0f}};
    const glm::vec3 run(glm::normalize(glm::vec2(1.0f, 0.5f)) * kFrameRun, 0.0f);
    for (int i = 0; i < 60; ++i) {
        const BodyMove move = walker.frame(mesh, run);
        INFO("frame " << i);
        REQUIRE(move.support == BodySupport::Walkable);
        REQUIRE(std::abs(walker.position.z) < 0.003f);
    }
    REQUIRE(walker.position.y > 2.0f);
}

TEST_CASE("A steep bump with ground behind it is walked over", "[body_mover]") {
    // The Dustwallow plank ramp's 57.4 degree bump: native crosses it in two
    // frames at the full horizontal move.
    Mesh mesh;
    floorAt(mesh, 0.0f, -20.0f, 2.0f);
    slope(mesh, 2.0f, 0.0f, 57.4f, 0.5f);
    const float top = 2.0f + 0.5f / std::tan(glm::radians(57.4f));
    quad(mesh, {top, -20, 0.5f}, {20, -20, 0.5f}, {20, 20, 0.5f}, {top, 20, 0.5f});
    Walker walker{.position = {1.0f, 0.0f, 0.0f}};
    for (int i = 0; i < 12; ++i) {
        const float before = walker.position.x;
        walker.frame(mesh, {kFrameRun, 0.0f, 0.0f});
        INFO("frame " << i << " at " << walker.position.x << ", " << walker.position.z);
        REQUIRE(walker.position.x == Catch::Approx(before + kFrameRun).margin(1e-4));
    }
    REQUIRE(walker.grounded);
    REQUIRE_FALSE(walker.perched);
    REQUIRE(walker.position.z == Catch::Approx(0.5f).margin(0.003));
}

TEST_CASE("Stairs are climbed and descended a whole frame at a time", "[body_mover]") {
    Mesh mesh;
    floorAt(mesh, 0.0f, -20.0f, 2.0f);
    for (int stepIndex = 0; stepIndex < 6; ++stepIndex) {
        const float x = 2.0f + 0.5f * static_cast<float>(stepIndex);
        const float z = 0.4f * static_cast<float>(stepIndex);
        quad(mesh, {x, -20, z}, {x, -20, z + 0.4f}, {x, 20, z + 0.4f}, {x, 20, z}, {-1.0f, 0.0f, 0.0f});
        quad(mesh, {x, -20, z + 0.4f}, {x + 0.5f, -20, z + 0.4f}, {x + 0.5f, 20, z + 0.4f}, {x, 20, z + 0.4f});
    }
    floorAt(mesh, 2.4f, 5.0f, 20.0f);
    Walker walker{.position = {1.0f, 0.0f, 0.0f}};
    for (int i = 0; i < 30; ++i) {
        const float before = walker.position.x;
        const BodyMove move = walker.frame(mesh, {kFrameRun, 0.0f, 0.0f});
        INFO("up, frame " << i << " at " << walker.position.x << ", " << walker.position.z);
        // Held every frame: on a tread, or by the bevel on the next tread's nose.
        REQUIRE(move.support != BodySupport::None);
        REQUIRE(walker.position.x == Catch::Approx(before + kFrameRun).margin(1e-4));
    }
    REQUIRE(walker.position.z == Catch::Approx(2.4f).margin(0.003));
    for (int i = 0; i < 30; ++i) {
        const BodyMove move = walker.frame(mesh, {-kFrameRun, 0.0f, 0.0f});
        INFO("down, frame " << i << " at " << walker.position.x << ", " << walker.position.z);
        REQUIRE(move.support == BodySupport::Walkable);
    }
    REQUIRE(walker.position.z == Catch::Approx(0.0f).margin(0.003));
}

TEST_CASE("A step is one yard at most", "[body_mover]") {
    for (const float tall : {0.95f, 1.05f, 1.3f}) {
        Mesh mesh;
        floorAt(mesh, 0.0f, -20.0f, 2.0f);
        quad(mesh, {2, -20, 0}, {2, -20, tall}, {2, 20, tall}, {2, 20, 0}, {-1.0f, 0.0f, 0.0f});
        floorAt(mesh, tall, 2.0f, 20.0f);
        Walker walker{.position = {1.0f, 0.0f, 0.0f}};
        for (int i = 0; i < 40; ++i) walker.frame(mesh, {kFrameRun, 0.0f, 0.0f});
        INFO("ledge " << tall << " reached " << walker.position.x << ", " << walker.position.z);
        REQUIRE(walker.grounded);
        if (tall < 1.0f) {
            REQUIRE(walker.position.z == Catch::Approx(tall).margin(0.003));
            REQUIRE(walker.position.x > 3.0f);
        } else {
            // Not onto it, and not hung on its edge by the bevel either: from
            // there the rest of the yard does not reach the top.
            REQUIRE(walker.position.z == Catch::Approx(0.0f).margin(0.003));
            REQUIRE(walker.position.x < 2.0f);
        }
    }
}

TEST_CASE("Walking off a low ledge lowers the body; a high one drops it", "[body_mover]") {
    Mesh low;
    floorAt(low, 0.0f, -20.0f, 2.0f);
    floorAt(low, -0.44f, 2.0f, 20.0f);
    Walker walker{.position = {1.0f, 0.0f, 0.0f}};
    for (int i = 0; i < 8; ++i) {
        const BodyMove move = walker.frame(low, {kFrameRun, 0.0f, 0.0f});
        // Native: 0.44 yd down within one frame at the Anvilmar railing.
        REQUIRE(move.support == BodySupport::Walkable);
    }
    REQUIRE(walker.position.z == Catch::Approx(-0.44f).margin(0.003));

    Mesh high;
    floorAt(high, 0.0f, -20.0f, 2.0f);
    floorAt(high, -3.0f, 2.0f, 20.0f);
    Walker faller{.position = {1.0f, 0.0f, 0.0f}};
    int airborneFrames = 0;
    for (int i = 0; i < 40; ++i) {
        faller.frame(high, {kFrameRun, 0.0f, 0.0f});
        if (!faller.grounded) ++airborneFrames;
    }
    REQUIRE(faller.grounded);
    REQUIRE(faller.position.z == Catch::Approx(-3.0f).margin(0.003));
    // 3 yd at 19.29 yd/s^2 is 0.558 s, 17 frames; the first of the drop is
    // ridden down the ledge's edge on the bevel.
    REQUIRE(airborneFrames >= 10);
    REQUIRE(airborneFrames <= 19);
}

TEST_CASE("A falling body is turned aside by a steep face and keeps its drop", "[body_mover]") {
    // The Exodar table's sides: native falls along them in free fall while
    // its horizontal move is taken out.
    Mesh mesh;
    floorAt(mesh, 0.0f);
    slope(mesh, 2.0f, 0.0f, 74.6f, 4.0f);
    const glm::vec3 start(2.0f, 0.0f, 2.0f);  // above the face, feet clear of it
    const glm::vec3 delta(kFrameRun, 0.0f, -0.1f);
    const BodyMove move = fallBody(start, delta, kRadius, kHeight, mesh);
    REQUIRE(move.support == BodySupport::None);
    REQUIRE(move.position.z == Catch::Approx(start.z + delta.z).margin(1e-4));
    REQUIRE(move.position.x <= start.x + kFrameRun);
    // And it comes to rest on the floor at the face's foot.
    Walker walker{.position = start, .grounded = false};
    for (int i = 0; i < 40 && !walker.grounded; ++i) walker.frame(mesh, {kFrameRun, 0.0f, 0.0f});
    REQUIRE(walker.grounded);
    REQUIRE(walker.position.z == Catch::Approx(0.0f).margin(0.003));
}

TEST_CASE("A ceiling ends a jump and a low roof refuses a step", "[body_mover]") {
    Mesh mesh;
    floorAt(mesh, 0.0f);
    quad(mesh, {-20, -20, kHeight + 0.5f}, {20, -20, kHeight + 0.5f}, {20, 20, kHeight + 0.5f},
         {-20, 20, kHeight + 0.5f}, {0.0f, 0.0f, -1.0f});
    const BodyMove jump = fallBody({0, 0, 0.2f}, {0.0f, 0.0f, 1.0f}, kRadius, kHeight, mesh);
    REQUIRE(jump.ceiling);
    REQUIRE(jump.position.z < 0.5f);

    // A 0.9 yd ledge under a roof 0.5 yd over the head: no room on top.
    quad(mesh, {2, -20, 0}, {2, -20, 0.9f}, {2, 20, 0.9f}, {2, 20, 0}, {-1.0f, 0.0f, 0.0f});
    floorAt(mesh, 0.9f, 2.0f, 20.0f);
    Walker walker{.position = {1.0f, 0.0f, 0.0f}};
    for (int i = 0; i < 10; ++i) walker.frame(mesh, {kFrameRun, 0.0f, 0.0f});
    REQUIRE(walker.grounded);
    REQUIRE(walker.position.z == Catch::Approx(0.0f).margin(0.003));
    REQUIRE(walker.position.x < 2.0f);
}

TEST_CASE("A jump that reaches a ledge's edge with the bevel gets onto it", "[body_mover]") {
    // Native clears the Northshire fence hull, 1.60 to 1.65 yd tall, with a
    // jump whose sole peaks 1.64 yd up, and walks on across it.
    Mesh mesh;
    floorAt(mesh, 0.0f, -20.0f, 3.0f);
    quad(mesh, {3, -20, 0}, {3, -20, 1.7f}, {3, 20, 1.7f}, {3, 20, 0}, {-1.0f, 0.0f, 0.0f});
    floorAt(mesh, 1.7f, 3.0f, 20.0f);
    Walker walker{.position = {0.5f, 0.0f, 0.0f}};
    walker.grounded = false;
    walker.verticalVelocity = 7.9555f;  // native jump velocity: 1.64 yd apex
    for (int i = 0; i < 40; ++i) walker.frame(mesh, {kFrameRun, 0.0f, 0.0f});
    REQUIRE(walker.grounded);
    REQUIRE(walker.position.z == Catch::Approx(1.7f).margin(0.003));
    REQUIRE(walker.position.x > 3.0f);

    // A ledge beyond the bevel's reach is not got onto.
    Mesh tall;
    floorAt(tall, 0.0f, -20.0f, 3.0f);
    quad(tall, {3, -20, 0}, {3, -20, 2.4f}, {3, 20, 2.4f}, {3, 20, 0}, {-1.0f, 0.0f, 0.0f});
    floorAt(tall, 2.4f, 3.0f, 20.0f);
    Walker blocked{.position = {0.5f, 0.0f, 0.0f}};
    blocked.grounded = false;
    blocked.verticalVelocity = 7.9555f;
    for (int i = 0; i < 40; ++i) blocked.frame(tall, {kFrameRun, 0.0f, 0.0f});
    REQUIRE(blocked.grounded);
    REQUIRE(blocked.position.z == Catch::Approx(0.0f).margin(0.003));
}

TEST_CASE("A triangle is solid from its front only", "[body_mover]") {
    // Native walks through the back of the coral sheet at a Karabor Sewers
    // rock; solid from both sides it wedged WoWee in an endless fall.
    Mesh mesh;
    floorAt(mesh, 0.0f);
    quad(mesh, {2, -20, 0}, {2, -20, 3}, {2, 20, 3}, {2, 20, 0}, {-1.0f, 0.0f, 0.0f});
    Walker front{.position = {1.0f, 0.0f, 0.0f}};
    for (int i = 0; i < 20; ++i) front.frame(mesh, {kFrameRun, 0.0f, 0.0f});
    REQUIRE(front.position.x < 2.0f);
    Walker back{.position = {3.0f, 0.0f, 0.0f}};
    for (int i = 0; i < 20; ++i) back.frame(mesh, {-kFrameRun, 0.0f, 0.0f});
    REQUIRE(back.position.x < 0.0f);
    REQUIRE(back.grounded);
}

TEST_CASE("A fall wedged between steep faces hangs and does not gather speed", "[body_mover]") {
    // Two 70 degree faces meeting in a V over a pit: nothing to stand on.
    Mesh mesh;
    const float run = 4.0f / std::tan(glm::radians(70.0f));
    quad(mesh, {0, -20, 0}, {-run, -20, 4}, {-run, 20, 4}, {0, 20, 0}, {1.0f, 0.0f, 0.3f});
    quad(mesh, {0, -20, 0}, {run, -20, 4}, {run, 20, 4}, {0, 20, 0}, {-1.0f, 0.0f, 0.3f});
    Walker walker{.position = {0.0f, 0.0f, 3.0f}, .grounded = false};
    BodyMove last;
    float fastest = 0.0f;
    for (int i = 0; i < 90; ++i) {
        last = walker.frame(mesh, {0.0f, 0.0f, 0.0f});
        if (last.held) walker.verticalVelocity = std::max(walker.verticalVelocity, 0.0f);
        fastest = std::min(fastest, walker.verticalVelocity);
    }
    REQUIRE(last.support == BodySupport::None);
    REQUIRE(last.held);
    REQUIRE(walker.position.z > 0.0f);
    REQUIRE(fastest > -12.0f);   // the 3 yd drop into the V, no more
    const float restZ = walker.position.z;
    for (int i = 0; i < 30; ++i) {
        last = walker.frame(mesh, {0.0f, 0.0f, 0.0f});
        if (last.held) walker.verticalVelocity = std::max(walker.verticalVelocity, 0.0f);
    }
    REQUIRE(walker.position.z == Catch::Approx(restZ).margin(0.01));
}

TEST_CASE("A face the body stands in still carries it", "[body_mover]") {
    // Native set down 0.3 yd inside the steep face of a Karabor Sewers rubble
    // block, with nothing under it, hangs there. Overlaps ignored, WoWee fell
    // out of the world from that spot.
    Mesh floor;
    floorAt(floor, 0.0f);
    Walker sunk{.position = {0.0f, 0.0f, -0.6f}, .grounded = false};
    for (int i = 0; i < 30; ++i) {
        const BodyMove move = sunk.frame(floor, {0.0f, 0.0f, 0.0f});
        if (move.held) sunk.verticalVelocity = std::max(sunk.verticalVelocity, 0.0f);
    }
    REQUIRE(sunk.position.z == Catch::Approx(-0.6f).margin(0.01));
    REQUIRE(sunk.grounded);
    // And is free to move along it and out of it.
    const BodyMove out = fallBody(sunk.position, {0.5f, 0.0f, 1.0f}, kRadius, kHeight, floor);
    REQUIRE(out.position.z == Catch::Approx(0.4f).margin(0.01));
    REQUIRE(out.position.x == Catch::Approx(0.5f).margin(0.01));

    Mesh steep;
    slope(steep, 0.0f, -5.0f, 70.0f, 10.0f);   // through the body, nothing below
    Walker hung{.position = {1.9f, 0.0f, -0.3f}, .grounded = false};
    BodyMove last;
    for (int i = 0; i < 60; ++i) {
        last = hung.frame(steep, {0.0f, 0.0f, 0.0f});
        if (last.held) hung.verticalVelocity = std::max(hung.verticalVelocity, 0.0f);
    }
    REQUIRE(last.held);
    REQUIRE_FALSE(hung.grounded);
    REQUIRE(hung.position.z > -1.0f);
}

TEST_CASE("Sliding along a wall is not turned by a face only the lifted body would meet", "[body_mover]") {
    // Orgrimmar (1821.37, -4243.25, 42.08; coordinates here less (1821, -4243, 42)):
    // a ramp, a wall leaning 84 degrees, and above the wall's diagonal a face
    // leaning 77 degrees that stands out from it. Native, walking into the
    // wall 32 degrees off head-on, slides along the 84 degree face on the
    // ramp, (-0.1066, +0.0542, -0.0103) a frame. Turned aside by the upper
    // face - what stops the body lifted a yard - the move led into the lower
    // one and the body was stepped up it, 0.13 yd a frame.
    const Mesh mesh = {
        {{{-1.6428f, 2.4585f, -0.5194f}, {-1.4080f, 0.2563f, 0.0026f}, {6.9522f, -4.0107f, 0.8045f}}},
        {{{6.7141f, -4.9126f, 9.3635f}, {6.9522f, -4.0107f, 0.8045f}, {-1.4080f, 0.2563f, 0.0026f}}},
        {{{-1.6428f, 2.4585f, -0.5194f}, {6.8508f, -0.2847f, -0.3170f}, {7.7898f, 1.0786f, -0.8896f}}},
        {{{6.8508f, -0.2847f, -0.3170f}, {-1.6428f, 2.4585f, -0.5194f}, {6.9522f, -4.0107f, 0.8045f}}},
        {{{-1.4080f, 0.2563f, 0.0026f}, {-1.5737f, -1.5615f, 8.1004f}, {6.7141f, -4.9126f, 9.3635f}}},
    };
    const glm::vec3 from(0.374f, -0.2495f, 0.0769f);
    const glm::vec3 run = glm::vec3(std::cos(-2.578f), std::sin(-2.578f), 0.0f) * kFrameRun;
    const auto rest = lowerBody(from + glm::vec3(0, 0, 0.05f), 0.1f, kRadius, kHeight, mesh);
    REQUIRE(rest);
    const BodyMove move = walkBody(rest->position, run, kMaxStepUp, kRadius, kHeight, mesh);
    const glm::vec3 moved = move.position - rest->position;
    CHECK(move.support == BodySupport::Walkable);
    CHECK(moved.x == Catch::Approx(-0.1066f).margin(0.003f));
    CHECK(moved.y == Catch::Approx(0.0542f).margin(0.003f));
    CHECK(moved.z == Catch::Approx(-0.0103f).margin(0.003f));
}

TEST_CASE("A wall met before any ground is walked up to, then slid along", "[body_mover]") {
    // Anvilmar, a post leaning 85 degrees on a level floor (-6065.74, 394.82,
    // 392.76; its face along y = 395.17 at the floor). Native, 10 degrees off
    // head-on with 0.097 yd of free run left, ends that frame against the
    // face: (+0.0400, +0.0936, 0), the whole sideways part of the move and
    // the way to the wall. Where the floor rises toward a wall the level move
    // meets ground first, and native slides from where it stands instead
    // (the Orgrimmar wall on its ramp, 3,400 frames).
    const float lean = std::tan(glm::radians(85.0f));
    const glm::vec3 run = glm::vec3(std::cos(1.396f), std::sin(1.396f), 0.0f) * 0.231f;
    const auto wallAt = [&](Mesh& mesh, float y, float base) {
        quad(mesh, {-5, y, base}, {5, y, base}, {5, y + 4.0f / lean, base + 4}, {-5, y + 4.0f / lean, base + 4}, {0, -1, 0});
    };
    SECTION("level floor") {
        Mesh mesh;
        floorAt(mesh, 0.0f);
        wallAt(mesh, 0.3515f, 0.0f);
        const auto rest = lowerBody({0, 0, 0.05f}, 0.1f, kRadius, kHeight, mesh);
        REQUIRE(rest);
        const BodyMove move = walkBody(rest->position, run, kMaxStepUp, kRadius, kHeight, mesh);
        const glm::vec3 moved = move.position - rest->position;
        CHECK(move.support == BodySupport::Walkable);
        CHECK(moved.x == Catch::Approx(0.0400f).margin(0.002f));
        CHECK(moved.y == Catch::Approx(0.0936f).margin(0.003f));
        CHECK(std::abs(moved.z) < 0.002f);
    }
    SECTION("floor rising toward the wall") {
        Mesh mesh;
        const float rise = std::tan(glm::radians(17.0f));
        quad(mesh, {-5, -5, -5 * rise}, {5, -5, -5 * rise}, {5, 5, 5 * rise}, {-5, 5, 5 * rise});
        wallAt(mesh, 0.3515f, 0.3515f * rise);
        const auto rest = lowerBody({0, 0, 0.05f}, 0.1f, kRadius, kHeight, mesh);
        REQUIRE(rest);
        const BodyMove move = walkBody(rest->position, run, kMaxStepUp, kRadius, kHeight, mesh);
        const glm::vec3 moved = move.position - rest->position;
        CHECK(move.support == BodySupport::Walkable);
        CHECK(moved.x == Catch::Approx(0.0400f).margin(0.002f));
        CHECK(std::abs(moved.y) < 0.002f);
    }
}

TEST_CASE("Scale above 1 grows the body and its step; below 1 it does not shrink them", "[body_mover]") {
    // Native: stopped by the Anvilmar post at the same place at scales 0.7 and
    // 1, a radius times 1.5 and 2 further out at 1.5 and 2; lifted 1, 1, 1.25,
    // 1.5 and 2 yd at the Orgrimmar wall at scales 0.7, 1, 1.25, 1.5 and 2.
    CHECK(bodyScale(0.7f) == 1.0f);
    CHECK(bodyScale(1.0f) == 1.0f);
    CHECK(bodyScale(1.5f) == 1.5f);
    CHECK(bodyScale(2.0f) == 2.0f);
    // A ledge 1.3 yd up: a step for scale 1.5, a wall for scale 1.
    Mesh mesh;
    floorAt(mesh, 0.0f, -20.0f, 2.0f);
    quad(mesh, {2, -20, 0}, {2, 20, 0}, {2, 20, 1.3f}, {2, -20, 1.3f}, {-1, 0, 0});
    quad(mesh, {2, -20, 1.3f}, {20, -20, 1.3f}, {20, 20, 1.3f}, {2, 20, 1.3f});
    const glm::vec3 run(kFrameRun, 0.0f, 0.0f);
    for (const float scale : {1.0f, 1.5f}) {
        const float factor = bodyScale(scale);
        const float radius = kRadius * factor, height = kHeight * factor, maxStep = kMaxStepUp * factor;
        glm::vec3 position(1.0f, 0.0f, 0.0f);
        float base = 0.0f;
        for (int i = 0; i < 12; ++i) {
            const float budget = std::clamp(base + maxStep - position.z, 0.0f, maxStep);
            const BodyMove move = walkBody(position, run, budget, radius, height, mesh, kMinWalkableNormalZ, maxStep);
            position = move.position;
            if (move.soleOnGround) base = position.z;
        }
        INFO("scale " << scale << " ends at " << position.x << ", " << position.z);
        if (scale > 1.0f) CHECK(position.z == Catch::Approx(1.3f).margin(0.003f));
        else CHECK(std::abs(position.z) < 0.003f);
    }
}

TEST_CASE("A wall the body is already inside of turns aside a move deeper into it", "[body_mover]") {
    // Orgrimmar wall (1826.75, -4245.98): native, grown by half where it
    // stood against the wall, slides along it; sweeps pass what the body
    // overlaps, and WoWee walked through the wall.
    Mesh mesh;
    floorAt(mesh, 0.0f);
    slope(mesh, 2.0f, 0.0f, 84.0f, 9.0f);
    // The body's side 0.15 yd inside the wall's foot.
    const glm::vec3 from(2.0f - kRadius + 0.15f, 0.0f, 0.0f);
    const glm::vec3 run(glm::normalize(glm::vec2(1.0f, 1.0f)) * kFrameRun, 0.0f);
    SECTION("walking") {
        const BodyMove move = walkBody(from, run, kMaxStepUp, kRadius, kHeight, mesh);
        CHECK(move.position.x == Catch::Approx(from.x).margin(0.002f));
        CHECK(move.position.y == Catch::Approx(from.y + run.y).margin(0.002f));
    }
    SECTION("falling") {
        const BodyMove move = fallBody(from + glm::vec3(0, 0, 0.5f), run + glm::vec3(0, 0, -0.01f), kRadius, kHeight, mesh);
        CHECK(move.position.x == Catch::Approx(from.x).margin(0.002f));
        CHECK(move.position.y == Catch::Approx(from.y + run.y).margin(0.002f));
    }
    SECTION("leaving") {
        const BodyMove move = walkBody(from, -run, kMaxStepUp, kRadius, kHeight, mesh);
        CHECK(move.position.x == Catch::Approx(from.x - run.x).margin(0.002f));
    }
}

TEST_CASE("Walking off ground lowers the body by its bevel's slope, then it falls", "[body_mover]") {
    // Anvilmar railing (-6083.3, 389.5), the Anvilmar post, Karabor Sewers:
    // native's last walking frame before a fall goes down 0.4271 yd for a
    // 0.2310 yd move with nothing under it, 16 frames of 16, and the fall
    // starts from there in the next frame.
    Mesh mesh;
    floorAt(mesh, 0.0f, -20.0f, 0.0f);
    floorAt(mesh, -5.0f, 0.0f, 20.0f);
    const glm::vec3 run(kFrameRun, 0.0f, 0.0f);
    SECTION("nothing within reach") {
        // The body's trailing bevel is already clear of the ledge's edge.
        const glm::vec3 from(kRadius + 0.05f, 0.0f, 0.0f);
        const BodyMove move = walkBody(from, run, kMaxStepUp, kRadius, kHeight, mesh);
        CHECK(move.support == BodySupport::None);
        CHECK(move.position.x == Catch::Approx(from.x + kFrameRun).margin(1e-4f));
        CHECK(move.position.z == Catch::Approx(-kFrameRun * kBodyBevel).margin(1e-4f));
    }
    SECTION("ground a step below is not snapped to from beyond that reach") {
        Mesh steps;
        floorAt(steps, 0.0f, -20.0f, 0.0f);
        floorAt(steps, -0.8f, 0.0f, 20.0f);
        const glm::vec3 from(kRadius + 0.05f, 0.0f, 0.0f);
        const BodyMove move = walkBody(from, run, kMaxStepUp, kRadius, kHeight, steps);
        CHECK(move.support == BodySupport::None);
        CHECK(move.position.z == Catch::Approx(-kFrameRun * kBodyBevel).margin(1e-4f));
    }
    SECTION("ground within reach is stood on") {
        Mesh steps;
        floorAt(steps, 0.0f, -20.0f, 0.0f);
        floorAt(steps, -0.3f, 0.0f, 20.0f);
        const glm::vec3 from(kRadius + 0.05f, 0.0f, 0.0f);
        const BodyMove move = walkBody(from, run, kMaxStepUp, kRadius, kHeight, steps);
        CHECK(move.support == BodySupport::Walkable);
        CHECK(move.soleOnGround);
        CHECK(move.position.z == Catch::Approx(-0.3f).margin(0.003f));
    }
}

TEST_CASE("A fall that ends on ground walks the rest of the frame's move", "[body_mover]") {
    // Beyond the Anvilmar railing (-6080.5, 389.5) native touches down 0.67
    // of the way through a frame and ends the frame its whole 0.2305 yd on.
    Mesh mesh;
    floorAt(mesh, 0.0f);
    const glm::vec3 from(0.0f, 0.0f, 0.1f);
    const glm::vec3 delta(kFrameRun, 0.0f, -0.2f);
    const BodyMove move = fallBody(from, delta, kRadius, kHeight, mesh);
    CHECK(move.support == BodySupport::Walkable);
    CHECK(move.soleOnGround);
    CHECK(move.position.x == Catch::Approx(kFrameRun).margin(1e-3f));
    CHECK(std::abs(move.position.z) < 0.003f);
}

TEST_CASE("A move turned along a face is not stopped by that face", "[body_mover]") {
    // Orgrimmar, where the ramp meets the 53 degree skirt under the wall
    // (1816.17, -4242.35), WoWee's own triangles: ramp, ramp, wall, skirt.
    // At 7 ms frames the body, lowered into the crease between the ramp's
    // edge and the skirt, had its move turned along the skirt stopped by the
    // skirt, and stayed there for good.
    const Mesh mesh = {
        {{{1819.59204f, -4242.74365f, 42.0026093f}, {1819.35718f, -4240.5415f, 41.4806137f}, {1812.26331f, -4238.63721f, 40.9909668f}}},
        {{{1819.59204f, -4242.74365f, 42.0026093f}, {1812.26331f, -4238.63721f, 40.9909668f}, {1810.82983f, -4241.74561f, 41.7339783f}}},
        {{{1819.59204f, -4242.74365f, 42.0026093f}, {1810.06311f, -4243.84814f, 44.6516876f}, {1819.42627f, -4244.56152f, 50.1003723f}}},
        {{{1810.06311f, -4243.84814f, 44.6516876f}, {1819.59204f, -4242.74365f, 42.0026093f}, {1810.82983f, -4241.74561f, 41.7339783f}}},
    };
    const float dt = 0.00694f;
    const glm::vec3 run(std::cos(-2.578f) * 7.0f * dt, std::sin(-2.578f) * 7.0f * dt, 0.0f);
    const BodyMove settled = fallBody(glm::vec3(1816.8987f, -4242.4370f, 42.3f), glm::vec3(0.0f, 0.0f, -1.5f),
                                      kRadius, kHeight, mesh);
    REQUIRE(settled.support == BodySupport::Walkable);
    Walker walker{.position = settled.position, .baseZ = settled.position.z};
    for (int i = 0; i < 36; ++i) walker.frame(mesh, run, dt);
    // A quarter of a second along the skirt: 1.43 yd; stopped, 0.73.
    CHECK(glm::distance(glm::vec2(walker.position), glm::vec2(settled.position)) > 1.3f);
}

TEST_CASE("An edge two walls share is not stepped over where the move can go on along one of them", "[body_mover]") {
    // Orgrimmar (1819.59, -4242.74), WoWee's own triangles: four of the ramp,
    // the 84 degree wall, the 77.7 degree face above its diagonal, the 77.4
    // degree wall beyond the fold and the 53 degree skirt under that.
    // Sliding west along the first face the body is carried past its end and
    // its side meets the ridge the two upper faces share. Native goes on
    // along the first face ((-0.1434, +0.0542) and (-0.1240, +0.0454) a
    // frame), is free for a frame ((-0.2012, -0.1270, +0.0289)) and slides
    // along the second wall ((-0.2015, -0.0098, +0.0011)). Stepping over the
    // ridge lifted the body 0.34 and 0.55 yd in two frames, and it fell.
    const Mesh mesh = {
        {{{1819.59204f, -4242.74365f, 42.0026093f}, {1819.35718f, -4240.5415f, 41.4806137f}, {1812.26331f, -4238.63721f, 40.9909668f}}},
        {{{1819.59204f, -4242.74365f, 42.0026093f}, {1812.26331f, -4238.63721f, 40.9909668f}, {1810.82983f, -4241.74561f, 41.7339783f}}},
        {{{1819.35718f, -4240.5415f, 41.4806137f}, {1819.59204f, -4242.74365f, 42.0026093f}, {1827.95215f, -4247.01074f, 42.8044586f}}},
        {{{1827.85083f, -4243.28467f, 41.6829758f}, {1819.35718f, -4240.5415f, 41.4806137f}, {1827.95215f, -4247.01074f, 42.8044586f}}},
        {{{1827.71411f, -4247.9126f, 51.3634529f}, {1827.95215f, -4247.01074f, 42.8044586f}, {1819.59204f, -4242.74365f, 42.0026093f}}},
        {{{1819.59204f, -4242.74365f, 42.0026093f}, {1819.42627f, -4244.56152f, 50.1003723f}, {1827.71411f, -4247.9126f, 51.3634529f}}},
        {{{1819.59204f, -4242.74365f, 42.0026093f}, {1810.06311f, -4243.84814f, 44.6516876f}, {1819.42627f, -4244.56152f, 50.1003723f}}},
        {{{1810.06311f, -4243.84814f, 44.6516876f}, {1819.59204f, -4242.74365f, 42.0026093f}, {1810.82983f, -4241.74561f, 41.7339783f}}},
    };
    const float dt = 1.0f / 30.0f;
    const glm::vec3 run(std::cos(-2.578f) * 7.0f * dt, std::sin(-2.578f) * 7.0f * dt, 0.0f);
    const BodyMove settled = fallBody(glm::vec3(1820.296875f, -4242.701171875f, 42.3f), glm::vec3(0.0f, 0.0f, -1.5f),
                                      kRadius, kHeight, mesh);
    REQUIRE(settled.support == BodySupport::Walkable);
    Walker walker{.position = settled.position, .baseZ = settled.position.z};
    float highest = settled.position.z;
    for (int i = 0; i < 12; ++i) {
        walker.frame(mesh, run, dt);
        highest = std::max(highest, walker.position.z);
    }
    // Past the fold, on the ramp all the way.
    CHECK(walker.position.x < 1818.4f);
    CHECK(highest < settled.position.z + 0.01f);
}
