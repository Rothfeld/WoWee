#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <optional>
#include <limits>
#include <span>
#include <glm/glm.hpp>

namespace wowee::rendering::movement {

// Build 8606, 0x632650: sole point, four lower corners at radius *
// 1.8493989706039429, four upper corners at collision height. The body
// is an upright beveled box, including a flat head, not a capsule.
inline constexpr float kBodyBevel = 1.8493989706039429f;

// Lower body vertices and edge midpoints, expressed relative to the sole.
// An offset floor supports the sole at floorZ - sample.z, not at floorZ.
inline std::array<glm::vec3, 9> bodySupportPoints(float radius) {
    assert(radius > 0.0f && std::isfinite(radius));
    const float bevel = radius * kBodyBevel;
    return {{{0, 0, 0}, {radius, 0, bevel}, {-radius, 0, bevel},
             {0, radius, bevel}, {0, -radius, bevel},
             {radius, radius, bevel}, {radius, -radius, bevel},
             {-radius, radius, bevel}, {-radius, -radius, bevel}}};
}
// Wound so that (b - a) x (c - a) points out of the solid, as WMO and M2
// collision triangles are stored.
using BodyTriangle = std::array<glm::vec3, 3>;

// Separation left between a body and the face it was stopped by. A contact
// in a crease pushes the body this far along one face's normal, which can
// carry it this far into the neighbouring face.
inline constexpr float kContactSkin = 0.0014f;

// The heightfield has no back: its triangles face up whatever order the mesh
// generator hands them in.
inline BodyTriangle upFacing(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
    return glm::cross(b - a, c - a).z < 0.0f ? BodyTriangle{a, c, b} : BodyTriangle{a, b, c};
}

struct BodyContact {
    float time;
    glm::vec3 normal;
    // The touched triangle's own unit normal, facing up. `normal` is the
    // separating direction, which at an edge is not the face's.
    glm::vec3 face;
    // Not met by moving: the body stood in this triangle already and is
    // carried by it (sweepBodyTriangle).
    bool standing = false;
    // A corner of the body met the triangle's face: the separating direction
    // is the face's own normal. Otherwise an edge or corner of the triangle
    // met a side, bevel or edge of the body.
    bool corner = false;
};

inline std::optional<BodyContact> sweepBodyTriangle(const glm::vec3& from,
        const glm::vec3& delta, float radius, float height, const BodyTriangle& triangle) {
    assert(radius > 0.0f && height >= radius * kBodyBevel);
    const float bevel = radius * kBodyBevel;
    const glm::vec3 a = triangle[0] - from;
    const glm::vec3 b = triangle[1] - from;
    const glm::vec3 c = triangle[2] - from;
    const std::array edges = {b-a, c-b, a-c};
    const glm::vec3 triangleNormal = glm::cross(edges[0], edges[1]);
    if (glm::dot(triangleNormal, triangleNormal) < 1e-10f) return std::nullopt;
    // A triangle is solid from its front only, the side its winding faces:
    // a body moving with the winding normal passes; one moving along the
    // face still meets its edges. Native steps over the edge of the coral
    // sheet around a Karabor Sewers rock (Black Temple, 346.8, 816.5) and
    // comes down through its back, and stands 0.1 yd lower than a back-facing
    // face under the Northshire Abbey steps; solid from both sides, WoWee
    // rested on the sheet's back, wedged against the rock in an endless fall.
    if (glm::dot(delta, triangleNormal) > 0.0f) return std::nullopt;
    float enter = -std::numeric_limits<float>::infinity();
    float leave = std::numeric_limits<float>::infinity();
    float enterSpeed = 0.0f;
    float standingDepth = std::numeric_limits<float>::infinity();
    glm::vec3 normal(0.0f);
    bool enteredOnFace = false;
    const auto axisOverlap = [&](glm::vec3 axis, bool faceAxis = false) {
        const float lengthSq = glm::dot(axis, axis);
        if (lengthSq < 1e-10f) return true;
        axis /= std::sqrt(lengthSq);
        const float extent = radius * (std::abs(axis.x) + std::abs(axis.y));
        const float shapeLow = std::min({0.0f, bevel*axis.z-extent, height*axis.z-extent});
        const float shapeHigh = std::max({0.0f, bevel*axis.z+extent, height*axis.z+extent});
        const float low = std::min({glm::dot(a,axis),glm::dot(b,axis),glm::dot(c,axis)}) - shapeHigh;
        const float high = std::max({glm::dot(a,axis),glm::dot(b,axis),glm::dot(c,axis)}) - shapeLow;
        // How far the body would have to move along this axis to clear the
        // triangle where it stands; the least over all axes is how deep it is in.
        standingDepth = std::min(standingDepth, std::min(-low, high));
        const float speed = glm::dot(delta,axis);
        if (std::abs(speed) < 1e-8f) return low <= 0.0f && high >= 0.0f;
        const float first = std::min(low/speed,high/speed);
        const float last = std::max(low/speed,high/speed);
        if (first > enter) { enter=first; enterSpeed=std::abs(speed); normal=speed>0.0f ? -axis : axis; enteredOnFace=faceAxis; }
        leave = std::min(leave,last);
        return enter <= leave;
    };
    if (!axisOverlap(triangleNormal, true)) return std::nullopt;
    const std::array<glm::vec3,7> faceAxes = {{
        {1,0,0},{0,1,0},{0,0,1},
        {kBodyBevel,0,-1},{-kBodyBevel,0,-1},
        {0,kBodyBevel,-1},{0,-kBodyBevel,-1}}};
    for (const auto& axis:faceAxes) if (!axisOverlap(axis)) return std::nullopt;
    const std::array<glm::vec3,7> bodyEdges = {{
        {1,0,0},{0,1,0},{0,0,1},
        {1,1,kBodyBevel},{-1,1,kBodyBevel},
        {1,-1,kBodyBevel},{-1,-1,kBodyBevel}}};
    for (const auto& edge:edges)
        for (const auto& bodyEdge:bodyEdges)
            if (!axisOverlap(glm::cross(edge,bodyEdge))) return std::nullopt;
    // A triangle the body already stands in, deeper than the contact skin,
    // still carries it: it stops the downward part of a move, and nothing
    // else. Native set down inside the steep face of a rubble block in the
    // Karabor Sewers (347.5, 815.7, 0.3 yd under the face, nothing below)
    // hangs there; ignored, as overlaps once were, that face let WoWee fall
    // out of the world, and a body sunk in its floor fell through it.
    //
    // Horizontally such a triangle is not there: existing overlap must not
    // trap an actor or block departure. (Native is turned aside by a coral
    // sheet it has come down astride at the rock beside that rubble - a frame
    // with no horizontal move, then a second of sliding off. Stopping all
    // motion against the front of a triangle stood in reproduces that frame
    // and then keeps the body there for good.)
    if (enter > 1.0f || leave < 0.0f) return std::nullopt;
    if (standingDepth > kContactSkin) {
        const glm::vec3 front = glm::normalize(triangleNormal);
        if (delta.z >= 0.0f || front.z <= 0.0f) return std::nullopt;
        return BodyContact{0.0f, glm::vec3(0.0f, 0.0f, 1.0f), front, true};
    }
    // Touching, not in. A contact entered before this move began is one the
    // body is leaving or sliding along - except within the skin and moving
    // deeper: a face entered by the skin push off its neighbour in a crease
    // (the Alcaz Island dock, redridgedocks03), which answers like any contact.
    if (enter < -1e-5f && -enter * enterSpeed > kContactSkin) return std::nullopt;
    glm::vec3 face = glm::normalize(triangleNormal);
    if (face.z < 0.0f) face = -face;
    return BodyContact{std::max(0.0f,enter),normal,face,false,enteredOnFace};
}

inline bool moveBody(const glm::vec3& from, const glm::vec3& to,
        float radius, float height, std::span<const BodyTriangle> triangles,
        glm::vec3& result, bool allowHorizontalRise = true) {
    result=from;
    glm::vec3 remaining=to-from;
    bool collided=false;
    for (int iteration=0;iteration<5;++iteration) {
        std::optional<BodyContact> first;
        for (const auto& triangle:triangles) {
            const auto contact=sweepBodyTriangle(result,remaining,radius,height,triangle);
            if (contact && (!first || contact->time<first->time)) first=contact;
        }
        if (!first) { result+=remaining; return collided; }
        collided=true;
        result+=remaining*first->time;
        glm::vec3 responseNormal=first->normal;
        if (std::abs(to.z-from.z)<1e-6f &&
                (responseNormal.z<0.0f || !allowHorizontalRise)) {
            // Horizontal contact cannot force feet down. Airborne movement
            // cannot climb a beveled contact as though stepping up either.
            responseNormal.z=0.0f;
            const float horizontalLength=glm::length(responseNormal);
            if (horizontalLength>1e-6f) responseNormal/=horizontalLength;
        }
        result+=responseNormal*kContactSkin;
        remaining*=1.0f-first->time;
        if (to.z>from.z && first->normal.z<0.0f &&
                glm::length(glm::vec2(to-from))<1e-6f) return collided;
        remaining-=responseNormal*std::min(0.0f,glm::dot(remaining,responseNormal));
        if (glm::dot(remaining,remaining)<1e-10f) return collided;
    }
    // Contact budget exhausted at a corner. Remain at the last solid position.
    return collided;
}

// When a vertical sweep meets the shared edge between a stair's vertical riser
// (face.z == 0) and its horizontal tread (face.z == 1), both triangles report
// the same hit distance within floating-point rounding. Prefer the flatter
// face so the landing normal reflects the walkable tread regardless of triangle
// ordering in the mesh.
inline bool preferSupportContact(const BodyContact& candidate,
                                 const BodyContact& current,
                                 float travelLength) {
    constexpr float kTieDistance = 1e-4f;
    const float diff = (candidate.time - current.time) * travelLength;
    if (diff < -kTieDistance) return true;
    if (diff > kTieDistance) return false;
    return candidate.face.z > current.face.z;
}

// Grounded step: lift the body by up to maxStepUp (less under a ceiling),
// move it across without rising, then lower it no further than the original
// sole. The result rests on whatever authored face the descent meets first,
// whatever that face's slope: build 8606 places the body at exactly this
// height against the 74-degree sides of the Exodar table and on hammer
// handles. Whether that face can support standing is the caller's floor
// decision. A descent that meets nothing ends at the original sole height,
// on the ground the walker already stood on.
//
// `landingFace`, when given, receives the up-facing normal of the face the
// descent rests on; `landingNormal` receives the separating axis normal (which
// is vertical when the flat sole lands on a horizontal tread and ~0.4757 when
// the lower bevel rides a sharp edge).
inline glm::vec3 stepBodyAcross(const glm::vec3& from, const glm::vec3& to,
        float radius, float height, std::span<const BodyTriangle> triangles, float maxStepUp,
        glm::vec3* landingFace = nullptr, glm::vec3* landingNormal = nullptr) {
    assert(std::abs(to.z - from.z) < 1e-6f);
    assert(std::isfinite(maxStepUp) && maxStepUp > 0.0f);
    const auto firstContact = [&](const glm::vec3& start, const glm::vec3& delta) {
        const float travel = glm::length(delta);
        std::optional<BodyContact> first;
        for (const auto& triangle : triangles) {
            const auto contact = sweepBodyTriangle(start, delta, radius, height, triangle);
            if (contact && (!first || preferSupportContact(*contact, *first, travel))) {
                first = contact;
            }
        }
        return first;
    };
    glm::vec3 up(0.0f, 0.0f, maxStepUp);
    if (const auto ceiling = firstContact(from, up))
        up.z = std::max(0.0f, up.z * ceiling->time - kContactSkin);
    glm::vec3 across;
    moveBody(from + up, to + up, radius, height, triangles, across, false);
    const glm::vec3 down(0.0f, 0.0f, from.z - across.z);
    const auto support = (down.z < -1e-6f) ? firstContact(across, down) : std::nullopt;
    if (landingFace) *landingFace = support ? support->face : glm::vec3(0.0f);
    if (landingNormal) *landingNormal = support ? support->normal : glm::vec3(0.0f);
    return across + down * (support ? support->time : 1.0f);
}

// Constrain a downward grounding snap (`requestedZ < from.z`) against the
// complete upright beveled body. Starts `2 * kContactSkin` above `from.z` so a
// body already resting in skin-depth crease contact with a stair bevel is not
// treated as a pre-existing overlap and dropped through the step. Returns true
// only when authored geometry blocks the descent above `requestedZ`, writing
// the clamped sole height to `supportZ`, the separating axis Z (`first->normal.z`)
// to `supportNormalZ`, and optionally the touched triangle's upward face normal
// Z (`first->face.z`) to `outFaceZ`.
inline bool constrainBodyDescent(const glm::vec3& from, float requestedZ,
        float radius, float height, std::span<const BodyTriangle> triangles,
        float& supportZ, float& supportNormalZ, float* outFaceZ = nullptr) {
    assert(requestedZ < from.z);
    const glm::vec3 probeFrom(from.x, from.y, from.z + kContactSkin * 2.0f);
    const glm::vec3 delta(0.0f, 0.0f, requestedZ - probeFrom.z);
    const float travel = probeFrom.z - requestedZ;
    std::optional<BodyContact> first;
    for (const auto& triangle : triangles) {
        const auto contact = sweepBodyTriangle(probeFrom, delta, radius, height, triangle);
        if (contact && (!first || preferSupportContact(*contact, *first, travel))) {
            first = contact;
        }
    }
    if (!first) return false;
    const float hitZ = std::min(from.z, probeFrom.z + delta.z * first->time);
    if (hitZ <= requestedZ + 1e-4f) return false;
    supportZ = hitZ;
    supportNormalZ = first->normal.z;
    if (outFaceZ) *outFaceZ = first->face.z;
    return true;
}

} // namespace wowee::rendering::movement
