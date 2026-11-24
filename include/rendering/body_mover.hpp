#pragma once

#include "rendering/body_collision.hpp"
#include "rendering/movement_limits.hpp"

#include <vector>

// One movement step of the upright beveled body among collision triangles
// (terrain, WMO and doodad alike), after build 8606's walking and falling.
// Derived from native per-frame positions read at 200 Hz, replayed against
// WoWee's own collision triangles for the same places
// (_experiments/20261123T182500Z.movement_collision_parity in the workspace).
//
// Per frame, with the whole frame's horizontal move (native takes it in one
// piece at 16 to 36 ms frames):
//
//  Walking. The move is taken level when nothing is in its way, and the body
//  then lowered onto what is under it, by no more than the move times the
//  slope of its own bevel (walkBody, at its end). Otherwise the
//  body is lifted by what is left of one yard above the last ground it could
//  stand on, carried the whole move, and lowered onto whatever is there. That
//  is a step only when the lifted move meets nothing. Over a wall that stops
//  the body's box a step from ground lifts by the move over the radius and
//  lowers by no more than the move times the bevel, and may leave the body
//  in the air above the edge it would rest on. A step refused, the
//  move is replaced by its part along the refusing face and tried again from
//  the same place (native's slide frames are that projection, with no
//  advance toward the face first).
//
//  Support. A face up to 50 degrees holds the body (ground), under its sole
//  or under its bevel at the face's edge. The yard a step may rise is counted
//  from where the sole last stood. A step may end short of ground under the
//  sole - on a steeper face, where the body is only perched and next frame
//  steps on or falls, or hung by the bevel on an edge. On a slope the sole
//  itself rests on (a face that is not ground, flatter than the underside)
//  the body is not perched: it walks on wherever its move carries it, and
//  falls from it only where it is carried nowhere.
//
//  Falling. Horizontal move and drop are swept together. Ground ends the
//  fall - under the sole, or its edge under the bevel where the sole is
//  within kEdgeCatch of standing over it - and the rest of the frame is
//  walked. Any other face turns the body aside horizontally and leaves the
//  drop alone, so a steep face is slid down at free-fall speed; where an
//  edge crosses an edge of the underside the drop pushes the body off along
//  both sides of the underside that meet there. Wedged between such faces
//  the body is held as on a perch: its fall starts over each frame, and the
//  move the keys ask for is taken from there when it steps on.
//
//  Triangles are solid from their front only (body_collision.hpp).
//
// Measured against this:
//  - Dustwallow plank ramp (-2697.7, -4701.1): 106 native frames - the ramp,
//    a 57.4 degree bump crossed in two frames, a 0.31 yd step down in one;
//    replay within 1 mm.
//  - Orgrimmar wall (1826.75, -4245.98), 84 degrees, and its 77.4 degree
//    section: native slides and never rises at 7 and 3.5 yd/s, 30 and 60
//    frames per second; replay within 8 mm over 116 frames.
//  - Northshire tree hull (faces at 64.6 and 55.3 degrees, 7 yd tall) and
//    rock face terrain (63 degrees): native slides along them and does not
//    step onto them; the tree replay stays within 9 mm for 52 frames.
//  - Exodar table DR_TABLE_MEDIUM_01 (74.6 degree sides, walkable rim about
//    a yard up): native steps onto the sides, landing within 3 mm of the rest
//    height lowerBody() gives in 43 of 47 step frames, and sometimes slides
//    instead from the same pose.
//
// Which steep faces native will step onto is not settled by the face's slope:
// it steps onto 74.6 and 57.4 degrees above and refuses 55.3, 63, 64.6, 77.4
// and 84. The rule used here - a step that does not put the sole on ground is
// taken only when repeating the same move from it does, within the step
// budget - is this client's, not native's: it keeps the body off faces native
// also stays off in the cases first measured, and no more is claimed for it.
//
// What native does, from 144 further paired runs
// (_experiments/20261123T214000Z.movement_collision_parity_rule; the rule that
// covers all of it is not found; the ridge step and the fall from ground's
// edge are built in below, the rest is not):
//  - Flat floor, a 74.5 degree side met by a corner of the body (Exodar
//    table, square on and 30 degrees off, run speed to three times that):
//    never stepped onto, at any gap. Square on the body stands off by whatever
//    the last free frame left, up to 0.40 yd.
//  - The ridge where two such sides meet, met edge-on (the edge against the
//    body's side, not a body corner against a face): the step is taken in that
//    frame, 12 of 12, to the height lowerBody() gives within 3 mm, and the
//    body falls from there unless the next step fits the rest of the yard.
//    Built in: walkBody takes that step when the frame's own move meets
//    an edge (Step::edgeMet).
//  - Orgrimmar wall, 84 degrees, on its 17 degree ramp, head-on: slides when
//    the level free run at the frame's start is 0.287 yd or less (34 runs),
//    is lifted when it is 0.295 or more (24 runs), whatever the frame's move.
//    Lifted by what is left of the yard, carried until the lifted body meets
//    the wall, left at that height: +1.0000 in 19. Held there it neither
//    slides nor falls until the forward key is released. 32 degrees off
//    head-on it slides at every gap up to 1.05 yd (20 runs).
//  - The yard is counted from the last ground: two-frame climbs end exactly a
//    yard above it.
//  - Set by its bevel on the edge of a walkable face after such a step, native
//    falls in the next frame (7 of 7). Built in: such a body is perched,
//    and fallBody ends a fall on ground's edge only when the rest of the
//    move, along the contact, stands the sole on it (the Northshire fence
//    hull: _experiments/20261123T230000Z.movement_collision_parity_landing).
//  - An edge it cannot be raised over within the yard (a leaning trunk's
//    underside at Orgrimmar) stops native dead; WoWee slides along it.
//
// From replaying this mover against the native side of 323 paired runs
// (_experiments/20261123T233500Z.movement_collision_parity_round10, batch.py):
//  - A refused move is turned aside by what stops the body at its own height,
//    when that is not ground - the wall it is sliding along - and not by a
//    face only the body lifted a yard would meet further on (Orgrimmar wall
//    folded along its diagonal, 1821.4, -4243.2: native slides on along the
//    lower face). Built in (Step::wall).
//  - It is turned aside along that face's own plane also where the face is
//    met by its edge: at the Northshire Abbey doorway pillar (-8916.6,
//    -141.3) the body's flat side meets the pillar's upright edge and native
//    moves (-0.050, -0.076) a frame, along the pillar face, not along its
//    own side. Built in (BodyContact::front).
//  - An edge under the body's bevel is mostly not stepped over (97 of 108
//    frames where the step would rest on something not ground, most of them
//    on the skirt of the Orgrimmar wall's 77 degree section); an edge against
//    the body's upright side mostly is (46 of 55, Exodar table). Built in as
//    always and never (Step::edgeMet); what decides the rest is not found.
//  - Not built in: at the foot of that folded wall (1819.8, -4242.5) the edge
//    between its upper faces meets the body's side and native slides on along
//    the face it was sliding along; WoWee steps 0.14 yd up it, twice a run.
//
// Whether a blocked frame first goes up to the wall
// (_experiments/20261124T010000Z.movement_collision_parity_round11, decomp.py):
//  - A native frame stopped by a face is either the whole move turned along
//    the face from where the body stood, or the way to the face and the rest
//    of the move turned along it. Of frames where a corner of the body meets
//    the face 0.03 yd or more on: the level move meets ground before the
//    face (a floor rising ahead: the Orgrimmar ramp, terrain, the Exodar
//    floor's 0.6 degrees) in about 3,400, all but 17 of them without the
//    advance; it meets the face first in 29 (Anvilmar post on its level
//    floor, Northshire steps going down, tree and rock on falling ground),
//    all 29 with it. Built in (walkBody).
//  - Where the face is met by its edge there is no advance: the Northshire
//    Abbey doorway pillar met on the way down the steps (-8916.57, -141.20),
//    0.056 yd on, 10 runs.
//  - On ground rising 2.3 mm a frame along a Northshire tree native advanced
//    part of the way in some frames (0.003 to 0.13 yd of gaps of 0.014 to
//    0.134): as far as a body resting 0.03 to 1.3 mm over the ground gets
//    before the ground meets it. What sets that height is not known; this
//    body rests on its ground and does not advance there.
//
// How far native lifts the body
// (_experiments/20261124T052000Z.movement_collision_parity_round15, liftscan.py,
// feat2.cpp; measured, not built in - this mover lifts by all that is left of
// the yard):
//  - Where the lifted body is stopped by something and native stays up there,
//    it has risen by the frame's move divided by the body's radius, no more
//    than what is left of the yard: 0.5962, 0.6414, 0.6637, 0.7100, 0.7560,
//    0.7789 and 0.8023 yd in frames of 26, 28, 29, 31, 33, 34 and 35 ms at
//    run speed, where move / radius is 0.5956, 0.6415, 0.6644, 0.7102,
//    0.7560, 0.7789 and 0.8018 (23 frames at the Anvilmar post, the
//    Northshire Abbey doorway pillar, the Northshire fence, the Karabor
//    Sewers and the Exodar table), and the rest of the yard in the next
//    frame. With moves longer than the radius
//    (speed 1.5 and up) that is the whole yard at once, as measured before.
//    Which blocked lifts native keeps and which it gives up for a slide is
//    still not known.
//  - Climbing 42 degree terrain beside a 52 degree face (Valley of Trials,
//    -521.9, -4255.5) native takes its whole move forward only when the
//    steep face is further ahead than this mover asks for, and slides along
//    it from further back: at 33 ms frames with the steep face's foot 0.62
//    to 0.70 yd ahead and not at 0.56 to 0.64; at 17 ms from 0.085 yd. Both
//    go along the foot of the steep face in a zigzag; native takes 5 moves
//    forward in 14 frames.
//
// What stops a walk up rising ground
// (_experiments/20261124T071000Z.movement_collision_parity_round17, solescan.py,
// boxscan.cpp, boxmeet.cpp; built in, walkBody):
//  - Ground that rises ahead is walked up - the whole move, the body set down
//    on it - unless a face that is not ground stops the move as the body
//    meets it moving level from where it stands; then the whole move is
//    turned along that face, also where the body set down at the move's end
//    would rest clear of it. A face steeper than the underside is met by a
//    lower corner of the body's box and stops the move when met within it. A
//    flatter one of the terrain is met by the sole and stops the move when
//    the sole, moved level, would be more than 0.1 yd beyond the face's plane
//    (Valley of Trials 0.096 to 0.100, Tanaris 0.098 to 0.151). Such a face
//    of a building does not: native walks onto the 54 degree bank beside a
//    Karabor Sewers ramp from 0.05 to 0.12 yd before its plane (3 frames).
//    Either kind stops the walk only where the walk does not lead over the
//    face to ground again (a step with a 77 degree riser in the Karabor
//    Sewers is walked up to and climbed).
//  - Of several such faces the flattest turns the move first, of equally
//    steep ones the first met (Eastern Plaguelands gully, Exodar pillar
//    corner).
//  - With moves longer than the body's radius (speeds 1.5 to 3 at 33 ms
//    frames) native in the Valley of Trials takes the whole move when the
//    plane is 0.186 yd or more ahead of the sole and slides at 0.159 or
//    less, whatever the move's length. Not built in: this mover takes such
//    moves in equal parts.
//  - Not built in: native steps onto a steep face with its whole move in the
//    Valley of Trials (-520.9, -4254.6, 0.48 yd up a 75 degree face), where
//    this mover turns aside.
//
// A step from a slope
// (_experiments/20261124T094500Z.movement_collision_parity_round18, bank.py,
// feat18.cpp; built in, walkBody):
//  - A walking body whose sole has gone over the edge of its ground onto a
//    slope of a building - a face that is not ground, flatter than the
//    body's underside - takes its next move, lifted by the move over the
//    radius, carried and lowered, as a step onto another steep face, and
//    falls from there (Karabor Sewers 347.2, 814.4: 20 frames of 25 with the
//    sole 1.1 mm or more beyond the edge, none of 42 nearer; Orgrimmar
//    1818.1, -4242.6, where the wall's 77 degree section begins above its
//    53 degree skirt: 15 runs of 18). It does not step up the slope it is
//    on (Orgrimmar skirt, 3 runs) save twice in the sewers, nor from a
//    slope of the terrain (Tanaris, 6 runs).
//  - While falling, what a steep face pushes the body aside by as it drops
//    onto it - the drop over the face's slope, square to the face - is not
//    kept as level velocity (camera_controller.cpp, BodyMove::levelKept).
namespace wowee::rendering::movement {

// What a character's scale (OBJECT_FIELD_SCALE_X times its model's) does to
// its body: nothing below 1, and above 1 radius and step height grow with it.
// Native walking head-on into the Anvilmar post (face along y = 395.17) stops
// at y = 394.9140 at scale 0.7 and at scale 1, 394.7866 at 1.5 and 394.6592
// at 2 - the human radius 0.3056 times 1, 1, 1.5 and 2 - and is lifted at the
// Orgrimmar wall by 1.0000, 1.0000, 1.2500, 1.5000 and 2.0000 yd at scales
// 0.7, 1, 1.25, 1.5 and 2 (_experiments/20261124T003000Z.movement_collision_parity_scale_step).
// The body's height at a scale other than 1 is not measured; it is taken to
// follow the same factor.
inline float bodyScale(float characterScale) {
    assert(characterScale > 0.0f);
    return std::max(1.0f, characterScale);
}

// How far along a move the sole may be beyond the plane of a steep face it
// would meet, or the underside over an edge, without the move being stopped
// (walkBody; measured 0.096 to 0.100 yd in the Valley of Trials).
inline constexpr float kSoleEntry = 0.1f;

// How near the sole of a falling body has to be to standing over ground for
// the bevel, come down on that ground's edge, to catch the body there: the
// level distance from the sole point to the ground's triangle (fallBody;
// native is caught at 0.0788 to 0.0829 yd, 41 frames, and is not at 0.0834
// to 0.0925, 21 frames, nor further).
inline constexpr float kEdgeCatch = 0.0832f;

enum class BodySupport { None, Steep, Walkable };

struct BodyMove {
    glm::vec3 position;
    BodySupport support = BodySupport::None;
    bool soleOnGround = false;  // Walkable, and standing on it by the sole
    bool stepped = false;    // lifted over or onto something
    bool deflected = false;  // the horizontal move was turned aside or stopped
    bool ceiling = false;    // an upward move was stopped
    bool held = false;       // a fall was stopped by faces that are not ground
    // Steep, and the sole itself rests on the face: a slope flatter than the
    // body's underside (walkBody).
    bool soleOnSlope = false;
    // The up-facing normal of a slope - a face of a building that is not
    // ground - a walk begun on ground has carried the sole onto; zero when
    // there is none (walkBody).
    glm::vec3 walkedOntoSlope{0.0f};
    // Of a fall: what the faces that turned it aside leave of the frame's
    // level move - the move without what the drop was pushed aside by.
    glm::vec2 levelKept{0.0f};
};

// `met`, when given, receives the triangle of the contact.
inline std::optional<BodyContact> firstBodyContact(const glm::vec3& from, const glm::vec3& delta,
        float radius, float height, std::span<const BodyTriangle> triangles,
        const BodyTriangle** met = nullptr) {
    const float travel = glm::length(delta);
    std::optional<BodyContact> first;
    for (const auto& triangle : triangles) {
        const auto contact = sweepBodyTriangle(from, delta, radius, height, triangle);
        if (contact && (!first || preferSupportContact(*contact, *first, travel))) {
            first = contact;
            if (met) *met = &triangle;
        }
    }
    return first;
}

// The level distance from a point to a triangle: zero where the point is
// over or under it.
inline float levelDistanceToTriangle(const glm::vec3& point, const BodyTriangle& triangle) {
    const glm::vec2 p(point);
    const std::array<glm::vec2, 3> corner = {glm::vec2(triangle[0]), glm::vec2(triangle[1]), glm::vec2(triangle[2])};
    const auto side = [&](int i) {
        const glm::vec2 edge = corner[(i + 1) % 3] - corner[i];
        return edge.x * (p.y - corner[i].y) - edge.y * (p.x - corner[i].x);
    };
    const float a = side(0), b = side(1), c = side(2);
    if ((a >= 0.0f && b >= 0.0f && c >= 0.0f) || (a <= 0.0f && b <= 0.0f && c <= 0.0f)) return 0.0f;
    float nearest = std::numeric_limits<float>::infinity();
    for (int i = 0; i < 3; ++i) {
        const glm::vec2 edge = corner[(i + 1) % 3] - corner[i];
        const float lengthSq = glm::dot(edge, edge);
        const float along = lengthSq > 1e-12f ? glm::clamp(glm::dot(p - corner[i], edge) / lengthSq, 0.0f, 1.0f) : 0.0f;
        nearest = std::min(nearest, glm::distance(p, corner[i] + edge * along));
    }
    return nearest;
}

struct BodyRest {
    glm::vec3 position;
    glm::vec3 face;    // up-facing normal of the supporting triangle
    glm::vec3 normal;  // separating direction of the contact
    size_t triangle = 0;  // index of the supporting triangle
};

// Lower a body by up to `distance`; where it comes to rest, if on anything.
// Starts two contact skins higher so a body already resting is found resting.
// Among faces met within 2 mm of the first, the flattest is the support: a
// body standing on a floor against a leaning wall touches both.
//
// `fromLifted` says the body was put at `from` to be lowered - lifted for a
// step, or raised to look for the floor it has sunk into - and does not stand
// there. A triangle it overlaps at that height is then not one that carries
// it: a lifted head inside the single-sided floor of the storey above would
// otherwise be "carried" up there.
inline std::optional<BodyRest> lowerBody(const glm::vec3& from, float distance,
        float radius, float height, std::span<const BodyTriangle> triangles,
        bool fromLifted = false) {
    assert(distance > 0.0f);
    constexpr float kSupportTie = 0.002f;
    const glm::vec3 start = from + glm::vec3(0.0f, 0.0f, kContactSkin * 2.0f);
    const float travel = distance + kContactSkin * 2.0f;
    const glm::vec3 delta(0.0f, 0.0f, -travel);
    const auto meet = [&](const BodyTriangle& triangle) {
        auto contact = sweepBodyTriangle(start, delta, radius, height, triangle);
        if (contact && fromLifted && contact->standing) contact.reset();
        return contact;
    };
    std::optional<BodyContact> first;
    size_t supporting = 0;
    for (size_t index = 0; index < triangles.size(); ++index) {
        const auto contact = meet(triangles[index]);
        if (contact && (!first || contact->time < first->time)) { first = contact; supporting = index; }
    }
    if (!first) return std::nullopt;
    BodyContact best = *first;
    for (size_t index = 0; index < triangles.size(); ++index) {
        const auto contact = meet(triangles[index]);
        if (contact && (contact->time - first->time) * travel <= kSupportTie && contact->face.z > best.face.z) {
            best = *contact;
            supporting = index;
        }
    }
    glm::vec3 rest = start + delta * first->time;
    rest.z = std::min(rest.z, from.z);
    return BodyRest{rest, best.face, best.normal, supporting};
}

// Ground is a face up to the walkable slope, under the body. The body may
// stand on it by its sole or hang on its edge by the bevel: native rides the
// nose of the Northshire Abbey steps down that way and hangs a yard up on a
// pillar base there.
inline bool isBodyGround(const BodyRest& rest, float minWalkableNormalZ) {
    return rest.face.z >= minWalkableNormalZ && rest.normal.z > 0.0f;
}
// Resting by the sole on a slope: a face that is not ground and flatter
// than the body's underside toward it, so that the sole point meets it
// before a lower corner of the box does.
inline bool isBodySoleOnSlope(const BodyRest& rest, float radius, float minWalkableNormalZ) {
    if (rest.face.z >= minWalkableNormalZ || glm::dot(rest.normal, rest.face) < 1.0f - 1e-4f) return false;
    return radius * kBodyBevel * rest.face.z > radius * (std::abs(rest.face.x) + std::abs(rest.face.y));
}
// Standing on ground by the sole: where the next step's yard is counted from.
// Counted from a body hung on an edge instead, the yard would be climbed
// again from every hold and a ledge of any height walked up.
inline bool isBodySoleOnGround(const BodyRest& rest, float minWalkableNormalZ) {
    return rest.face.z >= minWalkableNormalZ && rest.normal.z >= minWalkableNormalZ;
}

// A face too steep to stand on that the body is already inside of - it grew
// into a wall it stood at, or was set down astride one - is not met by any
// sweep: sweeps pass what the body overlaps, so that it can leave. It still
// turns aside the level part of a move that goes deeper into it, along its
// plane. Native at the Orgrimmar wall (1826.75, -4245.98), its radius grown
// by half with .modify scale 1.5 where it stood against the wall, slides along
// it as at scale 1; passing what it overlapped, WoWee walked through the
// wall and fell out of the world.
inline glm::vec3 turnedByOverlappedWalls(const glm::vec3& from, glm::vec3 delta,
        float radius, float height, std::span<const BodyTriangle> triangles,
        float minWalkableNormalZ, bool& turned) {
    turned = false;
    if (delta.x * delta.x + delta.y * delta.y <= 1e-12f) return delta;
    // Twice over: turned along one wall the move may head into another.
    for (int pass = 0; pass < 2; ++pass) {
        for (const auto& triangle : triangles) {
            const glm::vec3 front = glm::cross(triangle[1] - triangle[0], triangle[2] - triangle[0]);
            const float area = glm::length(front);
            if (area < 1e-5f || std::abs(front.z) >= minWalkableNormalZ * area) continue;
            const glm::vec2 level(front.x, front.y);
            const float into = glm::dot(glm::vec2(delta), level);
            if (into >= 0.0f) continue;
            const float levelSq = glm::dot(level, level);
            if (levelSq < 1e-10f) continue;
            if (bodyDepthInTriangle(from, radius, height, triangle) <= kContactSkin) continue;
            const glm::vec2 along = glm::vec2(delta) - level * (into / levelSq);
            delta.x = along.x;
            delta.y = along.y;
            turned = true;
        }
    }
    return delta;
}

inline BodyMove walkBody(glm::vec3 from, const glm::vec3& requestedDelta, float stepBudget,
        float radius, float height, std::span<const BodyTriangle> triangles,
        float minWalkableNormalZ, float maxStep, size_t heightfieldTriangles, const glm::vec3& walkedOntoSlope,
        bool wholeMove);

// A falling (or rising) step: `delta` is the whole displacement of the frame.
// `maxStep` is the step height of the walk that follows a landing;
// `heightfieldTriangles` as for walkBody.
inline BodyMove fallBody(const glm::vec3& from, const glm::vec3& delta,
        float radius, float height, std::span<const BodyTriangle> triangles,
        float minWalkableNormalZ = kMinWalkableNormalZ, float maxStep = kMaxStepUp,
        size_t heightfieldTriangles = 0, std::optional<glm::vec3> groundMove = std::nullopt) {
    BodyMove move{.position = from};
    bool turned = false;
    glm::vec3 remaining = turnedByOverlappedWalls(from, delta, radius, height, triangles, minWalkableNormalZ, turned);
    if (turned) move.deflected = true;
    move.levelKept = glm::vec2(remaining);
    for (int iteration = 0; iteration < 5 && glm::dot(remaining, remaining) > 1e-12f; ++iteration) {
        const BodyTriangle* met = nullptr;
        const auto contact = firstBodyContact(move.position, remaining, radius, height, triangles, &met);
        if (!contact) { move.position += remaining; remaining = glm::vec3(0.0f); break; }
        move.position += remaining * contact->time;
        remaining *= 1.0f - contact->time;
        const glm::vec3 n = contact->normal;
        if (n.z >= minWalkableNormalZ && remaining.z <= 0.0f && contact->face.z >= minWalkableNormalZ) {
            // Ground under the sole ends the fall, and what is left of the
            // frame's level move is walked from there: native coming down on
            // the floor beyond the Anvilmar railing (-6080.5, 389.5) 0.67 of
            // the way through a frame ends that frame its whole 0.2305 yd on
            // (11 runs of 11), not where it touched down.
            move.position += n * kContactSkin;
            move.support = BodySupport::Walkable;
            move.soleOnGround = true;
            // On the ground the move is the one the keys ask for
            // (`groundMove`, the level move of a whole frame on ground), for
            // the part of the frame that is left, whatever the fall had kept
            // of its level velocity: native turned back down the nose of a
            // step in Dun Morogh (-5164.4, 664.7) comes down on the floor
            // before it with no level velocity left, 0.52 of the way through
            // the frame, and ends that frame 0.105 yd on along its heading,
            // 0.238 yd up on the nose again (3 runs; 0.108 is the rest of a
            // frame's 0.231).
            const float left = delta.z < -1e-6f ? std::clamp(remaining.z / delta.z, 0.0f, 1.0f) : 0.0f;
            const glm::vec3 rest = groundMove ? glm::vec3(groundMove->x, groundMove->y, 0.0f) * left
                                              : glm::vec3(remaining.x, remaining.y, 0.0f);
            if ((groundMove || delta.x * delta.x + delta.y * delta.y > 1e-10f) && rest.x * rest.x + rest.y * rest.y > 1e-10f) {
                BodyMove walked = walkBody(move.position, rest, maxStep, radius, height, triangles,
                                           minWalkableNormalZ, maxStep, heightfieldTriangles, glm::vec3(0.0f), true);
                // Only a walk that stays on ground: a landing does not end in a perch or a second fall.
                if (walked.support == BodySupport::Walkable) {
                    walked.deflected = walked.deflected || move.deflected;
                    return walked;
                }
            }
            return move;
        }
        if (n.z > 0.0f && remaining.z <= 0.0f && contact->face.z >= minWalkableNormalZ) {
            // The edge of walkable ground under the bevel catches the body
            // when its sole is within kEdgeCatch of standing over that
            // ground: the fall ends there, and what is left of the frame's
            // level move is walked from there - onto the ground, or on up
            // its edge. Native coming down on the near edge of the
            // Northshire fence hull's top (-8941.81, -106.28) ends that
            // frame 0.146 yd on and 0.0055 yd higher with its sole on the
            // top, and walks on. Pressed against an Undercity wall (1552.6,
            // 227.5; 1.76 yd high, its top edge the foot of a 22.9 degree
            // slope, 0.12 yd above the top of a jump) it rises along the
            // wall with its bevel over that edge, and in the first frame
            // whose move goes down, 0.079 to 0.082 yd from the edge, takes
            // that frame's move forward and ends it with its sole on the
            // slope (38 runs of 39). So it does from touching the edge at
            // the start of a frame that begins falling, 0.0826 to 0.0829 yd
            // from it (4 runs), and met by the edge part way through a
            // frame (1 run: it ends that frame hung on the edge and walks
            // on up). 0.0834 yd from the edge and further it is not caught
            // (21 frames at 0.0834 to 0.0925: come upon the edge in the air
            // a frame later; touching it at a frame's start 0.0872 yd from
            // it; at the next section of that wall, which runs at another
            // angle to the body's sides, 0.091 to 0.093 yd from it with the
            // edge no higher above the sole, 10 runs; and at 0.10 to 0.18,
            // also at a wall 1.88 yd high, 1574.5, 185.9): the edge turns
            // it aside as any other face does and the drop pushes it back
            // off, also with a frame's whole move left. So do the rim of
            // the Exodar table, half a yard under its top, and the nose of
            // a step the body was left in the air over (walkBody). The move
            // is the fall's own, with what it has kept of its level
            // velocity. Taken along the contact, up the bevel, the rest of
            // the move left the sole short of the Undercity slope, and
            // WoWee came back down in 25 runs of 27.
            // (_experiments/20261124T160500Z.movement_collision_parity_round22)
            // The distance is taken where the body touches the edge, before
            // it is set back from it by the contact skin.
            const glm::vec3 held = move.position + n * kContactSkin;
            if (levelDistanceToTriangle(move.position, *met) <= kEdgeCatch) {
                move.position = held;
                move.support = BodySupport::Walkable;
                const glm::vec3 rest(remaining.x, remaining.y, 0.0f);
                if (rest.x * rest.x + rest.y * rest.y > 1e-10f) {
                    BodyMove walked = walkBody(held, rest, maxStep, radius, height, triangles,
                                               minWalkableNormalZ, maxStep, heightfieldTriangles, glm::vec3(0.0f), true);
                    if (walked.support == BodySupport::Walkable) {
                        walked.deflected = walked.deflected || move.deflected;
                        return walked;
                    }
                }
                return move;
            }
        }
        // What turns the level part of the move and what turns the drop.
        // The level part is turned by the direction separating body and
        // triangle, and the fall keeps what that leaves of its level
        // velocity. The drop is turned by that direction too - except where
        // an edge of the triangle crosses an edge of the underside: there
        // each of the two sides of the underside that meet in that edge
        // turns it. Native coming down with an edge of its underside on the
        // nose of a step in Dun Morogh (-5164.4, 664.7; the nose runs 4.5
        // degrees off the x axis) moves (-d / bevel, -d / bevel) for a drop
        // of d, frame after frame: -0.0174, -0.0174 for -0.0322; -0.0289,
        // -0.0288 for -0.0532; -0.0415, -0.0415 for -0.0767 (5 runs at two
        // speeds) - along its own edge, 135 degrees from the x axis, where
        // the direction separating the edges would send it square off the
        // nose, 94.5 degrees from it. So it does where the edge runs along
        // a side of the underside within a hair (an Anvilmar platform edge,
        // -6064.7, 395.8, 0.01 degrees off the x axis: (-0.0396, -0.0396)
        // for 0.0741), and along one axis alone where it runs along it
        // exactly ((-0.0171, 0.0000) for 0.0321 at the Anvilmar post).
        // The level part slides along the edge: in the first frame of that
        // fall native goes 0.024 yd along the nose, its move's part along
        // it (3 runs of 4), and over a sloping nose in the Karabor Sewers
        // (342.3, 815.8) it goes on at (-0.1784, -0.0346) a frame, its
        // 0.231 yd a frame turned by a direction 2.4 degrees from the one
        // separating the edges, for the eleven frames of the fall (turned
        // by both sides of the underside it stood still there and came down
        // on the near side of the nose, 7.6 yd from native after 3 s).
        // (_experiments/20261124T140000Z.movement_collision_parity_round20)
        const bool crossed = glm::dot(contact->bodyFaces[0], contact->bodyFaces[0]) > 0.0f;
        const glm::vec2 level(n.x, n.y);
        const float levelSq = glm::dot(level, level);
        if (levelSq < 1e-6f) {
            // Flat overhead, or flat underfoot yet too steep by its own face
            // (an edge): only the vertical motion is stopped.
            move.position.z += n.z > 0.0f ? kContactSkin : -kContactSkin;
            if (n.z < 0.0f) move.ceiling = true;
            else move.held = true;
            remaining.z = 0.0f;
            continue;
        }
        move.deflected = true;
        move.position += glm::vec3(level, 0.0f) * (kContactSkin / std::sqrt(levelSq));
        if (n.z < 0.0f && remaining.z > 0.0f) {
            // Rising under an overhang: the ascent ends, the body is not
            // pushed sideways by its own jump.
            move.ceiling = true;
            remaining.z = 0.0f;
        }
        if (crossed && remaining.z < 0.0f) {
            const float intoLevel = remaining.x * n.x + remaining.y * n.y;
            if (intoLevel < 0.0f) remaining -= glm::vec3(level, 0.0f) * (intoLevel / levelSq);
            for (const glm::vec3& side : contact->bodyFaces) {
                const glm::vec2 sideLevel(side.x, side.y);
                remaining -= glm::vec3(sideLevel, 0.0f) * (remaining.z * side.z / glm::dot(sideLevel, sideLevel));
            }
        } else {
            const float into = glm::dot(remaining, n);
            if (into < 0.0f) remaining -= glm::vec3(level, 0.0f) * (into / levelSq);
        }
        // A rising body keeps its level velocity whole whatever turns it:
        // native jumping while it slides along the Northshire fence
        // (-8942.5, -106.4; the fence 1.6 yd tall) goes on along the fence
        // as it rises, 0.058 yd a frame, and takes its whole 0.231 yd
        // forward again from the frame its sole is above the fence (2 runs
        // of 2), where a body left with the part of its velocity along the
        // fence went on along it and came down on the near side. Falling it
        // keeps what the faces leave: off the Dustwallow plank ramp
        // (-2692.0, -4695.0) native slides along a rail in the air and keeps
        // those 3.44 yd/s along the rail's direction after it has passed it.
        if (delta.z > 0.0f) continue;
        if (const float keptInto = glm::dot(move.levelKept, level); keptInto < 0.0f)
            move.levelKept -= level * (keptInto / levelSq);
    }
    // Turned aside five times with drop still left: wedged between faces
    // that are not ground. The body hangs there and its fall starts over
    // (native in such a spot drops 0.0105 yd, a first frame of fall, frame
    // after frame, and then walks on: the Karabor Sewers rock, 346.3, 816.6).
    const float left = delta.z < -1e-6f ? std::clamp(remaining.z / delta.z, 0.0f, 1.0f) : 0.0f;
    if (remaining.z < -1e-6f) move.held = true;
    if (delta.z <= 0.0f) {
        if (const auto rest = lowerBody(move.position, kContactSkin * 2.0f, radius, height, triangles)) {
            if (isBodySoleOnGround(*rest, minWalkableNormalZ)) {
                move.support = BodySupport::Walkable;
                move.soleOnGround = true;
                move.position = rest->position;
            }
        }
    }
    // A fall stopped by faces that are not ground holds the body as a perch
    // does: the move the keys ask for is walked from there - in what is left
    // of this frame and in the frames after - when it steps on, up or onto
    // ground, and otherwise the fall starts over. Native fallen into the
    // gap between an Undercity wall and a barrel beside it (1578.67,
    // 182.30), its bevel on the edge of the barrel's top 0.39 yd before its
    // sole and the wall at its back, is stopped there a tenth of the way
    // through a frame and ends that frame 0.215 yd on along its heading and
    // 0.571 yd up, on the barrel, and walks on; fallen back onto that
    // barrel's edge from the ledge beyond, it ends the frame it is stopped
    // in 0.21 to 0.24 yd on and 0.36 to 0.40 yd up (3 runs). Left in the
    // air there with what the fall had kept of its level velocity, WoWee
    // hung in that gap, its fall starting over every frame, for as long as
    // the user's client stood there (46 s, forward and backward keys held).
    // (_experiments/20261124T160500Z.movement_collision_parity_round22)
    if (move.held && move.support == BodySupport::None) {
        move.support = BodySupport::Steep;
        if (groundMove) {
            const glm::vec3 rest = glm::vec3(groundMove->x, groundMove->y, 0.0f) * left;
            if (rest.x * rest.x + rest.y * rest.y > 1e-10f) {
                BodyMove walked = walkBody(move.position, rest, maxStep, radius, height, triangles,
                                           minWalkableNormalZ, maxStep, heightfieldTriangles, glm::vec3(0.0f), true);
                if (walked.stepped && (walked.soleOnGround || walked.position.z > move.position.z + 1e-3f)) {
                    if (!walked.soleOnGround && walked.support == BodySupport::Walkable) walked.support = BodySupport::Steep;
                    walked.deflected = walked.deflected || move.deflected;
                    return walked;
                }
            }
        }
    }
    return move;
}

// A walking step. `stepBudget` is how far the body may still be lifted:
// `maxStep` above the last walkable ground, less its present height over it.
// `maxStep` is a yard for a character of scale 1 or less and the scale in
// yards above that (bodyScale). The first `heightfieldTriangles` of
// `triangles` are the terrain's; the rest belong to buildings and objects.
// `wholeMove` says the move is a frame's whole level move, not one of the
// equal parts a move longer than the radius is taken in.
inline BodyMove walkBody(glm::vec3 from, const glm::vec3& requestedDelta, float stepBudget,
        float radius, float height, std::span<const BodyTriangle> triangles,
        float minWalkableNormalZ = kMinWalkableNormalZ, float maxStep = kMaxStepUp,
        size_t heightfieldTriangles = 0, const glm::vec3& walkedOntoSlope = glm::vec3(0.0f),
        bool wholeMove = true) {
    assert(heightfieldTriangles <= triangles.size());
    assert(std::abs(requestedDelta.z) < 1e-6f);
    assert(stepBudget >= 0.0f && stepBudget <= maxStep + 1e-4f);
    BodyMove move{.position = from};
    const glm::vec3 origin = from;

    // One lift-across-lower from `start`: where it ends, or what refused it.
    struct Step {
        bool taken = false;
        bool level = false;         // nothing was in the way
        glm::vec3 end{0.0f};
        std::optional<BodyRest> rest;
        glm::vec3 blocker{0.0f};
        // The first thing in the level move's way that is not ground was an
        // edge against the body, not a corner of the body against a face.
        bool edgeMet = false;
        // Ended on a hold from which repeating the move reaches ground.
        bool leads = false;
        // `blocker` is a face that is not ground, met by the level move.
        bool wall = false;
        // The other faces that are not ground met together with it (at an
        // edge they share), as wound.
        std::vector<glm::vec3> besideWall;
        // Where the step leaves the body in the air above what it would
        // rest on: the height it ends at (at the end of `step`).
        std::optional<float> air;
    };
    const auto step = [&](const glm::vec3& start, const glm::vec3& delta, float budget) {
        Step result;
        const auto ahead = firstBodyContact(start, delta, radius, height, triangles);
        if (!ahead) {
            result.taken = result.level = true;
            result.end = start + delta;
            return result;
        }
        result.blocker = ahead->normal;
        float wallAt = std::numeric_limits<float>::infinity();
        bool wallByBox = false, undersideFirst = false;
        constexpr float kTie = 1e-4f;
        const float travel = glm::length(delta);
        // The first face that is not ground in the way of the body at `at`.
        // Of contacts met together, a corner on a face decides; of faces met
        // together by an edge they share, the one the move runs into most
        // squarely.
        static const float kBevelFaceNormalZ = 1.0f / std::sqrt(1.0f + kBodyBevel * kBodyBevel);
        // An edge met by the body's underside, not by its upright side or
        // the side's lower edge: the separating direction is then steeper
        // than the bevel faces' own normal.
        const auto underBevel = [&](const BodyContact& contact) {
            return !contact.corner && contact.normal.z >= kBevelFaceNormalZ - 1e-3f;
        };
        // A riser: a face that is not ground with an edge that is the edge
        // of ground - both its ends lie on an edge of a triangle the body
        // can stand on. Met by the bevel, that edge is the nose of a step.
        const auto isRiser = [&](const BodyTriangle& riser) {
            constexpr float kOnEdge = 1e-3f;
            const auto onSegment = [&](const glm::vec3& point, const glm::vec3& a, const glm::vec3& b) {
                const glm::vec3 ab = b - a;
                const float lengthSq = glm::dot(ab, ab);
                if (lengthSq < 1e-10f) return false;
                const float along = glm::clamp(glm::dot(point - a, ab) / lengthSq, 0.0f, 1.0f);
                return glm::distance(point, a + ab * along) <= kOnEdge;
            };
            for (const auto& ground : triangles) {
                const glm::vec3 normal = glm::cross(ground[1] - ground[0], ground[2] - ground[0]);
                const float area = glm::length(normal);
                if (area < 1e-5f || normal.z < minWalkableNormalZ * area) continue;
                for (int edge = 0; edge < 3; ++edge)
                    for (int side = 0; side < 3; ++side)
                        if (onSegment(riser[edge], ground[side], ground[(side + 1) % 3]) &&
                            onSegment(riser[(edge + 1) % 3], ground[side], ground[(side + 1) % 3]))
                            return true;
            }
            return false;
        };
        const auto firstWall = [&](const glm::vec3& at, bool pastUnderside = false) {
            std::optional<BodyContact> wall;
            for (const auto& triangle : triangles) {
                const auto contact = sweepBodyTriangle(at, delta, radius, height, triangle);
                if (!contact || contact->standing || contact->face.z >= minWalkableNormalZ) continue;
                if (pastUnderside && underBevel(*contact)) continue;
                const float later = wall ? (contact->time - wall->time) * travel : -1.0f;
                if (later < -kTie || (later <= kTie && contact->corner && !wall->corner) ||
                    (later <= kTie && contact->corner == wall->corner &&
                     glm::dot(delta, contact->front) < glm::dot(delta, wall->front)))
                    wall = contact;
            }
            return wall;
        };
        {
            const auto wall = firstWall(start);
            // An edge against the body's upright side or the side's lower
            // edge: the separating direction is then flatter than the
            // bevel faces' own normal. Under the bevel it is steeper.
            result.edgeMet = wall && !wall->corner && !underBevel(*wall);
            // What the move is turned aside by is what stops the body where
            // it stands, when that is not ground: the wall it slides along,
            // not a face only the lifted body would meet further on.
            if (wall) {
                // Turned aside along the face's own plane, also where the
                // face was met by its edge.
                result.blocker = wall->front;
                result.wall = true;
                wallAt = wall->time;
                // Whether what the move meets first is a wall stopping the
                // body's box - its upright part, from the lower corners up -
                // and nothing has met the underside before that: neither
                // ground that rises into the move (a floor rising by less
                // than a tenth of a millimetre over the move is level, as
                // for the slide below) nor a slope under the sole.
                float boxAt = std::numeric_limits<float>::infinity();
                for (const auto& triangle : triangles) {
                    const glm::vec3 front = glm::cross(triangle[1] - triangle[0], triangle[2] - triangle[0]);
                    if (front.z >= minWalkableNormalZ * glm::length(front)) continue;
                    if (const auto box = sweepBodyBoxTriangle(start, delta, radius, height, triangle))
                        boxAt = std::min(boxAt, box->time);
                }
                wallByBox = boxAt <= 1.0f;
                for (const auto& triangle : triangles) {
                    const auto contact = sweepBodyTriangle(start, delta, radius, height, triangle);
                    if (!contact || contact->standing) continue;
                    if (contact->face.z >= minWalkableNormalZ && -glm::dot(delta, contact->front) <= 1e-4f) continue;
                    if ((boxAt - contact->time) * travel > kTie) { undersideFirst = true; break; }
                }
                for (const auto& triangle : triangles) {
                    const auto contact = sweepBodyTriangle(start, delta, radius, height, triangle);
                    if (!contact || contact->standing || contact->face.z >= minWalkableNormalZ) continue;
                    if (std::abs(contact->time - wall->time) * travel <= kTie &&
                        glm::dot(contact->front, wall->front) < 1.0f - 1e-6f)
                        result.besideWall.push_back(contact->front);
                }
            }
        }
        glm::vec3 up(0.0f, 0.0f, budget);
        if (budget > 1e-4f)
            if (const auto ceiling = firstBodyContact(start, up, radius, height, triangles))
                up.z = std::max(0.0f, up.z * ceiling->time - kContactSkin);
        if (up.z <= 1e-4f) return result;
        auto across = firstBodyContact(start + up, delta, radius, height, triangles);
        // An edge of ground the lifted body's underside meets does not stop
        // the carry when the underside is over it for no more than
        // kSoleEntry of the move: the body, put that far past where it met
        // the edge, is clear of everything and stays so to the move's end.
        // Native gets over the near edge of a box top 1.006 yd above the
        // floor (Astranaar, 2770.2, -428.2; the top slopes down behind its
        // edge): +0.6514 and +0.3301 hung on the edge by its bevel, then on
        // with its whole move onto the top, its sole lifted to the yard
        // passing 0.006 yd under the edge for 0.04 yd of the move.
        if (across && underBevel(*across)) {
            // The edge is ground's when the face met at it is ground or a
            // riser: one that shares that edge with ground.
            bool ofGround = false;
            for (const auto& triangle : triangles) {
                const auto contact = sweepBodyTriangle(start + up, delta, radius, height, triangle);
                if (contact && !contact->standing && underBevel(*contact) &&
                    std::abs(contact->time - across->time) * travel <= kTie &&
                    (contact->face.z >= minWalkableNormalZ || isRiser(triangle))) { ofGround = true; break; }
            }
            const float past = across->time + kSoleEntry / travel;
            if (ofGround && past < 1.0f) {
                const glm::vec3 beyond = start + up + delta * past;
                bool clear = true;
                for (const auto& triangle : triangles)
                    if (bodyDepthInTriangle(beyond, radius, height, triangle) > kContactSkin) { clear = false; break; }
                if (clear && !firstBodyContact(beyond, delta * (1.0f - past), radius, height, triangles)) across.reset();
            }
        }
        if (across) {
            // Ground the carried body meets is walked up: the body is raised
            // by what that ground rises over the rest of the move, from
            // `lift`, as long as it meets nothing but ground - only its face
            // when `faceOnly`, not its edge under the bevel - and no higher
            // than `highest`.
            const auto upGround = [&](float lift, float highest, bool faceOnly) {
                for (int pass = 0; pass < 4 && lift <= highest; ++pass) {
                    float rise = 0.0f;
                    bool other = false;
                    for (const auto& triangle : triangles) {
                        const auto contact = sweepBodyTriangle(start + glm::vec3(0.0f, 0.0f, lift), delta,
                                                               radius, height, triangle);
                        if (!contact || contact->standing) continue;
                        if (contact->face.z < minWalkableNormalZ || (faceOnly && !contact->corner)) { other = true; break; }
                        rise = std::max(rise, -glm::dot(delta, contact->face) / contact->face.z * (1.0f - contact->time));
                    }
                    if (other) break;
                    if (rise <= 0.0f) {
                        result.taken = true;
                        if (lift > 0.0f)
                            result.rest = lowerBody(start + glm::vec3(0.0f, 0.0f, lift) + delta, lift,
                                                    radius, height, triangles, true);
                        result.end = result.rest ? result.rest->position : start + delta;
                        return true;
                    }
                    lift += rise + kContactSkin * 2.0f;
                }
                return false;
            };
            // The body lifted the whole step height meets something the body
            // at its own height does not reach - a wall leaning out over the
            // floor, a lintel - and the level move met nothing but ground
            // rising under it. The ground is walked up: the body is lifted by
            // no more than that ground rises over the move. (Lifted a yard
            // or not at all, a walker stopped for good a third of a yard
            // short of a wall leaning 12 degrees out over a Black Temple
            // floor that rises by a rounding error, 607.25, 110.33.)
            if (!result.wall && upGround(0.0f, up.z, false)) return result;
            // The lifted body carried into the face of ground goes on up it:
            // what is left of the yard bounds the lift, not how far the
            // ground then takes the body. Hung by its underside on the nose
            // of a Karabor Sewers step (337.3, 802.4; a 77 degree riser 0.68
            // yd high, ground of 43 degrees behind its top edge) 0.77 to 0.80
            // yd above the ramp its sole last stood on, native takes its
            // whole move onto that ground and ends 1.006 to 1.019 yd above
            // the ramp (11 runs of 11; the body lifted to the yard meets the
            // ground 0.008 to 0.023 yd before the move's end). Refused there,
            // the step onto the nose did not lead to ground, was not taken,
            // and the walker crept along the foot of the riser for good (the
            // user's client, and 15 paired runs of 32 where native went up in
            // all 32). Hung 0.82 to 0.83 yd above the ramp, where that move
            // would end 1.023 to 1.033 above it, native fell back down the
            // riser instead and went up at the next try (6 runs of 6), and
            // so it did hung 0.73 to 0.76 above the ramp (8 of 8), where the
            // move would end no higher than those it takes. What decides
            // either is not found, and this body goes on up.
            // (_experiments/20261124T120000Z.movement_collision_parity_round19)
            if (across->corner && !across->standing && across->face.z >= minWalkableNormalZ &&
                across->normal.z > 0.0f &&
                upGround(up.z, up.z + travel * std::sqrt(1.0f - minWalkableNormalZ * minWalkableNormalZ) / minWalkableNormalZ, true))
                return result;
            // The nose of a step under the bevel - the edge a riser shares
            // with the ground above it - is what a step lifts the body over.
            // The step refused by something beyond the nose, that turns the
            // move aside, not the riser: a wall met by a corner of the body
            // within the move, at its own height. On the nose of a step at
            // Orgrimmar (1976.75, -4802.29; a box 0.20 yd on) native creeps
            // (-0.0092, +0.0137) a frame along the box's face until it can
            // step up, nine frames, and goes on up the stairs; head-on at a
            // Stormwind wall with a kerb at its foot (-8774.14, 832.54; the
            // nose 0.08 yd on, the wall 0.23) it takes its move's part along
            // the wall's plane, 70 frames of 70. Turned along the riser
            // WoWee crept the other way in both places and never got onto
            // the Orgrimmar stairs. With the wall beyond the move (17 ms
            // frames at Stormwind) native too is turned along the riser.
            // Where what is beyond is met by an edge two faces share, which
            // of them native is turned by first is not known (the post on
            // the Dustwallow plank ramp, -2692.93, -4694.97: first one, then
            // the other); the riser is kept there.
            {
                std::optional<BodyContact> level;
                const BodyTriangle* met = nullptr;
                for (const auto& triangle : triangles) {
                    const auto contact = sweepBodyTriangle(start, delta, radius, height, triangle);
                    if (!contact || contact->standing || contact->face.z >= minWalkableNormalZ) continue;
                    if (!level || contact->time < level->time) { level = contact; met = &triangle; }
                }
                if (level && underBevel(*level) && isRiser(*met)) {
                    if (const auto beyond = firstWall(start, true); beyond && beyond->corner) {
                        result.blocker = beyond->front;
                        wallAt = beyond->time;
                    }
                }
            }
            // What the lifted body meets sooner than the body at its own
            // height meets a wall turns the move aside instead, and by its
            // face's own plane as well, chosen as above. Sliding west along
            // an overhanging wall at Orgrimmar (1980.39, -4804.32) into the
            // leaning ridge of the next pillar, which only the lifted body
            // meets within the move, native stops in the corner (2.2 s at 33
            // ms frames and at 17 ms; where the user's client stood on
            // 2026-11-23): the ridge's face the turned move runs into leads
            // back into the wall. Turned by the direction separating body
            // and ridge instead, WoWee slipped out sideways and went on
            // round the pillar, 3.7 yd past where native stands.
            // A ceiling does not turn it: a face that fronts down as
            // steeply as ground fronts up, which the lifted head comes up
            // against. Under a rock overhanging steep terrain in the Valley
            // of Trials (-520.4, -4254.4; where the user's client stood on
            // 2026-11-19), its underside 45 degrees from level, native
            // slides (+0.0353, -0.0063) a frame along the terrain face ahead
            // of its feet for as long as the key is held (four runs, 34 to
            // 60 frames each, at 33 and at 17 ms), through frames where
            // the body lifted 0.76 yd or a yard meets the underside sooner
            // (0.018 yd on) than the body at its own height meets the
            // terrain face (0.142 yd on). Turned by the
            // underside every other frame, WoWee went back and forth there
            // and came 1.3 yd less far. An upright face met sooner by the
            // lifted body does turn native (a Northshire fence post, -8944.9,
            // -118.2: five frames along the post's face, the fence met 0.011
            // yd later at the body's own height).
            if (across->time < wallAt && across->front.z > -minWalkableNormalZ) {
                const auto liftedWall = firstWall(start + up);
                result.blocker = liftedWall && liftedWall->time <= across->time + kTie / travel
                    ? liftedWall->front : across->normal;
            }
            return result;
        }
        result.taken = true;
        result.rest = lowerBody(start + up + delta, up.z, radius, height, triangles, true);
        // Nothing under the lifted body above its starting height: it was
        // carried over something lower than the step.
        result.end = result.rest ? result.rest->position : start + delta;
        // A step over a wall the box of the body was stopped by lifts the
        // body by the move over the radius - a step's height for a move of
        // the radius - and lowers it by no more than the move times the
        // bevel. What it would rest on further down it does not reach: it is
        // left in the air above it and falls. Native at a plain step in Dun
        // Morogh (-5164.4, 664.7; level floor, an upright riser 0.654 yd
        // high, level top) ends its first frame over the nose +0.3288 above
        // the floor for a move of 0.2310 and +0.2467 for 0.1733 - the move
        // times (1 / radius - bevel) - where the body lowered until it rests
        // hangs by its underside on the nose at +0.264; it then comes down
        // the nose again, turned back by it, and steps once more from the
        // floor (5 runs of 5 at speeds 0.75 and 1). With moves of 0.0693 and
        // 0.1155 (speeds 0.3 and 0.5), where the nose is within that reach,
        // it is set on the nose and climbs it in six and three frames. The
        // same rise of 1.4233 times the move, followed by a first frame of
        // fall, at 12 places and every length of frame from 26 to 41 ms
        // (58 frames; _experiments/20261124T120000Z.movement_collision_parity_round19).
        // Only for a body that stands on ground by its sole, whose box is
        // the first thing stopped, and only where the body lifted that far
        // is carried free as well. Where ground rising into the move or a
        // slope under the sole is met before the wall, native ends on what
        // the body lowered all the way rests on (a Karabor Sewers ramp
        // before its riser, 337.3, 802.4: +0.2650 where this would leave it
        // at +0.3388; the Exodar floor, rising 0.6 degrees, before the
        // table: +0.2875 against +0.3485), and so it does stepping on from
        // a steep face (+0.3020 against +0.3388); those steps stay as they
        // were. What native does where the body lifted by the move over the
        // radius is stopped and the body lifted the whole yard is not, is
        // not known.
        if (result.wall && wallByBox && !undersideFirst && result.rest) {
            const float lift = std::min(up.z, travel * maxStep / radius);
            const float lowest = start.z + lift - travel * kBodyBevel;
            const float restZ = result.rest->position.z;
            if (restZ > start.z + kContactSkin * 4.0f && lowest > restZ + kContactSkin * 2.0f &&
                (lift >= up.z - 1e-4f ||
                 !firstBodyContact(start + glm::vec3(0.0f, 0.0f, lift), delta, radius, height, triangles)))
                result.air = lowest;
        }
        return result;
    };
    // Whether repeating `delta` from a hold puts the sole on ground.
    const auto leadsToGround = [&](glm::vec3 perch, const glm::vec3& delta, float budget) {
        // The budget ends the search: every repeat that goes on rises. The
        // count only bounds the work on a hold that rises by hairs.
        for (int repeat = 0; repeat < 48; ++repeat) {
            const Step next = step(perch, delta, budget);
            if (!next.taken) return false;
            if (next.level || (next.rest && isBodySoleOnGround(*next.rest, minWalkableNormalZ))) return true;
            if (!next.rest) {
                // Carried over the hold with nothing under it down to the
                // hold's height: ground a step further down, or a pit.
                const auto below = lowerBody(next.end, maxStep, radius, height, triangles);
                return below && isBodySoleOnGround(*below, minWalkableNormalZ);
            }
            const float rise = next.end.z - perch.z;
            if (rise <= 1e-4f) return false;
            budget -= rise;
            perch = next.end;
        }
        return false;
    };
    // The whole move from `from`: a step is taken onto ground under the
    // sole; onto a hold - a steep face, or an edge under the bevel - that
    // leads there; and onto whatever is there when what stood in the level
    // move's way was an edge. Native takes that last step in the frame it
    // meets the ridge between two 74.5 degree sides of the Exodar table
    // edge-on, 12 of 12, to this height within 3 mm, and never steps onto
    // such a side met by a corner of the body (header comment).
    const auto attempt = [&](const glm::vec3& delta, bool ownMove) {
        Step first = step(from, delta, stepBudget);
        // Only the move itself steps over an edge; its part along a face it
        // was turned aside by does not. In the corner of the Northshire Abbey
        // doorway (-8916.2, -141.76) native creeps 0.001 yd a frame, where
        // stepping over the second wall's edge bobbed the body 0.09 yd.
        if (!ownMove) first.edgeMet = false;
        if (first.taken && first.rest && !isBodySoleOnGround(*first.rest, minWalkableNormalZ))
            first.leads = leadsToGround(first.end, delta, stepBudget - (first.end.z - from.z));
        // A body whose sole has walked off its ground onto a slope of a
        // building - a face that is not ground, flatter than the body's
        // underside - takes its move, lifted, carried and lowered, as a step
        // onto another face of a building that is not ground. In the
        // Karabor Sewers (347.2, 814.4; a ramp, ground of 43 degrees, a 54
        // degree bank and faces of 61 and 71 degrees around one vertex)
        // native creeps along the bank's foot, 0.007 to 0.008 yd a frame,
        // while its sole is on the ground or up to 1.07 mm beyond the
        // ground's edge over the bank (42 frames beyond the edge, 15 runs at
        // eight arrival phases), and takes its whole move in the frame it
        // starts 1.10 mm or more beyond it (20 frames; it lands 0.21 to 0.40
        // yd up, on the 61 degree face or, twice, on the bank itself, within
        // 2 mm of where the body lifted by the move over the radius, carried
        // and lowered rests), then falls from there, on over the face or
        // back down the bank. In 5 frames, 2.1 to 3.0 mm beyond the edge, it
        // fell down the bank instead; what tells those from the 20 is not
        // found. Sliding along the foot of the Orgrimmar wall with its sole
        // over the 53 degree skirt it steps 0.35 to 0.37 yd up the 77
        // degree section where that begins (13 runs) and does not step up
        // the skirt itself (3 runs); from a slope of the terrain it does not
        // step up that slope (Tanaris -7042.8, -3544.2, 6 runs). Built in
        // for a step from one face onto another; the two steps up the bank
        // itself are not.
        // (_experiments/20261124T094500Z.movement_collision_parity_round18)
        const bool ontoNextSlope = ownMove && walkedOntoSlope.z > 0.0f && first.taken && first.rest &&
            !isBodyGround(*first.rest, minWalkableNormalZ) && first.rest->triangle >= heightfieldTriangles &&
            glm::dot(walkedOntoSlope, first.rest->face) < 1.0f - 1e-4f;
        if (first.taken && first.rest && !isBodySoleOnGround(*first.rest, minWalkableNormalZ) &&
            !first.edgeMet && !first.leads && !ontoNextSlope) {
            first.taken = false;
            if (!first.wall) first.blocker = first.rest->normal;
        }
        // Faces that are not ground and stop a move over ground that rises
        // ahead - a move walked up: lifted, carried and set down with the
        // sole on ground. They are taken as the body meets them moving level
        // from where it stands, and the whole move is turned along such a
        // face from there, also where the body set down at the move's end
        // would rest clear of it.
        //
        // A face steeper than the body's underside toward it is met by a
        // lower corner of the box (sweepBodyBoxTriangle), and stops the move
        // when the corner meets it within the move. An edge or corner of a
        // face against the box's side does not count: native passes a corner
        // of the Exodar table that the box's side overlaps by its last 0.02
        // yd without turning (2 frames).
        //
        // A face flatter than the underside toward it is met by the sole, and
        // stops the move when the sole, moved level, would be more than
        // kSoleEntry beyond the face's plane: native takes a move of L yd
        // toward such a face when the plane is L - 0.096 yd or more ahead of
        // its sole and slides along the face when it is L - 0.100 or less -
        // Valley of Trials (-521.9, -4255.5), 42 degree terrain toward a 52
        // degree face, moves of 0.105 to 0.289 yd at speeds 0.5 to 1.25 and
        // 17 and 33 ms frames, 95 frames - and at L - 0.098 against L -
        // 0.151 on 47 degree terrain toward a 55 degree face in Tanaris
        // (-7042.8, -3544.2). Measured by where a corner of the box, the
        // underside or the lifted body meets the face, the frames of the two
        // places do not separate at one value
        // (_experiments/20261124T071000Z.movement_collision_parity_round17).
        // This is the terrain's: native walks up a Karabor Sewers ramp
        // onto a 54 degree bank of the building beside it with whole moves,
        // the bank's plane 0.12 and 0.05 yd ahead of its sole (341.8, 815.1,
        // where it goes on over the bank, a third of a yard wide) and 0.06
        // yd ahead (346.7, 814.4, where steeper faces follow), and onto the
        // 57 degree bump of the Dustwallow plank ramp from 0.11 yd.
        //
        // Of several such faces the flattest turns the move first, and of
        // equally steep ones the first met; a face the turned move then
        // meets turns it again. In an Eastern Plaguelands gully (2679.4,
        // -4975.1; terrain of 57, 67, 68 and 75 degrees around one vertex)
        // native is turned by the 57 degree face in every frame a corner of
        // the box meets it within the move - also when the 67 degree face is
        // met 0.002 to 0.05 yd sooner, or the 68 or 75 degree one is touched
        // already - and by the 75 degree face in the frame the 57 degree one
        // is out of reach (3 s, 85 frames; turned by the first met, the most
        // deeply entered or the one run into most squarely the replay leaves
        // native in 14 to 55 of them, and the free run went up and down 50
        // times where native does 7). Between two upright walls of an Exodar
        // pillar corner (-4181, -11468) native is turned by the one a box
        // corner meets first, frame by frame, back and forth (14 runs).
        {
            constexpr float kSameSlope = 1e-3f;
            const float travel = glm::length(delta);
            const glm::vec3 heading = delta / travel;
            struct Stop { bool met = false; float at = 0.0f; glm::vec3 front{0.0f}; };
            const auto consider = [&](Stop& stop, float at, const glm::vec3& front) {
                if (!stop.met || front.z > stop.front.z + kSameSlope ||
                    (front.z >= stop.front.z - kSameSlope && at < stop.at))
                    stop = Stop{true, at, front};
            };
            // byCorner: faces steeper than the underside, met by a corner
            // of the box; bySole: flatter ones, by the sole; anyCorner: any
            // face a corner of the box meets, which is what turns a move
            // refused for another reason.
            Stop byCorner, bySole, anyCorner;
            for (size_t index = 0; index < triangles.size(); ++index) {
                const BodyTriangle& triangle = triangles[index];
                const glm::vec3 normal = glm::cross(triangle[1] - triangle[0], triangle[2] - triangle[0]);
                const float area = glm::length(normal);
                if (area < 1e-5f || normal.z >= minWalkableNormalZ * area) continue;
                const glm::vec3 front = normal / area;
                const float into = -glm::dot(front, heading);
                if (into <= 1e-6f) continue;
                // How much nearer the face's plane the sole is than the
                // nearest lower corner of the box.
                const float soleNearer = radius * kBodyBevel * front.z - radius * (std::abs(front.x) + std::abs(front.y));
                if (const auto contact = sweepBodyBoxTriangle(from, delta, radius, height, triangle);
                    contact && contact->corner && contact->time < 1.0f - kContactSkin / travel) {
                    consider(anyCorner, contact->time, front);
                    if (soleNearer <= 0.0f) consider(byCorner, contact->time, front);
                }
                if (soleNearer <= 0.0f) continue;
                if (index >= heightfieldTriangles) continue;
                if (!first.taken || first.level || !first.rest || !isBodySoleOnGround(*first.rest, minWalkableNormalZ)) continue;
                // The face is there for the body: its underside, moved level, meets it within the move.
                const auto reached = sweepBodyTriangle(from, delta, radius, height, triangle);
                if (!reached || reached->standing) continue;
                const float ahead = glm::dot(front, from - triangle[0]) / into;
                if (ahead >= -kContactSkin && ahead < travel - kSoleEntry) consider(bySole, ahead / travel, front);
            }
            const bool walkedUp = first.taken && !first.level && first.rest &&
                                  isBodySoleOnGround(*first.rest, minWalkableNormalZ);
            Stop stop = bySole;
            if (byCorner.met) consider(stop, byCorner.at, byCorner.front);
            // Such a face stops the walk only where the walk does not lead
            // over it to ground again: the move repeated from where it ends,
            // as long as it stays on ground, until it is refused or comes to
            // rest on something that is not ground - which then has to lead
            // to ground as any hold does. Native walks up to a step with a
            // 77 degree riser in the Karabor Sewers (337.3, 802.4) and up it,
            // a corner of its box meeting the riser at the very end of the
            // move before; turned along the riser there, the walker crept
            // along it for good.
            if (stop.met && walkedUp) {
                bool leadsOver = true;
                glm::vec3 at = first.end;
                for (int repeat = 0; repeat < 8; ++repeat) {
                    const Step next = step(at, delta, maxStep);
                    if (!next.taken) { leadsOver = false; break; }
                    if (next.rest && !isBodySoleOnGround(*next.rest, minWalkableNormalZ)) {
                        const float rise = std::max(0.0f, next.end.z - at.z);
                        leadsOver = rise <= maxStep && leadsToGround(next.end, delta, maxStep - rise);
                        break;
                    }
                    at = next.end;
                }
                if (leadsOver) stop.met = false;
            }
            // A move refused for another reason is turned by the face a
            // corner of the box meets only for a body that stands on ground:
            // on a steep face it is on its way down, and what it does there
            // is the perch's. (Native standing on the 54 degree bank beside
            // the Karabor Sewers ramp, 346.8, 814.4, creeps down it for five
            // frames and then steps up onto the 61 degree face ahead; turned
            // along the 71 degree face its box meets there, the walker crept
            // along that face for good.)
            const auto standsOnGround = [&] {
                const auto under = lowerBody(from, kContactSkin * 4.0f, radius, height, triangles);
                return under && isBodySoleOnGround(*under, minWalkableNormalZ);
            };
            if (walkedUp ? stop.met : (!first.taken && anyCorner.met && standsOnGround())) {
                first.blocker = walkedUp ? stop.front : anyCorner.front;
                first.taken = false;
                first.wall = true;
            }
        }
        // An edge two faces share is stepped over onto a hold only when the
        // move cannot go on along one of them. Its part along the face it
        // runs into most squarely is taken instead when that leads away from
        // the other face and does not rise into the ground the body stands
        // on. Sliding west along the Orgrimmar wall (1819.6, -4242.7) the
        // body is carried past the end of the face it slides along, and its
        // side meets the ridge that face shares with the next one: native
        // goes on along the first face's plane, down the ramp, for two more
        // frames and is free in the third (8 runs of 8). At the ridge between
        // two sides of the Exodar table the same part of the move leads up
        // the floor (0.6 degrees, by 0.2 % of its length), and native is
        // lifted onto the ridge (67 frames of 70).
        if (ownMove && first.taken && first.edgeMet && first.wall && first.rest &&
            !isBodySoleOnGround(*first.rest, minWalkableNormalZ) && !first.leads) {
            const glm::vec2 level(first.blocker.x, first.blocker.y);
            const float levelSq = glm::dot(level, level);
            if (levelSq > 1e-8f) {
                const glm::vec2 wall = level / std::sqrt(levelSq);
                const glm::vec3 along(glm::vec2(delta) - wall * glm::dot(glm::vec2(delta), wall), 0.0f);
                // A part shorter than the contact skin takes the body nowhere.
                bool open = glm::length(along) > kContactSkin;
                for (const glm::vec3& other : first.besideWall)
                    if (glm::dot(along, other) < 0.0f) open = false;
                // Only a body standing on ground: from a hold the next step
                // on, or the fall, is the perch's (below).
                bool standing = false;
                if (open) {
                    const glm::vec3 above = from + glm::vec3(0.0f, 0.0f, kContactSkin * 2.0f);
                    const glm::vec3 down(0.0f, 0.0f, -kContactSkin * 4.0f);
                    for (const auto& triangle : triangles) {
                        const auto under = sweepBodyTriangle(above, down, radius, height, triangle);
                        if (!under || under->face.z < minWalkableNormalZ) continue;
                        standing = true;
                        if (glm::dot(along, under->front) < 0.0f) open = false;
                    }
                }
                if (open && standing) first.taken = false;
            }
        }
        return first;
    };

    bool turned = false;
    const glm::vec3 horizontalDelta = turnedByOverlappedWalls(from, requestedDelta, radius, height,
                                                              triangles, minWalkableNormalZ, turned);
    if (turned) move.deflected = true;

    bool perched = false;
    if (glm::dot(horizontalDelta, horizontalDelta) > 1e-12f) {
        // The move itself; refused, its part along the refusing face; refused
        // again, the part of that along the second face, when that does not
        // lead back into the first (the foot of a wall where a slope meets
        // it). In a corner nothing is left and the body stays.
        Step taken = attempt(horizontalDelta, true);
        const bool firstOwn = taken.taken;
        glm::vec2 intent(horizontalDelta.x, horizontalDelta.y);
        if (!taken.taken) {
            // A face that is not ground, met by a corner of the body before
            // anything else: the body goes up to it first, and the rest of
            // the move is turned aside from there. Where ground comes first
            // - any floor that rises ahead - or the face is met by its edge,
            // the whole move is turned aside from where the body stands, and
            // it is left standing off the wall by whatever the last free
            // frame left (header comment).
            // Ground is in the way where it rises into the move: a level
            // floor touches the resting body all along. Rising by less than
            // a tenth of a millimetre over the move it is level: native goes
            // up to the wall on Anvilmar's floor (0.037 degrees, 0.03 mm a
            // frame) and does not on terrain rising 1.05 mm a frame.
            constexpr float kLevelRise = 1e-4f;
            const float travel = glm::length(horizontalDelta);
            std::optional<BodyContact> ahead;
            for (const auto& triangle : triangles) {
                const auto contact = sweepBodyTriangle(from, horizontalDelta, radius, height, triangle);
                if (!contact || contact->standing) continue;
                if (contact->face.z >= minWalkableNormalZ &&
                    -glm::dot(horizontalDelta, contact->front) <= kLevelRise) continue;
                if (!ahead || preferSupportContact(*contact, *ahead, travel)) ahead = contact;
            }
            if (ahead && ahead->corner && ahead->face.z < minWalkableNormalZ) {
                const glm::vec2 level(ahead->normal.x, ahead->normal.y);
                const float levelLength = glm::length(level);
                const float into = -glm::dot(intent, level);
                if (levelLength > 1e-3f && into > 1e-6f) {
                    // Up to the contact skin short of the face.
                    const float reach = std::max(0.0f, ahead->time - kContactSkin * levelLength / into);
                    from += horizontalDelta * reach;
                    intent *= 1.0f - reach;
                    move.position = from;
                }
            }
        }
        glm::vec2 firstWall(0.0f);
        for (int slide = 0; slide < 2 && !taken.taken; ++slide) {
            move.deflected = true;
            const glm::vec2 level(taken.blocker.x, taken.blocker.y);
            const float levelSq = glm::dot(level, level);
            if (levelSq <= 1e-8f) break;
            const glm::vec2 wall = level / std::sqrt(levelSq);
            intent -= wall * glm::dot(intent, wall);
            if (glm::dot(intent, intent) <= 1e-12f) break;
            if (slide == 1 && glm::dot(intent, firstWall) < -1e-6f) break;
            firstWall = wall;
            // A move turned along a face runs along it and is not stopped by
            // it. Rounding leaves the turned move a hair to one side of the
            // face's plane or the other; a hair into it, a body resting
            // within the contact skin of that face was stopped by it again
            // and stayed where it was for good (Orgrimmar, the 53 degree
            // skirt under the wall at 1816.17, -4242.35, at 7 ms frames).
            // Leaning out of the plane by a hundred-thousandth of the move
            // settles the side.
            intent += wall * (1e-5f * glm::length(intent));
            taken = attempt(glm::vec3(intent, 0.0f), false);
        }
        const auto standsBySole = [&] {
            const auto under = lowerBody(origin, kContactSkin * 4.0f, radius, height, triangles);
            return under && isBodySoleOnGround(*under, minWalkableNormalZ);
        };
        // Of a frame's whole move only: a move longer than the radius is
        // lifted by the whole yard (round 15), and at that step native at
        // speeds 1.5 and 2 comes to rest on the nose in its first frame
        // (+0.4552 for a move of 0.3465) as the body lowered all the way
        // does.
        if (taken.taken && taken.air && firstOwn && wholeMove && standsBySole()) {
            // Stepped into the air: the fall begins in the next frame.
            move.position = glm::vec3(taken.end.x, taken.end.y, *taken.air);
            move.stepped = true;
            return move;
        }
        if (taken.taken) {
            move.position = taken.end;
            move.stepped = !taken.level;
            // Hung by the bevel on the edge of ground the body stands - it
            // rides the nose of a stair that way - unless it came there over
            // an edge and the move does not lead on to ground: native set on
            // the Exodar table's rim after a step onto the ridge falls in the
            // next frame, 7 of 7.
            perched = taken.rest && !isBodySoleOnGround(*taken.rest, minWalkableNormalZ) &&
                      (!isBodyGround(*taken.rest, minWalkableNormalZ) || (taken.edgeMet && !taken.leads));
        }
    }

    if (perched) {
        move.support = BodySupport::Steep;
        return move;
    }
    // The walking body follows what is under it down no steeper than its own
    // bevel: by the level move times kBodyBevel in one frame. Native's last
    // walking frame before a fall goes down by exactly that when nothing is
    // under it within that reach (0.4271 yd for 0.2310 at the Anvilmar
    // railing, the Anvilmar post and in the Karabor Sewers, 16 frames of 16;
    // no walking frame of 425 runs goes down steeper) and starts to fall in
    // the next frame from there. What it meets within the reach it is set
    // on: ground, to walk on; anything else - a steep face, or an edge not
    // of ground - as a perch.
    const float reach = std::max(glm::length(glm::vec2(move.position - origin)) * kBodyBevel, kContactSkin * 4.0f);
    const auto rest = lowerBody(move.position, reach, radius, height, triangles);
    if (rest && isBodyGround(*rest, minWalkableNormalZ)) {
        move.support = BodySupport::Walkable;
        move.soleOnGround = isBodySoleOnGround(*rest, minWalkableNormalZ);
        move.position = rest->position;
        return move;
    }
    if (!rest) {
        move.position.z -= reach;
        return move;
    }
    // The sole has come down on a slope of a building, off the ground the
    // walk began on: told to the caller for the next walk (ontoNextSlope
    // above). The body itself is lowered as before.
    const bool soleOnSlope = isBodySoleOnSlope(*rest, radius, minWalkableNormalZ);
    if (soleOnSlope && rest->triangle >= heightfieldTriangles) {
        const auto before = lowerBody(origin, kContactSkin * 4.0f, radius, height, triangles);
        if (before && isBodyGround(*before, minWalkableNormalZ)) move.walkedOntoSlope = rest->face;
    }
    // Held up by a steep face under the bevel: lowered as a fall lowers it,
    // the steep face turning it aside, so a body sliding along the foot of a
    // slope comes down onto the ground beside it.
    const BodyMove settled = fallBody(move.position, glm::vec3(0.0f, 0.0f, -reach),
                                      radius, height, triangles, minWalkableNormalZ, maxStep, heightfieldTriangles);
    if (settled.support == BodySupport::Walkable) {
        move.support = BodySupport::Walkable;
        move.soleOnGround = settled.soleOnGround;
        move.position = settled.position;
    } else {
        move.support = BodySupport::Steep;
        move.position = rest->position;
        move.soleOnSlope = soleOnSlope;
    }
    return move;
}

} // namespace wowee::rendering::movement
