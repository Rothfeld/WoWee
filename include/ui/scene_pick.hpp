#pragma once

// Ray-picking entities under the cursor, shared by the two callers that need it.
//
// The left-click target picker and the right-click world picker each grew their
// own copy of this traversal. They agreed on everything that matters - how a
// unit is ranked against an object, that a hostile is preferred, that a corpse
// must not outrank the living - and had to be edited in lockstep to stay that
// way, which is exactly how one of them quietly diverges. What genuinely
// differs between them is expressed as parameters instead.

#include <cstdint>
#include <functional>
#include <memory>

#include <cmath>
#include <algorithm>

#include <glm/glm.hpp>

#include "rendering/camera.hpp"  // rendering::Ray

namespace wowee {
namespace game { class GameHandler; class Entity; }

namespace ui {

/// Where a ray first meets a sphere, if it does at all.
///
/// The picker's own test, and the one every hand-rolled click target in the
/// interface needs. It was written out five times - once here and once in each
/// of the four files GameScreen was split into - and three of those four copies
/// were never called from the file they sat in. Identical to the character, so
/// nothing had gone wrong yet; five copies of a quadratic is simply five
/// chances for one of them to lose a sign.
inline bool raySphereIntersect(const rendering::Ray& ray, const glm::vec3& center,
                               float radius, float& tOut) {
    const glm::vec3 oc = ray.origin - center;
    const float b = glm::dot(oc, ray.direction);
    const float c = glm::dot(oc, oc) - radius * radius;
    const float disc = b * b - c;
    if (disc < 0.0f) return false;
    const float root = std::sqrt(disc);
    float t = -b - root;
    // Behind the near root means the origin is inside the sphere; the far root
    // is then the first surface the ray meets going forward.
    if (t < 0.0f) t = -b + root;
    if (t < 0.0f) return false;
    tOut = t;
    return true;
}

/// Occlude only when a ray crosses a rendered terrain surface from above.
/// A hole has no surface or vertical side wall: starting below a later valid
/// height sample is not an intersection. WMO geometry handles interior walls.
template<class HeightAt>
bool terrainOccludesSceneRay(const rendering::Ray& ray, float hitDistance,
                             HeightAt&& heightAt) {
    auto height = heightAt(ray.origin.x, ray.origin.y);
    std::optional<float> previousClearance;
    if (height) previousClearance = ray.origin.z - *height;
    constexpr float step = 0.5f;
    const int steps = std::max(1, static_cast<int>(std::ceil(hitDistance / step)));
    for (int i = 1; i <= steps; ++i) {
        const glm::vec3 point = ray.origin + ray.direction *
            (hitDistance * static_cast<float>(i) / static_cast<float>(steps));
        height = heightAt(point.x, point.y);
        if (!height) {
            previousClearance.reset();
            continue;
        }
        const float clearance = point.z - *height;
        if (previousClearance && *previousClearance >= -0.02f && clearance < -0.02f)
            return true;
        previousClearance = clearance;
    }
    return false;
}

/// The parts of picking the two callers disagree about.
struct ScenePickParams {
    /// Fallback hit sphere for a unit with no render bounds. The right-click
    /// picker uses a slightly taller, tighter sphere than the target picker.
    float unitHitRadius = 1.8f;
    float unitHeightOffset = 1.0f;
    bool trace = false; // Click diagnostics; hover stays quiet.
};

/// Everything the traversal found, before either caller applies its own rules.
/// What the cursor is on, ranked as build 8606 ranks it (CGWorldFrame
/// 0x4ae760 -> CM2Scene 0x70b2b0; docs/unit-size-rules.md): the nearest
/// surface the ray reaches, whatever kind of object it belongs to. A dead
/// unit with nothing left to do - no loot for this player, not skinnable -
/// is only considered when nothing else was hit (0x60e870 defers it).
struct ScenePick {
    uint64_t closestGuid = 0;          ///< Nearest hit, deferred corpses excluded.
    float    closestT = 1e30f;
    bool     closestInteractive = false; ///< False for scenery a click does not use.

    uint64_t deferredGuid = 0;         ///< Nearest dead unit with nothing to do.
    float    deferredT = 1e30f;

    /// The nearest game object hit, interactive or not - what a name tooltip
    /// describes even when a click would not use it.
    uint64_t namedObjectGuid = 0;
    float    namedObjectT = 1e30f;

    /// What a click acts on: the nearest hit when it is something a click
    /// uses, nothing when scenery is in front of it, the deferred corpse when
    /// nothing else was hit at all.
    [[nodiscard]] uint64_t resolve() const {
        if (closestGuid != 0) return closestInteractive ? closestGuid : 0;
        return deferredGuid;
    }
};

using ScenePickHit = std::function<void(uint64_t guid,
                                        const std::shared_ptr<game::Entity>& entity,
                                        float hitT, float centerT)>;

ScenePick pickScene(game::GameHandler& gameHandler,
                    const rendering::Ray& ray,
                    const ScenePickParams& params,
                    const ScenePickHit& onHit = {});

} // namespace ui
} // namespace wowee
