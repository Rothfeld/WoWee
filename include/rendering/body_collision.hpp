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
using BodyTriangle = std::array<glm::vec3, 3>;

// Separation left between a body and the face it was stopped by. A contact
// in a crease pushes the body this far along one face's normal, which can
// carry it this far into the neighbouring face.
inline constexpr float kContactSkin = 0.0014f;

struct BodyContact {
    float time;
    glm::vec3 normal;
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
    float enter = -std::numeric_limits<float>::infinity();
    float leave = std::numeric_limits<float>::infinity();
    float enterSpeed = 0.0f;
    glm::vec3 normal(0.0f);
    const auto axisOverlap = [&](glm::vec3 axis) {
        const float lengthSq = glm::dot(axis, axis);
        if (lengthSq < 1e-10f) return true;
        axis /= std::sqrt(lengthSq);
        const float extent = radius * (std::abs(axis.x) + std::abs(axis.y));
        const float shapeLow = std::min({0.0f, bevel*axis.z-extent, height*axis.z-extent});
        const float shapeHigh = std::max({0.0f, bevel*axis.z+extent, height*axis.z+extent});
        const float low = std::min({glm::dot(a,axis),glm::dot(b,axis),glm::dot(c,axis)}) - shapeHigh;
        const float high = std::max({glm::dot(a,axis),glm::dot(b,axis),glm::dot(c,axis)}) - shapeLow;
        const float speed = glm::dot(delta,axis);
        if (std::abs(speed) < 1e-8f) return low <= 0.0f && high >= 0.0f;
        const float first = std::min(low/speed,high/speed);
        const float last = std::max(low/speed,high/speed);
        if (first > enter) { enter=first; enterSpeed=std::abs(speed); normal=speed>0.0f ? -axis : axis; }
        leave = std::min(leave,last);
        return enter <= leave;
    };
    if (!axisOverlap(triangleNormal)) return std::nullopt;
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
    // Existing overlap must not trap an actor or block departure. Continuous
    // tests catch entry before overlap; tangent contact has no entering time.
    //
    // Except an overlap no deeper than the contact skin, moving deeper: that is
    // a face the body touches, entered by the skin push off its neighbour in a
    // crease. Ignored, the body passed through it - at the Alcaz Island dock
    // (redridgedocks03) a body sliding into the fold between the ramp and the
    // riser in front of the beam went through the riser and was held inside
    // the deck. A departing body entered long ago and is still let go.
    if (enter > 1.0f || leave < 0.0f) return std::nullopt;
    if (enter < -1e-5f && -enter * enterSpeed > kContactSkin) return std::nullopt;
    return BodyContact{std::max(0.0f,enter),normal};
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

// Grounded step: lift the body by up to maxStepUp (less under a ceiling),
// move it across without rising, then lower it no further than the original
// sole. The result rests on whatever authored face the descent meets first,
// whatever that face's slope: build 8606 places the body at exactly this
// height against the 74-degree sides of the Exodar table and on hammer
// handles. Whether that face can support standing is the caller's floor
// decision. A descent that meets nothing ends at the original sole height,
// on the ground the walker already stood on.
inline glm::vec3 stepBodyAcross(const glm::vec3& from, const glm::vec3& to,
        float radius, float height, std::span<const BodyTriangle> triangles, float maxStepUp) {
    assert(std::abs(to.z - from.z) < 1e-6f);
    assert(std::isfinite(maxStepUp) && maxStepUp > 0.0f);
    const auto firstContact = [&](const glm::vec3& start, const glm::vec3& delta) {
        std::optional<BodyContact> first;
        for (const auto& triangle : triangles) {
            const auto contact = sweepBodyTriangle(start, delta, radius, height, triangle);
            if (contact && (!first || contact->time < first->time)) first = contact;
        }
        return first;
    };
    glm::vec3 up(0.0f, 0.0f, maxStepUp);
    if (const auto ceiling = firstContact(from, up))
        up.z = std::max(0.0f, up.z * ceiling->time - kContactSkin);
    glm::vec3 across;
    moveBody(from + up, to + up, radius, height, triangles, across, false);
    const glm::vec3 down(0.0f, 0.0f, from.z - across.z);
    const auto support = firstContact(across, down);
    return across + down * (support ? support->time : 1.0f);
}

} // namespace wowee::rendering::movement
