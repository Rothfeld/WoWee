#pragma once

#include "rendering/body_collision.hpp"
#include "rendering/movement_limits.hpp"

// One movement step of the upright beveled body among collision triangles
// (terrain, WMO and doodad alike), after build 8606's walking and falling.
// Derived from native per-frame positions read at 200 Hz, replayed against
// WoWee's own collision triangles for the same places
// (_experiments/20261123T182500Z.movement_collision_parity in the workspace).
//
// Per frame, with the whole frame's horizontal move (native takes it in one
// piece at 16 to 36 ms frames):
//
//  Walking. The move is taken level when nothing is in its way. Otherwise the
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
// and 84. What separates them in every case measured is where the step leads.
// The bump and (on the paths that climbed it) the table have walkable ground
// within the yard; the hull, the rock face and the wall do not. So a step that
// does not put the sole on ground is taken here only when repeating the same
// move from it does, within the step budget. Native also hops onto the
// table's sides where that is not so and falls back, and hangs a yard up on a
// Northshire Abbey pillar base; WoWee slides along both instead.
namespace wowee::rendering::movement {

// Furthest a walking body is lowered onto ground in one frame. Native lowers
// 0.44 yd within one frame at the Anvilmar railing and 0.31 yd on the
// plank ramp; larger single-frame drops are not measured, and the step-up
// limit is used for both directions.
inline constexpr float kMaxStepDown = kMaxStepUp;

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
inline std::optional<BodyRest> lowerBody(const glm::vec3& from, float distance,
        float radius, float height, std::span<const BodyTriangle> triangles) {
    assert(distance > 0.0f);
    constexpr float kSupportTie = 0.002f;
    const glm::vec3 start = from + glm::vec3(0.0f, 0.0f, kContactSkin * 2.0f);
    const float travel = distance + kContactSkin * 2.0f;
    const glm::vec3 delta(0.0f, 0.0f, -travel);
    std::optional<BodyContact> first;
    for (const auto& triangle : triangles) {
        const auto contact = sweepBodyTriangle(start, delta, radius, height, triangle);
        if (contact && (!first || contact->time < first->time)) first = contact;
    }
    if (!first) return std::nullopt;
    BodyContact best = *first;
    for (const auto& triangle : triangles) {
        const auto contact = sweepBodyTriangle(start, delta, radius, height, triangle);
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

// A falling (or rising) step: `delta` is the whole displacement of the frame.
inline BodyMove fallBody(const glm::vec3& from, const glm::vec3& delta,
        float radius, float height, std::span<const BodyTriangle> triangles,
        float minWalkableNormalZ = kMinWalkableNormalZ) {
    BodyMove move{.position = from};
    glm::vec3 remaining = delta;
    for (int iteration = 0; iteration < 5 && glm::dot(remaining, remaining) > 1e-12f; ++iteration) {
        const auto contact = firstBodyContact(move.position, remaining, radius, height, triangles);
        if (!contact) { move.position += remaining; remaining = glm::vec3(0.0f); break; }
        move.position += remaining * contact->time;
        remaining *= 1.0f - contact->time;
        const glm::vec3 n = contact->normal;
        if (n.z > 0.0f && remaining.z <= 0.0f && contact->face.z >= minWalkableNormalZ) {
            // Ground, met by the sole or caught on its edge by the bevel:
            // native clears the Northshire fence hull (1.60 to 1.65 yd) with
            // a 1.64 yd jump and walks on over it.
            move.position += n * kContactSkin;
            move.support = BodySupport::Walkable;
            move.soleOnGround = n.z >= minWalkableNormalZ;
            return move;
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
            if (isBodyGround(*rest, minWalkableNormalZ)) {
                move.support = BodySupport::Walkable;
                move.soleOnGround = isBodySoleOnGround(*rest, minWalkableNormalZ);
                move.position = rest->position;
            }
        }
    }
    return move;
}

// A walking step. `stepBudget` is how far the body may still be lifted: one
// yard above the last walkable ground, less its present height over it.
inline BodyMove walkBody(const glm::vec3& from, const glm::vec3& horizontalDelta, float stepBudget,
        float radius, float height, std::span<const BodyTriangle> triangles,
        float minWalkableNormalZ = kMinWalkableNormalZ) {
    assert(std::abs(horizontalDelta.z) < 1e-6f);
    assert(stepBudget >= 0.0f && stepBudget <= kMaxStepUp + 1e-4f);
    BodyMove move{.position = from};

    // One lift-across-lower from `start`: where it ends, or what refused it.
    struct Step {
        bool taken = false;
        bool level = false;         // nothing was in the way
        glm::vec3 end{0.0f};
        std::optional<BodyRest> rest;
        glm::vec3 blocker{0.0f};
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
        glm::vec3 up(0.0f, 0.0f, budget);
        if (budget > 1e-4f)
            if (const auto ceiling = firstBodyContact(start, up, radius, height, triangles))
                up.z = std::max(0.0f, up.z * ceiling->time - kContactSkin);
        if (up.z <= 1e-4f) return result;
        if (const auto across = firstBodyContact(start + up, delta, radius, height, triangles)) {
            result.blocker = across->normal;
            return result;
        }
        result.taken = true;
        result.rest = lowerBody(start + up + delta, up.z, radius, height, triangles);
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
                const auto below = lowerBody(next.end, kMaxStepDown, radius, height, triangles);
                return below && isBodySoleOnGround(*below, minWalkableNormalZ);
            }
            const float rise = next.end.z - perch.z;
            if (rise <= 1e-4f) return false;
            budget -= rise;
            perch = next.end;
        }
        return false;
    };
    // The whole move from `from`: a step is taken only onto ground under the
    // sole, or onto a hold - a steep face, or an edge under the bevel - that
    // leads there.
    const auto attempt = [&](const glm::vec3& delta) {
        Step first = step(from, delta, stepBudget);
        if (first.taken && first.rest && !isBodySoleOnGround(*first.rest, minWalkableNormalZ) &&
            !leadsToGround(first.end, delta, stepBudget - (first.end.z - from.z))) {
            first.taken = false;
            first.blocker = first.rest->normal;
        }
        return first;
    };

    bool perched = false;
    if (glm::dot(horizontalDelta, horizontalDelta) > 1e-12f) {
        // The move itself; refused, its part along the refusing face; refused
        // again, the part of that along the second face, when that does not
        // lead back into the first (the foot of a wall where a slope meets
        // it). In a corner nothing is left and the body stays.
        Step taken = attempt(horizontalDelta);
        glm::vec2 intent(horizontalDelta.x, horizontalDelta.y);
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
            taken = attempt(glm::vec3(intent, 0.0f));
        }
        if (taken.taken) {
            move.position = taken.end;
            move.stepped = !taken.level;
            perched = taken.rest && !isBodyGround(*taken.rest, minWalkableNormalZ);
        }
    }

    if (perched) {
        move.support = BodySupport::Steep;
        return move;
    }
    const auto rest = lowerBody(move.position, kMaxStepDown, radius, height, triangles);
    if (rest && isBodyGround(*rest, minWalkableNormalZ)) {
        move.support = BodySupport::Walkable;
        move.soleOnGround = isBodySoleOnGround(*rest, minWalkableNormalZ);
        move.position = rest->position;
        return move;
    }
    // Held up by a steep face under the bevel, or by nothing: lowered as a
    // fall lowers it, the steep face turning it aside, so a body sliding
    // along the foot of a slope comes down onto the ground beside it.
    const BodyMove settled = fallBody(move.position, glm::vec3(0.0f, 0.0f, -kMaxStepDown),
                                      radius, height, triangles, minWalkableNormalZ);
    if (settled.support == BodySupport::Walkable) {
        move.support = BodySupport::Walkable;
        move.soleOnGround = settled.soleOnGround;
        move.position = settled.position;
    }
    return move;
}

} // namespace wowee::rendering::movement
