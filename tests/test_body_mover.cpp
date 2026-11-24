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
    size_t terrain = 0;     // how many of the mesh's first triangles are the terrain's
    glm::vec3 walkedOntoSlope{0.0f};
    glm::vec3 airMove{0.0f};  // the level move of a frame in the air
    bool airborneBefore = false;  // a walker begun in the air leaves the ground with its first frame's run

    // One frame as CameraController::moveBodyOnFootOnce takes it: `run` is
    // the level move the forward key asks for.
    BodyMove frame(const Mesh& mesh, glm::vec3 run, float dt = 1.0f / 30.0f) {
        BodyMove move;
        bool walked = false;
        if (grounded || !airborneBefore) airMove = run;
        airborneBefore = true;
        if (position.z < baseZ) baseZ = position.z;
        const glm::vec3 from = position;
        if (grounded && verticalVelocity <= 0.0f) {
            const float budget = std::clamp(baseZ + kMaxStepUp - position.z, 0.0f, kMaxStepUp);
            move = walkBody(position, run, budget, kRadius, kHeight, mesh, kMinWalkableNormalZ, kMaxStepUp, terrain,
                            walkedOntoSlope);
            const bool steppedOn = move.stepped &&
                (move.soleOnGround || move.position.z > position.z + 1e-3f);
            if (perched && steppedOn && !move.soleOnGround && move.support == BodySupport::Walkable)
                move.support = BodySupport::Steep;
            walked = !perched || steppedOn;
        }
        if (!walked) {
            const float drop = verticalVelocity * dt - 0.5f * kGravity * dt * dt;
            verticalVelocity -= kGravity * dt;
            move = fallBody(position, airMove + glm::vec3(0, 0, drop), kRadius, kHeight, mesh,
                            kMinWalkableNormalZ, kMaxStepUp, terrain, run);
            if (move.held && verticalVelocity < 0.0f) verticalVelocity = 0.0f;
            if (move.deflected) airMove = glm::vec3(move.levelKept, 0.0f);
        } else if (move.support == BodySupport::None) {
            airMove = glm::vec3(move.position.x - from.x, move.position.y - from.y, 0.0f);
        }
        position = move.position;
        grounded = move.support != BodySupport::None;
        perched = move.support == BodySupport::Steep;
        walkedOntoSlope = walked ? move.walkedOntoSlope : glm::vec3(0.0f);
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

TEST_CASE("Ground rising under a wall that leans out over it is walked up to the wall", "[body_mover]") {
    // A floor rising a thousandth and a wall leaning 9.5 degrees out over
    // it: the head meets the wall with the sole at x = 4.356, and lifted a
    // whole yard with the sole at 4.19. A walker that takes its move either
    // level or lifted a yard stopped within a frame's move of 4.19 for good,
    // here at 3.967 (Black Temple, 607.25, 110.33; under the leaning trunk
    // at Orgrimmar native walks on, 70 frames of `org_rock_d`). Walking the
    // floor it stops within a frame's move of where its head meets the wall.
    const Mesh mesh = {
        {{{-10.0f, -10.0f, -0.01f}, {10.0f, -10.0f, 0.01f}, {10.0f, 10.0f, 0.01f}}},
        {{{-10.0f, -10.0f, -0.01f}, {10.0f, 10.0f, 0.01f}, {-10.0f, 10.0f, -0.01f}}},
        {{{5.0f, -5.0f, 0.0f}, {4.5f, -5.0f, 3.0f}, {5.0f, 5.0f, 0.0f}}},
        {{{5.0f, 5.0f, 0.0f}, {4.5f, -5.0f, 3.0f}, {4.5f, 5.0f, 3.0f}}},
    };
    const float dt = 1.0f / 30.0f;
    const glm::vec3 run(7.0f * dt, 0.0f, 0.0f);
    const BodyMove settled = fallBody(glm::vec3(3.5f, 0.0f, 0.3f), glm::vec3(0.0f, 0.0f, -1.5f), kRadius, kHeight, mesh);
    REQUIRE(settled.support == BodySupport::Walkable);
    Walker walker{.position = settled.position, .baseZ = settled.position.z};
    for (int i = 0; i < 30; ++i) walker.frame(mesh, run, dt);
    CHECK(walker.position.x > 4.356f - 7.0f * dt);
    CHECK(walker.position.x < 4.36f);
    CHECK(walker.position.z < 0.01f);
}

TEST_CASE("A move turned along a wall into a leaning ridge stops in the corner", "[body_mover]") {
    // Orgrimmar (1980.39, -4804.32), WoWee's own triangles: two floors, an
    // overhanging wall (two triangles) and the pillar beyond it, whose ridge
    // leans out towards the walker (two faces). Sliding west along the wall
    // the body's side meets the ridge; the ridge's face the turned move runs
    // into leads back into the wall, and native stands in the corner at
    // (1980.388, -4804.323) for the rest of the run. Only the lifted body
    // meets the ridge within the move; turned by the direction separating
    // body and ridge, the walker slipped out sideways and went on round the
    // pillar to (1978.2, -4802.9).
    const Mesh mesh = {
        {{{1983.89673f, -4794.22949f, 55.8202591f}, {1980.28589f, -4803.43115f, 55.7883606f}, {1984.03699f, -4804.20898f, 55.8202667f}}},
        {{{1980.15173f, -4804.46533f, 58.8214149f}, {1979.86194f, -4804.91602f, 56.7403908f}, {1977.52637f, -4802.77637f, 58.8214149f}}},
        {{{1977.23657f, -4803.22705f, 56.7403908f}, {1977.52637f, -4802.77637f, 58.8214149f}, {1979.86194f, -4804.91602f, 56.7403908f}}},
        {{{1980.28589f, -4803.43115f, 56.2605858f}, {1976.22595f, -4802.19824f, 56.2924881f}, {1979.81323f, -4804.62891f, 56.2924881f}}},
        {{{1982.90002f, -4805.54297f, 56.7711754f}, {1979.43823f, -4804.83594f, 56.7711754f}, {1979.52087f, -4804.37891f, 59.0325546f}}},
        {{{1979.52087f, -4804.37891f, 59.0325546f}, {1982.98267f, -4805.08594f, 59.0325546f}, {1982.90002f, -4805.54297f, 56.7711754f}}},
        {{{1984.03699f, -4804.20898f, 56.2924881f}, {1979.81323f, -4804.62891f, 56.2924881f}, {1984.05762f, -4805.50146f, 56.2924881f}}},
        {{{1979.81323f, -4804.62891f, 56.2924881f}, {1984.03699f, -4804.20898f, 56.2924881f}, {1980.28589f, -4803.43115f, 56.2605858f}}},
        {{{1979.86194f, -4804.91602f, 56.7403908f}, {1980.15173f, -4804.46533f, 58.8214149f}, {1979.43262f, -4805.58301f, 56.7403908f}}},
    };
    const float dt = 1.0f / 30.0f;
    const glm::vec3 run(std::cos(-2.431f) * 7.0f * dt, std::sin(-2.431f) * 7.0f * dt, 0.0f);
    const BodyMove settled = fallBody(glm::vec3(1983.29f, -4803.32f, 56.1f), glm::vec3(0.0f, 0.0f, -1.5f),
                                      kRadius, kHeight, mesh);
    REQUIRE(settled.support == BodySupport::Walkable);
    Walker walker{.position = settled.position, .baseZ = settled.position.z};
    for (int i = 0; i < 60; ++i) walker.frame(mesh, run, dt);
    CHECK(glm::distance(glm::vec2(walker.position), glm::vec2(1980.388f, -4804.323f)) < 0.15f);
}

TEST_CASE("The nose of a step does not turn aside a move that a wall beyond it stops", "[body_mover]") {
    // Stormwind (-8774.14, 832.54), WoWee's own triangles: a ramp, the kerb
    // at the foot of a wall with the strip of ground on top of it, and the
    // wall. Head-on, the kerb's nose comes under the bevel 0.08 yd on and
    // the wall is met 0.23 yd on, lifted or not. Native takes its move's
    // part along the wall's plane and creeps up the ramp, (-0.0049, -0.0026,
    // +0.0021) a frame for 70 frames; turned along the kerb's riser the
    // walker crept the other way, down the ramp.
    const Mesh mesh = {
        {{{-8782.77051f, 825.299805f, 97.6522751f}, {-8764.28125f, 839.539429f, 88.8507843f}, {-8789.23145f, 833.422852f, 97.6522751f}}},
        {{{-8776.94922f, 829.864441f, 95.1943359f}, {-8773.17285f, 832.868713f, 93.4031448f}, {-8776.03711f, 830.655701f, 94.3608398f}}},
        {{{-8776.94922f, 829.864441f, 95.1943359f}, {-8772.97656f, 832.62262f, 93.4264603f}, {-8773.17285f, 832.868713f, 93.4031448f}}},
        {{{-8776.72656f, 829.639771f, 97.0967255f}, {-8772.97656f, 832.62262f, 97.0967255f}, {-8772.97656f, 832.62262f, 93.4264603f}}},
    };
    const float dt = 1.0f / 30.0f;
    const glm::vec3 run(std::cos(-0.9229f) * 7.0f * dt, std::sin(-0.9229f) * 7.0f * dt, 0.0f);
    const BodyMove settled = fallBody(glm::vec3(-8774.1426f, 832.5356f, 93.7f), glm::vec3(0.0f, 0.0f, -1.5f),
                                      kRadius, kHeight, mesh);
    REQUIRE(settled.support == BodySupport::Walkable);
    Walker walker{.position = settled.position, .baseZ = settled.position.z};
    for (int i = 0; i < 30; ++i) walker.frame(mesh, run, dt);
    CHECK(walker.position.x < settled.position.x - 0.1f);
    CHECK(walker.position.y < settled.position.y - 0.05f);
    CHECK(walker.position.z > settled.position.z + 0.04f);
}

TEST_CASE("A walker on the nose of a step is turned by the box beyond it and gets onto the stairs", "[body_mover]") {
    // Orgrimmar (1976.75, -4802.29), WoWee's own triangles: a floor, a step
    // up to the next floor, a box standing on the upper floor 0.20 yd beyond
    // the step's nose, and the stairs behind it. Native, hung by its bevel
    // on the nose, creeps (-0.0092, +0.0137) a frame along the box's face,
    // steps onto the upper floor once clear and walks up the stairs to
    // (1967.72, -4808.08). Turned along the step's riser the walker crept
    // the other way and stood at (1976.4, -4801.8) for good.
    const Mesh mesh = {
        {{{1983.89673f, -4794.22949f, 55.8202591f}, {1977.11792f, -4801.27734f, 55.7883606f}, {1980.28589f, -4803.43115f, 55.7883606f}}},
        {{{1983.89673f, -4794.22949f, 55.8202591f}, {1980.28589f, -4803.43115f, 55.7883606f}, {1984.03699f, -4804.20898f, 55.8202667f}}},
        {{{1976.22595f, -4802.19824f, 56.2924881f}, {1973.84985f, -4798.59814f, 56.7647095f}, {1976.22595f, -4802.19824f, 56.7647095f}}},
        {{{1977.11792f, -4801.27734f, 56.2605858f}, {1973.84985f, -4798.59814f, 56.2924881f}, {1976.22595f, -4802.19824f, 56.2924881f}}},
        {{{1977.23657f, -4803.22705f, 56.7403908f}, {1977.52637f, -4802.77637f, 58.8214149f}, {1979.86194f, -4804.91602f, 56.7403908f}}},
        {{{1976.22595f, -4802.19824f, 56.2924881f}, {1980.28589f, -4803.43115f, 56.2605858f}, {1977.11792f, -4801.27734f, 56.2605858f}}},
        {{{1976.22595f, -4802.19824f, 56.7647095f}, {1979.81323f, -4804.62891f, 56.2924881f}, {1976.22595f, -4802.19824f, 56.2924881f}}},
        {{{1970.94885f, -4808.07129f, 56.764698f}, {1979.81323f, -4804.62891f, 56.7647095f}, {1976.22595f, -4802.19824f, 56.7647095f}}},
        {{{1975.88855f, -4802.46777f, 57.5946693f}, {1976.47571f, -4802.96436f, 57.2474594f}, {1975.98193f, -4802.30908f, 57.2474747f}}},
        {{{1976.43689f, -4803.73633f, 56.9822273f}, {1975.87903f, -4803.44434f, 58.5333977f}, {1976.35474f, -4803.65918f, 57.7205544f}}},
        {{{1976.58374f, -4802.81201f, 57.0882187f}, {1976.42407f, -4802.98877f, 57.7317085f}, {1976.52466f, -4802.98389f, 57.0882187f}}},
        {{{1976.0509f, -4803.12061f, 58.4731941f}, {1976.42407f, -4802.98877f, 57.7317085f}, {1976.58374f, -4802.81201f, 57.0882187f}}},
    };
    const float dt = 1.0f / 30.0f;
    const glm::vec3 run(std::cos(-2.568f) * 7.0f * dt, std::sin(-2.568f) * 7.0f * dt, 0.0f);
    const BodyMove settled = fallBody(glm::vec3(1982.4f, -4799.42f, 56.1f), glm::vec3(0.0f, 0.0f, -1.5f),
                                      kRadius, kHeight, mesh);
    REQUIRE(settled.support == BodySupport::Walkable);
    Walker walker{.position = settled.position, .baseZ = settled.position.z};
    for (int i = 0; i < 66; ++i) walker.frame(mesh, run, dt);
    CHECK(walker.position.x < 1974.5f);
    CHECK(walker.position.z > 56.7f);
}

TEST_CASE("A ceiling the lifted body comes up against does not turn the move aside", "[body_mover]") {
    // Valley of Trials (-520.4, -4254.4), WoWee's own triangles: two terrain
    // faces to stand on (47 and 45 degrees), the 75 degree terrain face
    // ahead, and the underside of a rock that hangs over them, a yard above
    // the walker's head. Native slides (+0.0353, -0.0063) a frame along the
    // steep terrain face for as long as the key is held, from (-520.64,
    // -4254.47) past (-519.2, -4254.5) in four runs. Turned every other
    // frame by the rock's underside (45 degrees from level), which only the
    // lifted body meets, the walker went back and forth around (-520.4,
    // -4254.43).
    const Mesh mesh = {
        upFacing({-518.75f, -4256.25f, 48.9323769f}, {-520.833313f, -4254.1665f, 49.5732269f}, {-520.833313f, -4258.33301f, 45.8596802f}),
        upFacing({-518.75f, -4256.25f, 48.9323769f}, {-516.666626f, -4254.1665f, 51.7840157f}, {-520.833313f, -4254.1665f, 49.5732269f}),
        upFacing({-518.75f, -4252.08301f, 58.2001762f}, {-520.833313f, -4254.1665f, 49.5732269f}, {-516.666626f, -4254.1665f, 51.7840157f}),
        {{{-518.098511f, -4256.19141f, 53.6731033f}, {-522.537903f, -4254.6333f, 54.0844193f}, {-519.735535f, -4252.41797f, 50.8644028f}}},
    };
    const float dt = 1.0f / 30.0f;
    const glm::vec3 run(std::cos(1.269f) * 7.0f * dt, std::sin(1.269f) * 7.0f * dt, 0.0f);
    const BodyMove settled = fallBody(glm::vec3(-520.6385f, -4254.4678f, 49.72f), glm::vec3(0.0f, 0.0f, -1.5f),
                                      kRadius, kHeight, mesh);
    REQUIRE(settled.support == BodySupport::Walkable);
    Walker walker{.position = settled.position, .baseZ = settled.position.z};
    float back = 0.0f;
    for (int i = 0; i < 30; ++i) {
        const float before = walker.position.x;
        walker.frame(mesh, run, dt);
        back = std::max(back, before - walker.position.x);
    }
    CHECK(back < 0.005f);
    CHECK(walker.position.x > -519.7f);
    CHECK(std::abs(walker.position.y + 4254.63f) < 0.1f);
}

TEST_CASE("Ground rising beside a steep face is walked up only where the body's box clears the face", "[body_mover]") {
    // Valley of Trials (-521.9, -4255.5), WoWee's own terrain triangles: a
    // 42 degree face to stand on and the 52 degree face it meets along a
    // line ahead and to the left. Native at 33 ms frames slides (+0.1021,
    // +0.0156) along the steep face from (-521.941, -4255.646), the line
    // 0.565 yd ahead of its sole, and takes its whole move (+0.0687,
    // +0.2207) from (-521.839, -4255.630), the line 0.696 yd ahead; set down
    // at the move's end the body rests clear of the steep face from both.
    const Mesh mesh = {
        upFacing({-522.916626f, -4256.25f, 47.3643684f}, {-520.833313f, -4258.33301f, 45.8596802f}, {-520.833313f, -4254.1665f, 49.5732269f}),
        upFacing({-522.916626f, -4256.25f, 47.3643684f}, {-520.833313f, -4254.1665f, 49.5732269f}, {-525.0f, -4254.1665f, 50.4329987f}),
    };
    const float dt = 0.033f;
    const glm::vec3 run(std::cos(1.269f) * 7.0f * dt, std::sin(1.269f) * 7.0f * dt, 0.0f);
    SECTION("the steep face within the box's reach: the move is turned along it") {
        const auto rest = lowerBody(glm::vec3(-521.941f, -4255.646f, 48.2f), 1.0f, kRadius, kHeight, mesh);
        REQUIRE(rest);
        const BodyMove move = walkBody(rest->position, run, kMaxStepUp, kRadius, kHeight, mesh,
                                       kMinWalkableNormalZ, kMaxStepUp, mesh.size());
        CHECK(move.support == BodySupport::Walkable);
        CHECK(move.position.x - rest->position.x == Catch::Approx(0.1021f).margin(0.003f));
        CHECK(move.position.y - rest->position.y == Catch::Approx(0.0156f).margin(0.003f));
    }
    SECTION("the steep face beyond the box's reach: the whole move is taken") {
        const auto rest = lowerBody(glm::vec3(-521.839f, -4255.630f, 48.2f), 1.0f, kRadius, kHeight, mesh);
        REQUIRE(rest);
        const BodyMove move = walkBody(rest->position, run, kMaxStepUp, kRadius, kHeight, mesh,
                                       kMinWalkableNormalZ, kMaxStepUp, mesh.size());
        CHECK(move.support == BodySupport::Walkable);
        CHECK(move.position.x - rest->position.x == Catch::Approx(0.0687f).margin(0.003f));
        CHECK(move.position.y - rest->position.y == Catch::Approx(0.2207f).margin(0.003f));
    }
}

TEST_CASE("Of the steep faces around a corner the flattest turns the move first", "[body_mover]") {
    // Eastern Plaguelands gully (2679.4, -4975.1), WoWee's own terrain
    // triangles around one vertex: floors of 23 and 35 degrees, faces of 57,
    // 63, 67, 68 and 75 degrees. Native walking west into it slides along the
    // 57 degree face, creeps back along the foot of the 68 degree one, hops
    // onto the floor along the 75 degree one and comes round again: 7
    // reversals of height in 3 s, ending at (2679.434, -4975.101). Turned by
    // the face met first the walker took the hop every other frame: 43 to 50
    // reversals.
    const Mesh mesh = {
        upFacing({2677.08325f, -4977.08301f, 103.9505f}, {2679.1665f, -4979.1665f, 101.874603f}, {2679.1665f, -4975.0f, 99.8784561f}),
        upFacing({2677.08325f, -4977.08301f, 103.9505f}, {2679.1665f, -4975.0f, 99.8784561f}, {2675.0f, -4975.0f, 108.133789f}),
        upFacing({2677.08325f, -4972.9165f, 106.778526f}, {2679.1665f, -4975.0f, 99.8784561f}, {2679.1665f, -4970.83301f, 109.388268f}),
        upFacing({2677.08325f, -4972.9165f, 106.778526f}, {2675.0f, -4975.0f, 108.133789f}, {2679.1665f, -4975.0f, 99.8784561f}),
        upFacing({2681.25f, -4977.08301f, 99.788559f}, {2679.1665f, -4975.0f, 99.8784561f}, {2679.1665f, -4979.1665f, 101.874603f}),
        upFacing({2681.25f, -4977.08301f, 99.788559f}, {2683.33325f, -4975.0f, 98.5081635f}, {2679.1665f, -4975.0f, 99.8784561f}),
        upFacing({2681.25f, -4972.9165f, 98.6271591f}, {2679.1665f, -4975.0f, 99.8784561f}, {2683.33325f, -4975.0f, 98.5081635f}),
        upFacing({2681.25f, -4972.9165f, 98.6271591f}, {2679.1665f, -4970.83301f, 109.388268f}, {2679.1665f, -4975.0f, 99.8784561f}),
    };
    const float dt = 1.0f / 30.0f;
    const glm::vec3 run(std::cos(-3.04f) * 7.0f * dt, std::sin(-3.04f) * 7.0f * dt, 0.0f);
    const BodyMove settled = fallBody(glm::vec3(2681.6f, -4975.35f, 100.0f), glm::vec3(0.0f, 0.0f, -1.5f),
                                      kRadius, kHeight, mesh);
    REQUIRE(settled.support == BodySupport::Walkable);
    Walker walker{.position = settled.position, .baseZ = settled.position.z, .terrain = mesh.size()};
    int reversals = 0;
    float last = 0.0f;
    for (int i = 0; i < 90; ++i) {
        const float before = walker.position.z;
        walker.frame(mesh, run, dt);
        const float dz = walker.position.z - before;
        if (std::abs(dz) > 0.005f) {
            if (last * dz < 0.0f) ++reversals;
            last = dz;
        }
    }
    CHECK(reversals <= 10);
    CHECK(glm::distance(glm::vec2(walker.position), glm::vec2(2679.434f, -4975.101f)) < 0.1f);
}

TEST_CASE("A steep face of the terrain the sole would meet stops the walk where the walk does not lead over it", "[body_mover]") {
    // A 40 degree ramp, a 54 degree bank across it, and ground again behind
    // the bank - or the bank going on up. Native slides along a terrain face
    // of that slope that goes on up (Valley of Trials, Tanaris) from where
    // its plane is less than the move less 0.1 yd ahead of the sole, and
    // walks onto such a bank of a building (Karabor Sewers ramp, 341.8,
    // 815.1) and over it, a third of a yard wide, with whole moves.
    const float rampTop = std::tan(glm::radians(40.0f));
    Mesh low, tall;
    for (Mesh* mesh : {&low, &tall}) {
        floorAt(*mesh, 0.0f, -20.0f, 0.0f);
        slope(*mesh, 0.0f, 0.0f, 40.0f, rampTop);
    }
    const float bankRise = 0.2f * std::tan(glm::radians(54.0f));
    slope(low, 1.0f, rampTop, 54.0f, bankRise);
    slope(low, 1.2f, rampTop + bankRise, 20.0f, 3.0f);
    slope(tall, 1.0f, rampTop, 54.0f, 4.0f);
    const glm::vec3 run(kFrameRun, 0.0f, 0.0f);
    SECTION("a bank with ground behind it is walked over, terrain or building") {
        for (const size_t terrain : {size_t{0}, low.size()}) {
            Walker walker{.position = {-0.5f, 0.0f, 0.0f}, .terrain = terrain};
            for (int i = 0; i < 12; ++i) walker.frame(low, run);
            CHECK(walker.position.x > 2.0f);
        }
    }
    SECTION("a terrain face that goes on up is not walked up to") {
        Walker walker{.position = {-0.5f, 0.0f, 0.0f}, .terrain = tall.size()};
        for (int i = 0; i < 12; ++i) walker.frame(tall, run);
        // The plane is level with the sole (1 - x) (1 - tan 40 / tan 54) yd ahead on the ramp.
        const float ahead = (1.0f - walker.position.x) * (1.0f - rampTop / std::tan(glm::radians(54.0f)));
        CHECK(walker.position.x < 1.0f);
        CHECK(ahead < kFrameRun - 0.1f + 1e-3f);
        CHECK(ahead > kFrameRun - 0.1f - kFrameRun * (1.0f - rampTop / std::tan(glm::radians(54.0f))) - 1e-3f);
    }
}

TEST_CASE("The near edge of a top a hair over a yard up is got over when the top falls away behind it", "[body_mover]") {
    // Astranaar (2770.2, -428.2): a box whose top is 1.006 yd above the floor
    // at its near edge and slopes down behind it. Native hangs on the edge by
    // its bevel for two frames (+0.6514, +0.3301) and walks on over the top
    // with its whole move; its sole, lifted to the yard, passes under the
    // edge for 0.04 yd of the move. The user's client stood in front of it.
    Mesh mesh;
    floorAt(mesh, 0.0f, -20.0f, 1.0f);
    quad(mesh, {1.0f, -20.0f, 0.0f}, {1.0f, 20.0f, 0.0f}, {1.0f, 20.0f, 1.006f}, {1.0f, -20.0f, 1.006f}, {-1.0f, 0.0f, 0.0f});
    quad(mesh, {1.0f, -20.0f, 1.006f}, {5.0f, -20.0f, 0.406f}, {5.0f, 20.0f, 0.406f}, {1.0f, 20.0f, 1.006f});
    const glm::vec3 run(kFrameRun, 0.0f, 0.0f);
    Walker walker{.position = {-0.5f, 0.0f, 0.0f}};
    for (int i = 0; i < 14; ++i) walker.frame(mesh, run);
    CHECK(walker.position.x > 2.0f);
    CHECK(walker.grounded);
    CHECK(walker.position.z == Catch::Approx(1.006f - 0.15f * (walker.position.x - 1.0f)).margin(0.01f));
}

TEST_CASE("A body walked off its ground onto a building's slope steps on up another steep face", "[body_mover]") {
    // Karabor Sewers (347.2, 814.4), about the vertex its faces share: a 21
    // degree ramp, ground of 43 degrees, a 54 degree bank, faces of 61 and 71
    // degrees and ground of 40 degrees above them. Native, heading 23.6
    // degrees, creeps along the foot of the bank until its sole is 1.1 mm or
    // more beyond the ground's edge over the bank, then takes its whole move
    // 0.21 to 0.40 yd up onto the 61 degree face (20 frames of 15 runs) and
    // falls from there, on over the face and up the ramp beyond in most runs.
    // Turned along the bank instead, the user's client went back down to the
    // ramp and round again, 3 to 6 yd behind native after 3 s.
    Mesh mesh;
    mesh.push_back({glm::vec3(6.7100f, -1.2233f, 2.5427f), glm::vec3(0.0002f, -0.0002f, 0.0002f), glm::vec3(-5.8321f, 0.0276f, -2.1989f)});
    mesh.push_back({glm::vec3(-5.8392f, -1.1474f, -2.2054f), glm::vec3(6.7100f, -1.2233f, 2.5427f), glm::vec3(-5.8321f, 0.0276f, -2.1989f)});
    mesh.push_back({glm::vec3(6.8740f, -0.0330f, 2.5918f), glm::vec3(0.0002f, -0.0002f, 0.0002f), glm::vec3(6.7100f, -1.2233f, 2.5427f)});
    mesh.push_back({glm::vec3(0.0002f, -0.0002f, 0.0002f), glm::vec3(6.8740f, -0.0330f, 2.5918f), glm::vec3(0.3541f, 1.1702f, 2.1854f)});   // 61 degrees
    mesh.push_back({glm::vec3(-0.0908f, 1.1702f, 0.9725f), glm::vec3(-0.7883f, 1.1732f, 1.1582f), glm::vec3(0.0002f, -0.0002f, 0.0002f)}); // 40 degrees
    mesh.push_back({glm::vec3(0.3541f, 1.1702f, 2.1854f), glm::vec3(-0.0908f, 1.1702f, 0.9725f), glm::vec3(0.0002f, -0.0002f, 0.0002f)});  // 71 degrees
    mesh.push_back({glm::vec3(-5.8321f, 0.0276f, -2.1989f), glm::vec3(0.0002f, -0.0002f, 0.0002f), glm::vec3(-6.1592f, 1.1913f, -1.3156f)}); // 43 degrees
    mesh.push_back({glm::vec3(-0.7883f, 1.1732f, 1.1582f), glm::vec3(-6.1592f, 1.1913f, -1.3156f), glm::vec3(0.0002f, -0.0002f, 0.0002f)}); // the bank
    const glm::vec3 heading(std::cos(0.412f), std::sin(0.412f), 0.0f);
    const glm::vec3 bank = glm::normalize(glm::cross(mesh[7][1] - mesh[7][0], mesh[7][2] - mesh[7][0]));

    SECTION("the step itself, from 2 mm beyond the ground's edge") {
        // Native's frame 19 of run u17_24: from (-0.3533, +0.0699, -0.0722) by (+0.2117, +0.0925, +0.3894).
        const glm::vec3 from(-0.3533f, 0.0699f, -0.0722f);
        const BodyMove step = walkBody(from, heading * 0.2310f, kMaxStepUp, kRadius, kHeight, mesh,
                                       kMinWalkableNormalZ, kMaxStepUp, 0, bank);
        CHECK(step.position.x - from.x == Catch::Approx(0.2117f).margin(0.002f));
        CHECK(step.position.y - from.y == Catch::Approx(0.0925f).margin(0.002f));
        CHECK(step.position.z - from.z == Catch::Approx(0.3894f).margin(0.005f));
        CHECK(step.support == BodySupport::Steep);
        // Not walked onto the bank - standing on ground - it is turned aside instead.
        const BodyMove turned = walkBody(from, heading * 0.2310f, kMaxStepUp, kRadius, kHeight, mesh,
                                         kMinWalkableNormalZ, kMaxStepUp, 0);
        CHECK(turned.position.z - from.z < 0.15f);
        // The same faces as terrain are not stepped onto.
        const BodyMove terrain = walkBody(from, heading * 0.2310f, kMaxStepUp, kRadius, kHeight, mesh,
                                          kMinWalkableNormalZ, kMaxStepUp, mesh.size(), bank);
        CHECK(terrain.position.z - from.z < 0.15f);
    }
    SECTION("up the ramp, past the bank, within native's time") {
        // After 3 s native is 10.8 to 15.9 yd on (8 arrival phases), 5 to 10 yd past the vertex.
        for (const float back : {0.0f, 0.07f, 0.14f}) {
            Walker walker{.position = glm::vec3(-3.5f, -1.35f, -1.32f) - heading * back};
            walker.position.z = lowerBody(walker.position + glm::vec3(0, 0, 0.5f), 2.0f, kRadius, kHeight, mesh)->position.z;
            walker.baseZ = walker.position.z;
            float rise = 0.0f;
            for (int i = 0; i < 90; ++i) {
                const float before = walker.position.z;
                walker.frame(mesh, heading * kFrameRun);
                rise = std::max(rise, walker.position.z - before);
            }
            INFO("start " << back << " yd back, at x " << walker.position.x);
            CHECK(rise > 0.2f);
            CHECK(walker.position.x > 1.0f);
        }
    }
}

TEST_CASE("What a steep face pushes a falling body aside by is not kept as level velocity", "[body_mover]") {
    // Karabor Sewers (347.0, 814.5): native sliding down a 71 degree face
    // moves out from it by the frame's drop over the face's slope - 0.0112,
    // 0.0175 and 0.0265 yd for drops of 0.0328, 0.0512 and 0.0774 - square to
    // the face every frame, with no level velocity gathered from those pushes.
    Mesh mesh;
    slope(mesh, 0.0f, 0.0f, 71.0f, 6.0f);
    // The body resting against the face, a yard up, falling with no level move of its own.
    const float foot = 1.0f / std::tan(glm::radians(71.0f));
    const glm::vec3 from(foot - kRadius - 0.01f, 0.0f, 1.0f - kRadius * kBodyBevel);
    const BodyMove fall = fallBody(from, glm::vec3(0.0f, 0.0f, -0.0512f), kRadius, kHeight, mesh);
    CHECK(fall.deflected);
    CHECK(fall.position.z - from.z == Catch::Approx(-0.0512f).margin(0.002f));
    CHECK(fall.position.x - from.x < -0.005f);
    CHECK(glm::length(fall.levelKept) < 1e-6f);
    // A level move along the face is kept whole, one into it not at all.
    const BodyMove along = fallBody(from, glm::vec3(0.0f, 0.1f, -0.0512f), kRadius, kHeight, mesh);
    CHECK(along.levelKept.y == Catch::Approx(0.1f).margin(1e-4f));
    CHECK(std::abs(along.levelKept.x) < 1e-4f);
    const BodyMove into = fallBody(from, glm::vec3(0.1f, 0.0f, -0.0512f), kRadius, kHeight, mesh);
    CHECK(glm::length(into.levelKept) < 1e-4f);
}

TEST_CASE("Ground the lifted body is carried into takes it on up past the yard", "[body_mover]") {
    // Karabor Sewers (337.3, 802.4), about the pose a walker up the ramp
    // touches the riser at: a 32 degree ramp, a 77 degree riser 0.68 yd high
    // whose top edge rises 25 degrees along its length, ground of 43 degrees
    // behind that edge and a 53 degree face beyond. Native, heading -87.1
    // degrees, goes up in three frames - onto the riser's top edge by its
    // underside twice, then onto the ground - and ends the third 1.006 to
    // 1.019 yd above the ramp its sole last stood on (11 runs).
    Mesh mesh;
    mesh.push_back({glm::vec3(3.6440f, 1.0956f, 0.8979f), glm::vec3(-9.5535f, 1.1755f, -4.3347f), glm::vec3(1.6184f, -0.3657f, 0.8158f)});   // ramp
    mesh.push_back({glm::vec3(1.6184f, -0.3657f, 0.8158f), glm::vec3(-9.5535f, 1.1755f, -4.3347f), glm::vec3(-9.6337f, -0.3010f, -3.6143f)}); // ramp
    mesh.push_back({glm::vec3(1.6184f, -0.3657f, 0.8158f), glm::vec3(-9.6337f, -0.3010f, -3.6143f), glm::vec3(1.4298f, -0.5485f, 1.5208f)});  // riser
    mesh.push_back({glm::vec3(-9.6337f, -0.3010f, -3.6143f), glm::vec3(-2.5484f, -2.1289f, 1.0161f), glm::vec3(1.4298f, -0.5485f, 1.5208f)}); // 43 degrees
    mesh.push_back({glm::vec3(1.4298f, -0.5485f, 1.5208f), glm::vec3(-2.5484f, -2.1289f, 1.0161f), glm::vec3(1.7863f, -0.8756f, 2.1331f)});   // 53 degrees
    const glm::vec3 heading(std::cos(-1.5202f), std::sin(-1.5202f), 0.0f);

    SECTION("native's three frames of run p19_u28a_6") {
        // From (+0.0010, +0.0056, -0.0019): +0.4521, +0.3317 and, 0.238 yd long, +0.2338.
        Walker walker{.position = glm::vec3(0.0010f, 0.0056f, 0.05f)};
        walker.position.z = lowerBody(walker.position, 0.2f, kRadius, kHeight, mesh)->position.z;
        walker.baseZ = walker.position.z;
        const float base = walker.position.z;
        walker.frame(mesh, heading * 0.2310f, 0.033f);
        CHECK(walker.position.z - base == Catch::Approx(0.4521f).margin(0.003f));
        walker.frame(mesh, heading * 0.2310f, 0.033f);
        CHECK(walker.position.z - base == Catch::Approx(0.7838f).margin(0.003f));
        walker.frame(mesh, heading * 0.2380f, 0.034f);
        CHECK(walker.position.z - base == Catch::Approx(1.0176f).margin(0.003f));
        CHECK(walker.grounded);
        CHECK(-walker.position.y == Catch::Approx(0.6938f).margin(0.002f));
    }
    SECTION("a walker up the ramp gets over the step at any frame length") {
        // Native 3 s on from (-0.67, 13.55): 15.5 to 19.6 yd, over the step in 40 runs of 40.
        for (const float dt : {0.031f, 0.033f, 1.0f / 30.0f, 0.0335f, 0.034f, 0.035f, 0.036f}) {
            Walker walker{.position = glm::vec3(-0.45f, 9.0f, -1.2f)};
            walker.position.z = lowerBody(walker.position, 2.0f, kRadius, kHeight, mesh)->position.z;
            walker.baseZ = walker.position.z;
            for (int i = 0; i < 60; ++i) walker.frame(mesh, heading * (7.0f * dt), dt);
            INFO("frame of " << dt << " s, at y " << walker.position.y << " z " << walker.position.z);
            CHECK(walker.position.y < -1.0f);
        }
    }
}

TEST_CASE("A step over a wall the box meets lifts by the move over the radius and falls back from the nose", "[body_mover]") {
    // Dun Morogh (-5164.4, 664.7), WoWee's own triangles moved to the
    // origin: a level floor, an upright riser 0.654 yd high along a line
    // 4.5 degrees off the x axis, a level top. Native heading 78.7 degrees.
    const glm::vec2 along(std::cos(glm::radians(-4.5f)), std::sin(glm::radians(-4.5f)));
    const glm::vec2 beyond(-along.y, along.x);
    const float top = 0.654f;
    const auto at = [&](float a, float b, float z) { return glm::vec3(along * a + beyond * b, z); };
    const Mesh mesh = {
        {{{-30.0f, -30.0f, 0.0f}, {30.0f, -30.0f, 0.0f}, {30.0f, 30.0f, 0.0f}}},
        {{{-30.0f, -30.0f, 0.0f}, {30.0f, 30.0f, 0.0f}, {-30.0f, 30.0f, 0.0f}}},
        {{at(-10.0f, 0.0f, 0.0f), at(10.0f, 0.0f, 0.0f), at(-10.0f, 0.0f, top)}},
        {{at(10.0f, 0.0f, 0.0f), at(10.0f, 0.0f, top), at(-10.0f, 0.0f, top)}},
        {{at(-10.0f, 0.0f, top), at(10.0f, 0.0f, top), at(-10.0f, 20.0f, top)}},
        {{at(10.0f, 0.0f, top), at(10.0f, 20.0f, top), at(-10.0f, 20.0f, top)}},
    };
    const glm::vec3 heading(std::cos(glm::radians(78.7f)), std::sin(glm::radians(78.7f)), 0.0f);

    SECTION("at run speed: up by 1.4233 times the move, back down the nose, up again") {
        // Native (4 runs at 33 to 35 ms): +0.3288 for a move of 0.2310; a
        // first frame of fall; then (-0.0174, -0.0174, -0.0322), (-0.0288,
        // -0.0288, -0.0532), (-0.0415, -0.0415, -0.0767) - the drop over the
        // bevel on either axis; on the top eight frames after the first step.
        const float dt = 0.033f;
        Walker walker{.position = glm::vec3(0.0f, -0.45f, 0.0f)};
        const BodyMove up = walker.frame(mesh, heading * (7.0f * dt), dt);
        REQUIRE(up.support == BodySupport::None);
        CHECK(walker.position.z == Catch::Approx(7.0f * dt * (1.0f / kRadius - kBodyBevel)).margin(1e-3f));
        walker.frame(mesh, heading * (7.0f * dt), dt);
        // Besides the drop's push the body goes on with what the nose left
        // of its level move, along the nose (native does so in the first
        // frame of the fall only).
        const glm::vec3 kept = walker.airMove;
        CHECK(glm::length(kept) < 0.03f);
        for (int fall = 0; fall < 3; ++fall) {
            const glm::vec3 before = walker.position;
            walker.frame(mesh, heading * (7.0f * dt), dt);
            const glm::vec3 moved = walker.position - before - kept;
            INFO("fall frame " << fall + 2 << ": " << moved.x << " " << moved.y << " " << moved.z);
            REQUIRE(moved.z < -0.02f);
            CHECK(moved.x == Catch::Approx(moved.z / kBodyBevel).margin(1.5e-3f));
            CHECK(moved.y == Catch::Approx(moved.z / kBodyBevel).margin(1.5e-3f));
        }
        int frames = 5;
        while (frames < 14 && !(walker.grounded && walker.position.z > top - 0.01f)) {
            walker.frame(mesh, heading * (7.0f * dt), dt);
            ++frames;
        }
        CHECK(frames <= 9);
    }
    SECTION("at a third of that the nose is within the lowering and the body climbs it") {
        // Native (speed 0.3, 34 ms): +0.1084, +0.1176, +0.1233, +0.1170,
        // +0.1195, +0.0685, never in the air.
        const float dt = 0.033f;
        Walker walker{.position = glm::vec3(0.0f, -0.40f, 0.0f)};
        int frames = 0;
        float rose = 0.0f;
        while (frames < 20 && walker.position.z < top - 0.01f) {
            const float before = walker.position.z;
            const BodyMove move = walker.frame(mesh, heading * (2.1f * dt), dt);
            REQUIRE(move.support != BodySupport::None);
            if (walker.position.z > before + 0.01f) rose = std::max(rose, walker.position.z - before);
            ++frames;
        }
        CHECK(frames <= 9);
        // The first frame may rise by the 0.089 yd the nose stands above
        // the box's lower corners besides.
        CHECK(rose < 0.21f);
    }
}

TEST_CASE("A jump pressed against a wall is caught by the bevel on the edge of ground above it and walks on", "[body_mover]") {
    // Undercity, 1552.6, 227.5: a wall 1.76 yd high whose top edge is the
    // foot of a 22.9 degree slope, 0.12 yd above the top of a jump (1.64 yd).
    // Native pressed against it rises along it, comes down with its bevel
    // on that edge in the frame that passes the jump's top, and walks the
    // frame's move from there, onto the slope (26 runs of 27).
    for (const float turn : {0.0f, 0.2755f}) {
        // The wall square to the body's side, and 15.8 degrees off it as there.
        const glm::vec3 into(std::cos(turn), std::sin(turn), 0.0f);
        const glm::vec3 along(-std::sin(turn), std::cos(turn), 0.0f);
        const auto at = [&](float ahead, float aside, float z) { return into * ahead + along * aside + glm::vec3(0, 0, z); };
        Mesh mesh;
        floorAt(mesh, 0.0f);
        quad(mesh, at(3, -20, 0), at(3, -20, 1.76f), at(3, 20, 1.76f), at(3, 20, 0), -into);
        quad(mesh, at(3, -20, 1.76f), at(4.3f, -20, 2.309f), at(4.3f, 20, 2.309f), at(3, 20, 1.76f));
        quad(mesh, at(4.3f, -20, 2.309f), at(20, -20, 2.309f), at(20, 20, 2.309f), at(4.3f, 20, 2.309f));
        const glm::vec3 run = into * kFrameRun;
        Walker walker{.position = at(2.0f, 0.0f, 0.0f)};
        for (int i = 0; i < 20; ++i) walker.frame(mesh, run);
        REQUIRE(walker.grounded);
        REQUIRE(walker.position.z == Catch::Approx(0.0f).margin(0.003));
        walker.grounded = false;
        walker.verticalVelocity = 7.9565f;
        float highestInAir = 0.0f;
        for (int i = 0; i < 40; ++i) {
            walker.frame(mesh, run);
            if (!walker.grounded) highestInAir = std::max(highestInAir, walker.position.z);
        }
        INFO("turn " << turn << " ends at " << walker.position.x << ", " << walker.position.y << ", " << walker.position.z);
        REQUIRE(walker.grounded);
        REQUIRE(walker.position.z == Catch::Approx(2.309f).margin(0.003));
        // The sole never came up to the wall's top in the air.
        REQUIRE(highestInAir < 1.70f);
    }

    // With no ground behind the edge within the frame's move - the wall's top
    // a narrow sill, a wall on it - the body comes back down.
    Mesh sill;
    floorAt(sill, 0.0f);
    quad(sill, {3, -20, 0}, {3, -20, 1.76f}, {3, 20, 1.76f}, {3, 20, 0}, {-1.0f, 0.0f, 0.0f});
    quad(sill, {3, -20, 1.76f}, {3.05f, -20, 1.76f}, {3.05f, 20, 1.76f}, {3, 20, 1.76f});
    quad(sill, {3.05f, -20, 1.76f}, {3.05f, -20, 6}, {3.05f, 20, 6}, {3.05f, 20, 1.76f}, {-1.0f, 0.0f, 0.0f});
    Walker blocked{.position = {2.0f, 0.0f, 0.0f}};
    for (int i = 0; i < 20; ++i) blocked.frame(sill, {kFrameRun, 0.0f, 0.0f});
    blocked.grounded = false;
    blocked.verticalVelocity = 7.9565f;
    for (int i = 0; i < 40; ++i) blocked.frame(sill, {kFrameRun, 0.0f, 0.0f});
    REQUIRE(blocked.grounded);
    REQUIRE(blocked.position.z == Catch::Approx(0.0f).margin(0.003));
}
