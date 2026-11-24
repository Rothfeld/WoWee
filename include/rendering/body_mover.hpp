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
//  is a step only when the lifted move meets nothing. A step refused, the
//  move is replaced by its part along the refusing face and tried again from
//  the same place (native's slide frames are that projection, with no
//  advance toward the face first).
//
//  Support. A face up to 50 degrees holds the body (ground), under its sole
//  or under its bevel at the face's edge. The yard a step may rise is counted
//  from where the sole last stood. A step may end short of ground under the
//  sole - on a steeper face, where the body is only perched and next frame
//  steps on or falls, or hung by the bevel on an edge.
//
//  Falling. Horizontal move and drop are swept together. Ground ends the
//  fall, under the sole or caught by the bevel. Any other face turns the body
//  aside horizontally and leaves the drop alone, so a steep face is slid down
//  at free-fall speed. Wedged between such faces the body hangs and its fall
//  starts over each frame.
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

enum class BodySupport { None, Steep, Walkable };

struct BodyMove {
    glm::vec3 position;
    BodySupport support = BodySupport::None;
    bool soleOnGround = false;  // Walkable, and standing on it by the sole
    bool stepped = false;    // lifted over or onto something
    bool deflected = false;  // the horizontal move was turned aside or stopped
    bool ceiling = false;    // an upward move was stopped
    bool held = false;       // a fall was stopped by faces that are not ground
};

inline std::optional<BodyContact> firstBodyContact(const glm::vec3& from, const glm::vec3& delta,
        float radius, float height, std::span<const BodyTriangle> triangles) {
    const float travel = glm::length(delta);
    std::optional<BodyContact> first;
    for (const auto& triangle : triangles) {
        const auto contact = sweepBodyTriangle(from, delta, radius, height, triangle);
        if (contact && (!first || preferSupportContact(*contact, *first, travel))) first = contact;
    }
    return first;
}

struct BodyRest {
    glm::vec3 position;
    glm::vec3 face;    // up-facing normal of the supporting triangle
    glm::vec3 normal;  // separating direction of the contact
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
    for (const auto& triangle : triangles) {
        const auto contact = meet(triangle);
        if (contact && (!first || contact->time < first->time)) first = contact;
    }
    if (!first) return std::nullopt;
    BodyContact best = *first;
    for (const auto& triangle : triangles) {
        const auto contact = meet(triangle);
        if (contact && (contact->time - first->time) * travel <= kSupportTie && contact->face.z > best.face.z)
            best = *contact;
    }
    glm::vec3 rest = start + delta * first->time;
    rest.z = std::min(rest.z, from.z);
    return BodyRest{rest, best.face, best.normal};
}

// Ground is a face up to the walkable slope, under the body. The body may
// stand on it by its sole or hang on its edge by the bevel: native rides the
// nose of the Northshire Abbey steps down that way and hangs a yard up on a
// pillar base there.
inline bool isBodyGround(const BodyRest& rest, float minWalkableNormalZ) {
    return rest.face.z >= minWalkableNormalZ && rest.normal.z > 0.0f;
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
        float minWalkableNormalZ, float maxStep);

// A falling (or rising) step: `delta` is the whole displacement of the frame.
// `maxStep` is the step height of the walk that follows a landing.
inline BodyMove fallBody(const glm::vec3& from, const glm::vec3& delta,
        float radius, float height, std::span<const BodyTriangle> triangles,
        float minWalkableNormalZ = kMinWalkableNormalZ, float maxStep = kMaxStepUp) {
    BodyMove move{.position = from};
    bool turned = false;
    glm::vec3 remaining = turnedByOverlappedWalls(from, delta, radius, height, triangles, minWalkableNormalZ, turned);
    if (turned) move.deflected = true;
    for (int iteration = 0; iteration < 5 && glm::dot(remaining, remaining) > 1e-12f; ++iteration) {
        const auto contact = firstBodyContact(move.position, remaining, radius, height, triangles);
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
            const glm::vec3 rest(remaining.x, remaining.y, 0.0f);
            if (delta.x * delta.x + delta.y * delta.y > 1e-10f && rest.x * rest.x + rest.y * rest.y > 1e-10f) {
                BodyMove walked = walkBody(move.position, rest, maxStep, radius, height, triangles,
                                           minWalkableNormalZ, maxStep);
                // Only a walk that stays on ground: a landing does not end in a perch or a second fall.
                if (walked.support == BodySupport::Walkable) {
                    walked.deflected = walked.deflected || move.deflected;
                    return walked;
                }
            }
            return move;
        }
        if (n.z > 0.0f && remaining.z <= 0.0f && contact->face.z >= minWalkableNormalZ) {
            // The edge of walkable ground under the bevel. The rest of the
            // move goes along the contact, up the bevel; the fall ends if
            // that stands the sole on the ground. Native coming down on the
            // near edge of the Northshire fence hull's top (-8941.81, -106.28,
            // its bevel 0.21 yd over the edge) ends that frame 0.146 yd on
            // and 0.0055 yd higher with its sole on the top, and walks on.
            // Where it does not - hung by the bevel on the rim of the Exodar
            // table, the top half a yard further up - native keeps falling,
            // turned aside by the rim as by any other face (7 of 7).
            const glm::vec3 held = move.position + n * kContactSkin;
            const float into = glm::dot(remaining, n);
            const glm::vec3 along = into < 0.0f ? remaining - n * into : remaining;
            const auto stop = firstBodyContact(held, along, radius, height, triangles);
            const glm::vec3 end = held + along * (stop ? stop->time : 1.0f);
            const float rise = std::max(end.z - held.z, 0.0f);
            if (const auto rest = lowerBody(end, rise + kContactSkin * 2.0f, radius, height, triangles);
                rest && isBodySoleOnGround(*rest, minWalkableNormalZ)) {
                move.position = rest->position;
                move.support = BodySupport::Walkable;
                move.soleOnGround = true;
                return move;
            }
        }
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
        const float into = glm::dot(remaining, n);
        if (into < 0.0f) remaining -= glm::vec3(level, 0.0f) * (into / levelSq);
    }
    // Turned aside five times with drop still left: wedged between faces
    // that are not ground. The body hangs there and its fall starts over
    // (native in such a spot drops 0.0105 yd, a first frame of fall, frame
    // after frame).
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
    return move;
}

// A walking step. `stepBudget` is how far the body may still be lifted:
// `maxStep` above the last walkable ground, less its present height over it.
// `maxStep` is a yard for a character of scale 1 or less and the scale in
// yards above that (bodyScale).
inline BodyMove walkBody(glm::vec3 from, const glm::vec3& requestedDelta, float stepBudget,
        float radius, float height, std::span<const BodyTriangle> triangles,
        float minWalkableNormalZ = kMinWalkableNormalZ, float maxStep = kMaxStepUp) {
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
        {
            constexpr float kTie = 1e-4f;
            const float travel = glm::length(delta);
            std::optional<BodyContact> wall;
            for (const auto& triangle : triangles) {
                const auto contact = sweepBodyTriangle(start, delta, radius, height, triangle);
                if (!contact || contact->standing || contact->face.z >= minWalkableNormalZ) continue;
                const float later = wall ? (contact->time - wall->time) * travel : -1.0f;
                // Of contacts met together, a corner on a face decides; of
                // faces met together by an edge they share, the one the move
                // runs into most squarely.
                if (later < -kTie || (later <= kTie && contact->corner && !wall->corner) ||
                    (later <= kTie && contact->corner == wall->corner &&
                     glm::dot(delta, contact->front) < glm::dot(delta, wall->front)))
                    wall = contact;
            }
            // An edge against the body's upright side or the side's lower
            // edge: the separating direction is then flatter than the
            // bevel faces' own normal. Under the bevel it is steeper.
            static const float kBevelFaceNormalZ = 1.0f / std::sqrt(1.0f + kBodyBevel * kBodyBevel);
            result.edgeMet = wall && !wall->corner && wall->normal.z < kBevelFaceNormalZ - 1e-3f;
            // What the move is turned aside by is what stops the body where
            // it stands, when that is not ground: the wall it slides along,
            // not a face only the lifted body would meet further on.
            if (wall) {
                // Turned aside along the face's own plane, also where the
                // face was met by its edge.
                result.blocker = wall->front;
                result.wall = true;
                wallAt = wall->time;
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
        if (const auto across = firstBodyContact(start + up, delta, radius, height, triangles)) {
            // The body lifted the whole step height meets something the body
            // at its own height does not reach - a wall leaning out over the
            // floor, a lintel - and the level move met nothing but ground
            // rising under it. The ground is walked up: the body is lifted by
            // no more than that ground rises over the move. (Lifted a yard
            // or not at all, a walker stopped for good a third of a yard
            // short of a wall leaning 12 degrees out over a Black Temple
            // floor that rises by a rounding error, 607.25, 110.33.)
            if (!result.wall) {
                float lift = 0.0f;
                for (int pass = 0; pass < 4 && lift <= up.z; ++pass) {
                    float rise = 0.0f;
                    bool other = false;
                    for (const auto& triangle : triangles) {
                        const auto contact = sweepBodyTriangle(start + glm::vec3(0.0f, 0.0f, lift), delta,
                                                               radius, height, triangle);
                        if (!contact || contact->standing) continue;
                        if (contact->face.z < minWalkableNormalZ) { other = true; break; }
                        rise = std::max(rise, -glm::dot(delta, contact->face) / contact->face.z * (1.0f - contact->time));
                    }
                    if (other) break;
                    if (rise <= 0.0f) {
                        result.taken = true;
                        if (lift > 0.0f)
                            result.rest = lowerBody(start + glm::vec3(0.0f, 0.0f, lift) + delta, lift,
                                                    radius, height, triangles, true);
                        result.end = result.rest ? result.rest->position : start + delta;
                        return result;
                    }
                    lift += rise + kContactSkin * 2.0f;
                }
            }
            if (across->time < wallAt) result.blocker = across->normal;
            return result;
        }
        result.taken = true;
        result.rest = lowerBody(start + up + delta, up.z, radius, height, triangles, true);
        // Nothing under the lifted body above its starting height: it was
        // carried over something lower than the step.
        result.end = result.rest ? result.rest->position : start + delta;
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
        if (first.taken && first.rest && !isBodySoleOnGround(*first.rest, minWalkableNormalZ) &&
            !first.edgeMet && !first.leads) {
            first.taken = false;
            if (!first.wall) first.blocker = first.rest->normal;
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
    // Held up by a steep face under the bevel: lowered as a fall lowers it,
    // the steep face turning it aside, so a body sliding along the foot of a
    // slope comes down onto the ground beside it.
    const BodyMove settled = fallBody(move.position, glm::vec3(0.0f, 0.0f, -reach),
                                      radius, height, triangles, minWalkableNormalZ);
    if (settled.support == BodySupport::Walkable) {
        move.support = BodySupport::Walkable;
        move.soleOnGround = settled.soleOnGround;
        move.position = settled.position;
    } else {
        move.support = BodySupport::Steep;
        move.position = rest->position;
    }
    return move;
}

} // namespace wowee::rendering::movement
