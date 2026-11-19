#include "rendering/camera_controller.hpp"

#include "core/env_flag.hpp"
#include "rendering/swim_wall.hpp"
#include "rendering/water_camera.hpp"
#include "core/click_drag.hpp"
#include "core/coordinates.hpp"
#include "ui/keybinding_manager.hpp"
#include "ui/framexml_takeover.hpp"
#include "rendering/movement_limits.hpp"
#include "rendering/body_collision.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <future>
#include <limits>
#include <sstream>
#include <string>
#include <stdexcept>
#include "core/thread_pool.hpp"
#include <imgui.h>
#include "rendering/terrain_manager.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/water_renderer.hpp"
#include "rendering/character_renderer.hpp"
#include "game/opcodes.hpp"
#include "core/logger.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <cmath>
#include <limits>

namespace wowee {
namespace rendering {

namespace {

constexpr float kMaxPhysicsDelta = 1.0f / 30.0f;
// Measured native 2.4.3 drift after a standing jump; see moveFollowedCharacter.
constexpr float kAirborneDriftSpeed = 2.5f;

/// WoW's own key bone id for the head, as the M2 skeleton names it.
constexpr int32_t kKeyBoneHead = 6;

std::optional<float> selectReachableFloor(const std::optional<float>& terrainH,
                                          const std::optional<float>& wmoH,
                                          float refZ,
                                          float maxStepUp) {
    // Filter to reachable floors (not too far above)
    std::optional<float> reachTerrain;
    std::optional<float> reachWmo;
    if (terrainH && *terrainH <= refZ + maxStepUp) reachTerrain = terrainH;
    if (wmoH && *wmoH <= refZ + maxStepUp) reachWmo = wmoH;

    if (reachTerrain && reachWmo) {
        // Prefer the highest surface - prevents clipping through
        // WMO floors that sit above terrain.
        return (*reachWmo >= *reachTerrain) ? reachWmo : reachTerrain;
    }
    if (reachWmo) return reachWmo;
    if (reachTerrain) return reachTerrain;
    return std::nullopt;
}

std::optional<float> selectHighestFloor(const std::optional<float>& a,
                                        const std::optional<float>& b,
                                        const std::optional<float>& c) {
    std::optional<float> best;
    auto consider = [&](const std::optional<float>& h) {
        if (!h) return;
        if (!best || *h > *best) best = h;
    };
    consider(a);
    consider(b);
    consider(c);
    return best;
}

std::optional<float> selectClosestFloor(const std::optional<float>& a,
                                        const std::optional<float>& b,
                                        float refZ) {
    if (a && b) {
        float da = std::abs(*a - refZ);
        float db = std::abs(*b - refZ);
        return (da <= db) ? a : b;
    }
    if (a) return a;
    if (b) return b;
    return std::nullopt;
}

std::optional<float> selectReachableFloor3(const std::optional<float>& a,
                                           const std::optional<float>& b,
                                           const std::optional<float>& c,
                                           float refZ,
                                           float maxStepUp) {
    std::optional<float> best;
    auto consider = [&](const std::optional<float>& h) {
        if (!h) return;
        if (*h > refZ + maxStepUp) return;
        if (!best || *h > *best) best = h;
    };
    consider(a);
    consider(b);
    consider(c);
    return best;
}


/// How flat a surface has to be to stand on rather than slide off.
///
/// Three names for one number. They are kept apart because the surfaces are:
/// a WMO tunnel or bridge ramp is authored steeper than outdoor terrain, and if
/// that ever needs different limits this is where they part. Today they agree.
constexpr float MIN_WALKABLE_NORMAL_TERRAIN = movement::kMinWalkableNormalZ;
constexpr float MIN_WALKABLE_NORMAL_WMO = movement::kMinWalkableNormalZ;
constexpr float MIN_WALKABLE_NORMAL_M2 = movement::kMinWalkableNormalZ;

/// The points around the feet a floor probe samples.
///
/// One ray straight down at the character's centre falls between two planks, or
/// misses a stair tread, and the character drops through a floor that is plainly
/// there. Sampling a ring around the feet instead is what stops it. Five points
/// for a cheap check, nine when something has already gone wrong and a wider
/// footprint is worth the queries.
///
/// The ring was written out five times, each with its own footprint constant
/// spelled into nine brace pairs.
std::array<glm::vec2, 5> feetCross(float footprint) {
    return {{ {0.0f, 0.0f},
              { footprint, 0.0f}, {-footprint, 0.0f},
              {0.0f,  footprint}, {0.0f, -footprint} }};
}

std::array<glm::vec2, 9> feetRing(float footprint) {
    return {{ {0.0f, 0.0f},
              { footprint, 0.0f}, {-footprint, 0.0f},
              {0.0f,  footprint}, {0.0f, -footprint},
              { footprint,  footprint}, { footprint, -footprint},
              {-footprint,  footprint}, {-footprint, -footprint} }};
}

/// The band a floor has to be in to be believed: high enough to step onto,
/// low enough not to be the ceiling or another storey.
struct FloorBand {
    float minZ;
    float maxZ;
};

/// The highest walkable floor among the sampled points, if any.
///
/// `query` is a renderer's getFloorHeight - it answers a height and writes the
/// surface normal's Z, and a normal flatter than `minNormalZ` is a wall rather
/// than something to stand on.
template <typename Offsets, typename Query>
std::optional<float> highestWalkableFloor(Query&& query, float x, float y,
                                          const Offsets& offsets, float probeZ,
                                          float minNormalZ, FloorBand band) {
    std::optional<float> best;
    for (const auto& o : offsets) {
        float normalZ = 1.0f;
        auto h = query(x + o.x, y + o.y, probeZ, &normalZ);
        if (!h) continue;
        if (normalZ < minNormalZ) continue;
        if (*h > band.maxZ || *h < band.minZ) continue;
        if (!best || *h > *best) best = h;
    }
    return best;
}

} // namespace

CameraController::CameraController(Camera* cam) : camera(cam) {
    yaw = defaultYaw;
    facingYaw = defaultYaw;
    pitch = defaultPitch;
    reset();
}

/// The floor dumps, which are a hunt rather than a fault.
///
/// Each is ten lines of every WMO group and doodad near a position, written
/// whenever a floor probe comes up empty - which happens in ordinary play and
/// is recovered from. In one six-minute session they were ninety-one lines of
/// eleven hundred, at the level these logs carry, describing something that
/// then worked. The one-line statement that a probe found nothing stays; the
/// inventory behind it is asked for.
///
/// F8 still dumps on demand, which is somebody asking for exactly this.
bool floorDiagEnabled() {
    static const bool on = core::envFlagEnabled("WOWEE_FLOOR_DIAG", false);
    return on;
}

void CameraController::startIntroPan(float durationSec, float orbitDegrees) {
    if (!camera) return;
    introActive = true;
    introTimer = 0.0f;
    idleTimer_ = 0.0f;
    introDuration = std::max(0.5f, durationSec);
    introStartYaw = yaw;
    introEndYaw = yaw - orbitDegrees;
    introOrbitDegrees = orbitDegrees;
    introStartPitch = pitch;
    introEndPitch = pitch;
    introStartDistance = currentDistance;
    introEndDistance = currentDistance;
    thirdPerson = true;
}
void CameraController::setFollowedCollisionHeight(float height) {
    if (height == followedCollisionHeight_) return;
    followedCollisionHeight_ = height;
    LOG_INFO("Camera pivot: followed CollisionHeight ", height, ", pivot ",
             height > 0.0f ? nativePivotHeight(pivotHeight_, height, false) : 0.0f, " on foot, ",
             height > 0.0f ? nativePivotHeight(pivotHeight_, height, true) : 0.0f, " above a seat");
}

float CameraController::raymarchTerrainCameraLimit(const glm::vec3& pivot, const glm::vec3& camDir,
                                                   float maxDist) const {
    if (!terrainManager || maxDist <= MIN_DISTANCE) return maxDist;

    // Clearance the camera keeps above the terrain surface along the whole ray.
    // Enough to keep the near plane out of the ground and no more: the quarter
    // yard on top of the sphere that used to be here was felt as the ground
    // pushing the camera forward on every rise.
    constexpr float kClearance = CAM_SPHERE_RADIUS + 0.08f;

    // If the pivot itself sits below the heightfield (caves, WMO basements,
    // terrain holes under cities), the heightfield is not the relevant occluder
    // here - skip limiting rather than pinning the camera to first-person.
    auto pivotH = terrainManager->getHeightAt(pivot.x, pivot.y);
    if (pivotH && pivot.z < *pivotH - 0.2f) return maxDist;

    // Coarse march in ~1.25-unit steps (capped so worst case stays cheap),
    // then bisect between the last clear sample and the first blocked one.
    const int steps = std::clamp(static_cast<int>(maxDist / 1.25f) + 1, 4, 24);
    const float stepLen = maxDist / static_cast<float>(steps);
    float prevT = 0.0f;
    for (int i = 1; i <= steps; ++i) {
        float t = stepLen * static_cast<float>(i);
        glm::vec3 p = pivot + camDir * t;
        auto h = terrainManager->getHeightAt(p.x, p.y);
        if (h && p.z < *h + kClearance) {
            float lo = prevT;
            float hi = t;
            for (int j = 0; j < 4; ++j) {
                float mid = 0.5f * (lo + hi);
                glm::vec3 mp = pivot + camDir * mid;
                auto mh = terrainManager->getHeightAt(mp.x, mp.y);
                if (mh && mp.z < *mh + kClearance) hi = mid; else lo = mid;
            }
            return std::max(MIN_DISTANCE, lo);
        }
        prevT = t;
    }
    return maxDist;
}

std::optional<float> CameraController::cameraTerrainFloor(float x, float y,
                                                         const glm::vec3& targetPos) const {
    if (!terrainManager) return std::nullopt;
    auto feetH = terrainManager->getHeightAt(targetPos.x, targetPos.y);
    if (feetH && targetPos.z < *feetH - 0.2f) return std::nullopt;

    // The near plane is wider than a point. On a ridge or a bank the ground a
    // few hand-widths to the side can stand well above the ground straight
    // below, and a camera held clear of only the latter looks into the hill.
    constexpr float kFootprint = 0.35f;
    //
    // A hole-cut quad is a cave mouth or a way underground; the height across
    // it is interpolated and is not ground, so it holds nothing up.
    std::optional<float> best;
    for (const auto& [ox, oy] : {std::pair{0.0f, 0.0f},
                                 std::pair{kFootprint, 0.0f}, std::pair{-kFootprint, 0.0f},
                                 std::pair{0.0f, kFootprint}, std::pair{0.0f, -kFootprint}}) {
        if (terrainManager->isHoleAt(x + ox, y + oy)) continue;
        auto h = terrainManager->getHeightAt(x + ox, y + oy);
        if (h && (!best || *h > *best)) best = h;
    }
    return best;
}

void CameraController::triggerShake(float magnitude, float frequency, float duration) {
    // Scaled here rather than at each caller, so the setting covers the sources
    // that exist - a spell's shake and a thunderstorm's - and any added later.
    magnitude *= shakeScale_;
    // Allow stronger shake to override weaker; don't allow zero magnitude.
    if (magnitude <= 0.0f || duration <= 0.0f) return;
    if (magnitude > shakeMagnitude_ || shakeElapsed_ >= shakeDuration_) {
        shakeMagnitude_ = magnitude;
        shakeFrequency_ = frequency;
        shakeDuration_  = duration;
        shakeElapsed_   = 0.0f;
    }
}

glm::vec3 CameraController::sweepAgainstWalls(const glm::vec3& from, const glm::vec3& to,
                                              bool includeDoodads) {
    const glm::vec3 delta = to - from;
    const float distSq = glm::dot(delta, delta);
    if (distSq <= 1e-4f) return to;   // stationary: not worth a collision call

    const float dist = std::sqrt(distSq);
    // Tighter steps indoors, where the geometry is closer together.
    const float stepSize = cachedInsideWMO ? 0.20f : 0.35f;
    const int steps = std::max(1, std::min(8, static_cast<int>(std::ceil(dist / stepSize))));
    const glm::vec3 stepDelta = delta / static_cast<float>(steps);

    glm::vec3 stepPos = from;
    for (int i = 0; i < steps; i++) {
        glm::vec3 candidate = stepPos + stepDelta;

        if (wmoRenderer) {
            glm::vec3 adjusted;
            if (!grounded) {
                // Airborne bodies have no step-up allowance. Separate lateral
                // contact from head/foot contact, as for authored M2 geometry.
                const glm::vec3 horizontal(candidate.x, candidate.y, stepPos.z);
                glm::vec3 horizontalAdjusted;
                wmoRenderer->checkWallCollision(stepPos, horizontal, horizontalAdjusted,
                    cachedInsideWMO, true, collisionRadius_, collisionHeight_);
                candidate = horizontalAdjusted + glm::vec3(0, 0, candidate.z - stepPos.z);
                if (wmoRenderer->checkWallCollision(horizontalAdjusted, candidate, adjusted,
                        cachedInsideWMO, true, collisionRadius_, collisionHeight_)) {
                    if (adjusted.z > candidate.z + 0.00001f && verticalVelocity <= 0.0f &&
                            !swimming && !isFlightAirborne()) {
                        // Beveled-foot contact against a rail side is not a
                        // landing. Marking it grounded enables a step-up onto
                        // a top that the jump never reached.
                        const auto support = wmoRenderer->getFloorHeight(
                            adjusted.x, adjusted.y, adjusted.z + 0.01f);
                        if (support && std::abs(*support - adjusted.z) <= 0.02f) {
                            grounded = true;
                            verticalVelocity = 0.0f;
                        }
                    }
                    if (adjusted.z < candidate.z - 0.00001f && verticalVelocity > 0.0f)
                        verticalVelocity = 0.0f;
                    candidate = adjusted;
                }
            } else if (wmoRenderer->checkWallCollision(stepPos, candidate, adjusted, cachedInsideWMO)) {
                ++movementTraceWmoContacts_;
                candidate.x = adjusted.x;
                candidate.y = adjusted.y;
                // Up is a ramp; down would be a wall pulling the feet under the floor.
                candidate.z = std::max(candidate.z, adjusted.z);
            }
        }

        if (includeDoodads && m2Renderer && !externalFollow_) {
            // Resolve lateral movement before vertical movement. A diagonal
            // head contact can come from running into a side, not from the
            // jump itself; only the vertical sweep may cancel ascent.
            glm::vec3 horizontal(candidate.x, candidate.y, stepPos.z);
            glm::vec3 horizontalAdjusted;
            const bool horizontalContact = m2Renderer->checkCollision(stepPos, horizontal, horizontalAdjusted,
                collisionRadius_, collisionHeight_, grounded);
            if (horizontalContact) {
                ++movementTraceM2Contacts_;
                static const bool traceContact = core::envFlagEnabled("WOWEE_BODY_CONTACT_TRACE");
                if (traceContact) {
                    LOG_INFO("M2 horizontal contact: from=", stepPos.x, ",", stepPos.y, ",", stepPos.z,
                        " requested=", horizontal.x, ",", horizontal.y, ",", horizontal.z,
                        " resolved=", horizontalAdjusted.x, ",", horizontalAdjusted.y, ",", horizontalAdjusted.z,
                        " grounded=", grounded);
                }
            }
            // Build 8606 steps a grounded body onto the authored face it walks
            // into, whatever that face's slope: native feet rest exactly on the
            // Exodar table's 74-degree sides and on hammer handles before
            // falling or walking on. Ground-follow then decides whether that
            // face supports standing. Ledger #12.
            //
            // A step that ends on a face too steep to stand on leaves the body
            // there at rest, and the next frame it steps again - onto walkable
            // ground or not at all. Alcaz Island dock (redridgedocks03): the
            // deck meets the beam in front of the boardwalk with a 0.4-yard
            // riser of 57-66 degree faces. 8606 at 20 and at 60 fps walks
            // over it without losing speed, one frame on the riser
            // (-2697.70, -4694.68, 9.67) and the next on the beam; it does not
            // climb the Exodar table's 74-degree pedestal at 20 fps, where the
            // second step would end on the pedestal side again.
            if (horizontalContact && (grounded || stepFromRest_) && !swimming && !isFlightAirborne()) {
                glm::vec3 landingFace(0.0f, 0.0f, 1.0f);
                const glm::vec3 stepped = m2Renderer->stepBody(stepPos, horizontal,
                    collisionRadius_, collisionHeight_, movement::kMaxStepUp, &landingFace);
                const glm::vec2 intent(horizontal - stepPos);
                bool farther = glm::dot(glm::vec2(stepped - stepPos), intent) >
                    glm::dot(glm::vec2(horizontalAdjusted - stepPos), intent) + 1e-6f;
                const bool fromRest = !grounded;
                if (fromRest && !movement::isWalkableNormal(landingFace.z)) farther = false;
                // The step's raised path must also clear building geometry.
                if (farther && wmoRenderer) {
                    const glm::vec3 raised(stepPos.x, stepPos.y, std::max(stepPos.z, stepped.z));
                    glm::vec3 ignored;
                    farther = !wmoRenderer->checkWallCollision(stepPos, raised, ignored,
                                  cachedInsideWMO, true, collisionRadius_, collisionHeight_) &&
                              !wmoRenderer->checkWallCollision(raised, stepped, ignored,
                                  cachedInsideWMO, true, collisionRadius_, collisionHeight_);
                }
                static const bool traceStep = core::envFlagEnabled("WOWEE_BODY_CONTACT_TRACE");
                if (traceStep) {
                    LOG_INFO("M2 grounded step: from=", stepPos.x, ",", stepPos.y, ",", stepPos.z,
                        " stepped=", stepped.x, ",", stepped.y, ",", stepped.z,
                        " ordinary=", horizontalAdjusted.x, ",", horizontalAdjusted.y, ",", horizontalAdjusted.z,
                        " selected=", farther);
                }
                if (farther) {
                    horizontalAdjusted = stepped;
                    stepFinishedThisFrame_ = true;
                    if (fromRest) {
                        grounded = true;
                        verticalVelocity = 0.0f;
                        stepFromRest_ = false;
                    }
                }
            }
            candidate = horizontalAdjusted + glm::vec3(0.0f, 0.0f, candidate.z - stepPos.z);
            glm::vec3 adjusted;
            if (m2Renderer->checkCollision(horizontalAdjusted, candidate, adjusted,
                    collisionRadius_, collisionHeight_)) {
                ++movementTraceM2Contacts_;
                // A beveled side contact stops penetration, but only a
                // walkable surface at the feet can end a fall. Otherwise the
                // next horizontal sweep would gain a grounded step allowance.
                if (adjusted.z > candidate.z + 0.00001f && verticalVelocity <= 0.0f &&
                    !swimming && !isFlightAirborne()) {
                    float normalZ = 0.0f;
                    const auto support = m2Renderer->getFloorHeight(
                        adjusted.x, adjusted.y, adjusted.z + 0.01f, &normalZ);
                    if (support && movement::isWalkableNormal(normalZ) &&
                        std::abs(*support - adjusted.z) <= 0.02f) {
                        grounded = true;
                        verticalVelocity = 0.0f;
                    }
                }
                if (adjusted.z < candidate.z - 0.00001f && verticalVelocity > 0.0f) {
                    verticalVelocity = 0.0f;
                }
                candidate = adjusted;
            }
        }

        stepPos = candidate;
    }
    return stepPos;
}

// The camera that orbits a character and moves it.
//
// Collision, grounding, swimming, flight, the zoom, and the pullback that keeps
// the camera out of walls. This was the larger half of a two-thousand-line
// update(), and everything it needs from the half before it arrives in `f`.
// Where the followed character wants to be this frame.
//
// The movement intent - walking, swimming, flying, jumping, falling - and the
// swept wall collision that keeps it out of geometry. Answers the position it
// arrived at; the floor has not been consulted yet.
void CameraController::setFlyingActive(bool active) {
    // Said when it changes, as take-off and landing are: whether space climbs
    // or jumps comes down to this one flag from the server, and a report that
    // the mount "will not fly" is otherwise a guess at whether it was sent.
    if (active != flyingActive_) {
        LOG_WARNING("Flight: ", active ? "the server allows flying here"
                                       : "flying is no longer allowed");
    }
    flyingActive_ = active;
    if (!active) flightAirborne_ = false;
}

glm::vec3 CameraController::moveFollowedCharacter(float /*deltaTime*/, FrameInput& f,
                                                  glm::vec3& prevTargetPos) {
    // Move the follow target (character position) instead of the camera
    prevTargetPos = *followTarget;
    stepFromRest_ = stepFinishedThisFrame_ && !grounded && verticalVelocity <= 0.0f;
    stepFinishedThisFrame_ = false;
    glm::vec3 targetPos = *followTarget;
    if (swimming || flightAirborne_ || externalFollow_) preserveAirborneMomentum_ = false;
    if (!externalFollow_) {
        if (wmoRenderer) {
            wmoRenderer->setCollisionFocus(targetPos, COLLISION_FOCUS_RADIUS_THIRD_PERSON);
        }
        if (m2Renderer) {
            m2Renderer->setCollisionFocus(targetPos, COLLISION_FOCUS_RADIUS_THIRD_PERSON);
        }
    }

    if (!externalFollow_) {
        // Enter swim only when water is deep enough (waist-deep+),
        // not for shallow wading.
        std::optional<float> waterH;
        if (waterRenderer) {
            waterH = waterRenderer->getNearestWaterHeightAt(targetPos.x, targetPos.y, targetPos.z, 400.0f);
        }
        constexpr float MAX_SWIM_DEPTH_FROM_SURFACE = 12.0f;
        // Hysteresis: starting to swim needs deeper water than continuing to
        // swim does. With one threshold, a character wading at the boundary
        // flipped between swim and walk every frame - each flip restarts the
        // locomotion animation and sends a START_SWIM/STOP_SWIM pair, which
        // is what made walking out of water stutter.
        // Entering takes the character's own swimming depth: native 2.4.3
        // (human male) stands without MOVEFLAG_SWIMMING on floors 0.05-1.3 yd
        // under Blackfathom MLIQ water and sends START_SWIM 1.58 yd under it
        // (_experiments/20261117T174000Z.bfd_liquid_pair7). Exit is unmeasured.
        const float SWIM_ENTER_WATER_DEPTH = swimSurfaceOffset_;
        constexpr float SWIM_EXIT_WATER_DEPTH  = 0.85f;
        const float MIN_SWIM_WATER_DEPTH =
            swimming ? SWIM_EXIT_WATER_DEPTH : SWIM_ENTER_WATER_DEPTH;
        bool inWater = false;
        // Water Walk: treat water surface as ground - player walks on top, not through.
        if (waterWalkActive_ && waterH && targetPos.z >= *waterH - 0.5f) {
            // Clamp to water surface so the player stands on it
            targetPos.z = *waterH;
            verticalVelocity = 0.0f;
            grounded = true;
            inWater = false;
        } else if (waterH && targetPos.z < *waterH) {
            std::optional<uint16_t> waterType;
            if (waterRenderer) {
                waterType = waterRenderer->getWaterTypeAt(targetPos.x, targetPos.y);
            }
            bool isOcean = false;
            if (waterType && *waterType != 0) {
                isOcean = (((*waterType - 1) % 4) == 1);
            }
            // Depth gate only applies when ENTERING swim (e.g. don't start
            // swimming in a dry cave beneath a lake's water plane). Once
            // swimming, any depth is fine - you can only get deep by
            // deliberately diving through the water column.
            bool depthAllowed = swimming || isOcean ||
                                ((*waterH - targetPos.z) <= MAX_SWIM_DEPTH_FROM_SURFACE);
            if (depthAllowed) {
                std::optional<float> terrainH;
                std::optional<float> wmoH;
                std::optional<float> m2H;
                if (terrainManager)
                    terrainH = liquidBedTerrain(terrainManager->getHeightAt(targetPos.x, targetPos.y),
                                                targetPos.z + 2.0f);
                if (wmoRenderer) wmoH = wmoRenderer->getFloorHeight(targetPos.x, targetPos.y, targetPos.z + 2.0f);
                if (m2Renderer) m2H = m2Renderer->getFloorHeight(targetPos.x, targetPos.y, targetPos.z + 1.0f);
                auto floorH = selectHighestFloor(terrainH, wmoH, m2H);

                // Prefer measured depth from floor; if floor sample is missing,
                // fall back to feet-to-surface depth.
                float depthFromFeet = (*waterH - targetPos.z);
                inWater = (floorH && ((*waterH - *floorH) >= MIN_SWIM_WATER_DEPTH)) ||
                          (!floorH && (depthFromFeet >= MIN_SWIM_WATER_DEPTH));

                // Ramp exit assist: when swimming forward near the surface toward a
                // reachable floor (dock/shore ramp), switch to walking sooner.
                if (swimming && inWater && floorH && f.nowForward) {
                    float floorDelta = *floorH - targetPos.z;
                    float waterOverFloor = *waterH - *floorH;
                    // Measured from where a swimmer actually floats, not from
                    // a bare number.
                    //
                    // depthFromFeet is swimSurfaceOffset_ exactly while
                    // swimming - the feet are pinned that far under the
                    // surface - so this constant was really "the float depth
                    // plus a little". It was 1.45 against a float depth of 0.9.
                    // Raising the float depth to 1.45 so the character treads at
                    // chest height put the test exactly on its own threshold,
                    // and the assist that walks a swimmer up a shore stopped
                    // firing: the character stayed swimming in ankle-deep water,
                    // shuddering on the boundary, and never fully left it.
                    bool nearSurface = depthFromFeet <= swimSurfaceOffset_ + 0.55f;
                    bool reachableRamp = (floorDelta >= -0.30f && floorDelta <= 1.10f);
                    bool shallowRampWater = waterOverFloor <= 1.55f;
                    bool notDiving = f.swimForward.z > -0.20f && !f.xDown;
                    if (nearSurface && reachableRamp && shallowRampWater && notDiving) {
                        inWater = false;
                    }
                }

                // Forward plank/ramp assist: sample structure floors ahead so water exit
                // can happen when the ramp is in front of us (not only under our feet).
                if (swimming && inWater && f.nowForward && f.swimForward.z > -0.20f && !f.xDown) {
                    auto queryFloorAt = [&](float x, float y, float probeZ) -> std::optional<float> {
                        std::optional<float> best;
                        if (terrainManager) {
                            best = terrainManager->getHeightAt(x, y);
                        }
                        if (wmoRenderer) {
                            float nz = 1.0f;
                            auto wh = wmoRenderer->getFloorHeight(x, y, probeZ, &nz);
                            if (wh && nz >= 0.40f && (!best || *wh > *best)) best = wh;
                        }
                        if (m2Renderer && !externalFollow_) {
                            float nz = 1.0f;
                            auto mh = m2Renderer->getFloorHeight(x, y, probeZ, &nz);
                            if (mh && nz >= 0.35f && (!best || *mh > *best)) best = mh;
                        }
                        return best;
                    };

                    glm::vec2 fwd2(f.forward.x, f.forward.y);
                    float fwdLenSq = glm::dot(fwd2, fwd2);
                    if (fwdLenSq > 1e-8f) {
                        fwd2 *= glm::inversesqrt(fwdLenSq);
                        std::optional<float> aheadFloor;
                        const float probeZ = targetPos.z + 2.0f;
                        const float dists[] = {0.45f, 0.90f, 1.25f};
                        for (float d : dists) {
                            float sx = targetPos.x + fwd2.x * d;
                            float sy = targetPos.y + fwd2.y * d;
                            auto h = queryFloorAt(sx, sy, probeZ);
                            if (h && (!aheadFloor || *h > *aheadFloor)) aheadFloor = h;
                        }

                        if (aheadFloor) {
                            float floorDelta = *aheadFloor - targetPos.z;
                            float waterOverFloor = *waterH - *aheadFloor;
                            bool nearSurface = depthFromFeet <= 1.65f;
                            bool reachableRamp = (floorDelta >= -0.35f && floorDelta <= 1.25f);
                            bool shallowRampWater = waterOverFloor <= 1.75f;
                            if (nearSurface && reachableRamp && shallowRampWater) {
                                inWater = false;
                            }
                        }
                    }
                }
            }
        }
        // Keep swimming through water-data gaps at chunk boundaries.
        if (!inWater && swimming && !waterH) {
            inWater = true;
        }

        // Space applies one upward impulse, including below the surface.
        // Underwater ascent stops at the apex; a surface jump completes its
        // arc before returning to swimming. Holding Space cannot retrigger it.
        if (waterJumpAirborne_ && waterH && targetPos.z >= *waterH - swimSurfaceOffset_)
            waterJumpCrossedSurface_ = true;
        if (waterJumpAirborne_ &&
            (grounded || !waterH || (verticalVelocity <= 0.0f &&
             targetPos.z <= *waterH - swimSurfaceOffset_))) {
            if (!grounded && waterH) {
                if (waterJumpCrossedSurface_) targetPos.z = *waterH - swimSurfaceOffset_;
                verticalVelocity = 0.0f;
            }
            waterJumpAirborne_ = false;
        }
        const bool startWaterJump = inWater && waterH && f.nowJump && !mounted_;
        if (startWaterJump) {
            waterJumpAirborne_ = true;
            waterJumpCrossedSurface_ = targetPos.z >= *waterH - swimSurfaceOffset_;
        }
        if (waterJumpAirborne_) inWater = false;

        if (inWater) {
        swimming = true;
        // Swim movement follows look pitch (f.forward/back), while strafe stays
        // lateral for stable control.
        float swimSpeed = (swimSpeedOverride_ > 0.0f && swimSpeedOverride_ < 100.0f && !std::isnan(swimSpeedOverride_))
                              ? swimSpeedOverride_ : f.speed * SWIM_SPEED_FACTOR;
        float waterSurfaceZ = waterH ? (*waterH - swimSurfaceOffset_) : targetPos.z;
        // Where the swimmer stood before this step; the surface clamp below can
        // lower the feet under a shallow floor they were resting on.
        const float swimStartZ = targetPos.z;

        const glm::vec3 swimForward = f.swimForward;
        // Use character's facing direction for strafe, not camera's right vector
        glm::vec3 swimRight = f.right;  // Character's right (horizontal facing), not camera's

        float swimBackSpeed = (swimBackSpeedOverride_ > 0.0f && swimBackSpeedOverride_ < 100.0f
                                && !std::isnan(swimBackSpeedOverride_))
                                   ? swimBackSpeedOverride_ : swimSpeed * 0.5f;

        glm::vec3 swimMove(0.0f);
        if (f.nowForward) swimMove += swimForward;
        if (f.nowBackward) swimMove -= swimForward;
        if (f.nowStrafeLeft) swimMove += swimRight;
        if (f.nowStrafeRight) swimMove -= swimRight;

        float swimMoveLenSq = glm::dot(swimMove, swimMove);
        if (swimMoveLenSq > 1e-6f) {
            swimMove *= glm::inversesqrt(swimMoveLenSq);
            // Use backward swim speed when moving backwards only (not when combining with strafe)
            float applySpeed = (f.nowBackward && !f.nowForward) ? swimBackSpeed : swimSpeed;
            targetPos += swimMove * applySpeed * f.physicsDeltaTime;
        }

        // Space's upward impulse is handled above. X swims down continuously
        // at the server's swim speed, rather than an unrelated buoyancy speed.
        bool diveKey = f.xDown;
        bool diveIntent = diveKey || (f.nowForward && (swimForward.z < -0.28f));
        if (diveKey) {
            verticalVelocity = -swimSpeed;
        } else {
            // No vertical key held. Retail holds depth here rather than
            // pulling the swimmer back to the surface: a character can idle
            // underwater and drown doing it, which a surface spring would
            // make impossible. The old behaviour yanked you up unless you
            // were actively diving, so a dive only held while a key was
            // down. Bleed off whatever vertical momentum was carried in and
            // then hover.
            verticalVelocity *= std::max(0.0f, 1.0f - SWIM_VERTICAL_DAMPING * f.physicsDeltaTime);
            if (std::abs(verticalVelocity) < 0.05f) verticalVelocity = 0.0f;

            // Near the surface a body does float up to it, which is what
            // keeps a swimmer's head out of the water instead of drifting
            // just below. Only within a short band, and not while the look
            // direction says the player is diving.
            const float surfaceErr = waterSurfaceZ - targetPos.z;
            if (!diveIntent && surfaceErr > 0.0f && surfaceErr < SWIM_FLOAT_BAND) {
                verticalVelocity += surfaceErr * 6.0f * f.physicsDeltaTime;
                if (surfaceErr < 0.06f && std::abs(verticalVelocity) < 0.35f) {
                    verticalVelocity = 0.0f;
                }
            }
        }

        targetPos.z += verticalVelocity * f.physicsDeltaTime;

        // Don't rise above water surface
        if (waterH && targetPos.z > *waterH - swimSurfaceOffset_) {
            targetPos.z = *waterH - swimSurfaceOffset_;
            if (verticalVelocity > 0.0f) verticalVelocity = 0.0f;
        }

        // Prevent sinking/clipping through world floor while swimming.
        // Cache floor queries (update every 3 frames or 1 unit f.movement)
        std::optional<float> floorH;
        float dx2D = targetPos.x - lastFloorQueryPos.x;
        float dy2D = targetPos.y - lastFloorQueryPos.y;
        float dist2DSq = dx2D * dx2D + dy2D * dy2D;
        constexpr float kFloorDistSq = FLOOR_QUERY_DISTANCE_THRESHOLD * FLOOR_QUERY_DISTANCE_THRESHOLD;
        bool updateFloorCache = (floorQueryFrameCounter++ >= FLOOR_QUERY_FRAME_INTERVAL) ||
                                 (dist2DSq > kFloorDistSq);

        if (updateFloorCache) {
            floorQueryFrameCounter = 0;
            lastFloorQueryPos = targetPos;
            constexpr float MAX_SWIM_FLOOR_ABOVE_FEET = 0.25f;
            constexpr float MIN_SWIM_CEILING_ABOVE_FEET = 0.30f;
            constexpr float MAX_SWIM_CEILING_ABOVE_FEET = 1.80f;
            std::optional<float> ceilingH;
            auto considerFloor = [&](const std::optional<float>& h) {
                if (!h) return;
                // Swim-floor guard: only accept surfaces at or very slightly above feet.
                if (*h <= std::max(targetPos.z, swimStartZ) + MAX_SWIM_FLOOR_ABOVE_FEET) {
                    if (!floorH || *h > *floorH) floorH = h;
                }
                // Swim-ceiling guard: detect structures just above feet so upward swim
                // can't clip through docks/platform undersides.
                float dz = *h - targetPos.z;
                if (dz >= MIN_SWIM_CEILING_ABOVE_FEET && dz <= MAX_SWIM_CEILING_ABOVE_FEET) {
                    if (!ceilingH || *h < *ceilingH) ceilingH = h;
                }
            };

            if (terrainManager) {
                considerFloor(terrainManager->getHeightAt(targetPos.x, targetPos.y));
            }
            if (wmoRenderer) {
                auto wh = wmoRenderer->getFloorHeight(targetPos.x, targetPos.y, targetPos.z + 2.0f);
                considerFloor(wh);
            }
            if (m2Renderer && !externalFollow_) {
                auto mh = m2Renderer->getFloorHeight(targetPos.x, targetPos.y, targetPos.z + 2.0f);
                considerFloor(mh);
            }

            // Terrain is not a surface a swimmer can be underneath.
            //
            // considerFloor only takes a floor at or just above the feet, so a
            // dive can pass under a dock rather than being held on top of it.
            // Terrain has no underside to pass under, and once the feet are
            // below the heightmap its sample is far above them: the guard drops
            // it, no floor is left, and nothing pushes back. One fast frame
            // through the surface and the swimmer is under the map for good.
            //
            // A structure floor found beneath the feet still owns the vertical,
            // so a cave or an interior that genuinely sits below the heightmap
            // keeps swimming as it did.
            if (terrainManager && !floorH) {
                auto th = terrainManager->getHeightAt(targetPos.x, targetPos.y);
                if (th && *th > targetPos.z) floorH = th;
            }

            if (ceilingH && verticalVelocity > 0.0f) {
                float ceilingLimit = *ceilingH - 0.35f;
                if (targetPos.z > ceilingLimit) {
                    targetPos.z = ceilingLimit;
                    verticalVelocity = 0.0f;
                }
            }

            cachedFloorHeight = floorH;
        } else {
            floorH = cachedFloorHeight;
        }
        if (floorH) {
            // The floor itself bounds a swimmer. Native 2.4.3 swims 1.58-1.59
            // yd under the Aku'mai pool surface over floors 1.5 and 2.0 yd deep;
            // the former 0.5 yd lift held WoWee 0.6 yd higher there.
            float swimFloor = *floorH;
            if (targetPos.z < swimFloor) {
                targetPos.z = swimFloor;
                if (verticalVelocity < 0.0f) verticalVelocity = 0.0f;
            }
        }

        // Enforce collision while swimming too (horizontal only), skip when stationary.
        {
            // Horizontal only: the swim clamp owns the vertical.
            glm::vec3 stepPos = sweepAgainstWalls(*followTarget, targetPos, true);

            // Terrain is a wall to a swimmer, not only a floor.
            //
            // sweepAgainstWalls knows WMO walls and doodad collision, and a
            // shoreline is neither: it is the heightmap, which the swim path
            // only ever consulted downward, for something to float above. So
            // nothing stopped a swimmer crossing into a hillside - and the
            // floor probe above will not push them out of it either, because
            // it takes a floor only at or just above the feet and a cliff face
            // ahead is far above them. Leaving the water inside the hill then
            // drops them through the world.
            //
            // Only a rise the swimmer could not float over counts. The clamp
            // holds them half a yard above the floor, so a beach shelving up
            // under them stays passable and they swim ashore as before.
            constexpr float SWIM_TERRAIN_WALL_CLEARANCE = 0.9f;
            if (terrainManager) {
                const SwimStep allowed = swimStepAgainstTerrain(
                    {followTarget->x, followTarget->y}, {stepPos.x, stepPos.y},
                    targetPos.z + SWIM_TERRAIN_WALL_CLEARANCE,
                    [&](float x, float y) { return terrainManager->getHeightAt(x, y); });
                stepPos.x = allowed.x;
                stepPos.y = allowed.y;
            }

            targetPos.x = stepPos.x;
            targetPos.y = stepPos.y;
        }

        grounded = false;
        } else {
        // Exiting water - boost upward to help climb onto shore/stairs.
        if (swimming) {
            // Anchor lastGroundZ to current position so WMO floor probes
            // start from a sensible height instead of stale pre-swim values.
            lastGroundZ = targetPos.z;
            grounded = true;  // Treat as grounded so step-up budget is full
            // Small upward boost to clear stair lip geometry
            if (verticalVelocity < 1.5f) {
                verticalVelocity = 1.5f;
            }
        }
        swimming = false;

        // Taking off: space, on something allowed to fly. It climbs from the
        // first frame rather than jumping first, which is what the key means
        // once a flying mount is under the player.
        if (flyingActive_ && !flightAirborne_ && f.spaceHeld) {
            flightAirborne_ = true;
            LOG_WARNING("Flight: took off");
        }

        // Player-controlled flight (flying mount / druid Flight Form):
        // Use 3D pitch-following movement with no gravity or grounding.
        if (flyingActive_ && flightAirborne_) {
            grounded = true;  // suppress fall-damage checks
            verticalVelocity = 0.0f;
            jumpBufferTimer = 0.0f;
            coyoteTimer = 0.0f;

            // Forward/back along the mount's facing at the flight pitch - see
            // flightForward. The camera's own look direction is not it.
            glm::vec3 flyFwd = f.flightForward;
            if (glm::dot(flyFwd, flyFwd) < 1e-8f) flyFwd = f.forward;
            const glm::vec3 flightFrom = targetPos;
            glm::vec3 flyMove(0.0f);
            if (f.nowForward)     flyMove += flyFwd;
            if (f.nowBackward)    flyMove -= flyFwd;
            if (f.nowStrafeLeft)  flyMove += f.right;
            if (f.nowStrafeRight) flyMove -= f.right;
            // Space = ascend, X = descend while airborne (works on foot too,
            // e.g. .gm fly, not just on a flying mount).
            bool flyDescend = !f.uiWantsKeyboard && f.xDown;
            if (f.spaceHeld)     flyMove.z += 1.0f;
            if (flyDescend)      flyMove.z -= 1.0f;
            float flyMoveLenSq = glm::dot(flyMove, flyMove);
            if (flyMoveLenSq > 1e-6f) {
                flyMove *= glm::inversesqrt(flyMoveLenSq);
                float flyFwdSpeed = (flightSpeedOverride_ > 0.0f && flightSpeedOverride_ < 200.0f
                                     && !std::isnan(flightSpeedOverride_))
                                        ? flightSpeedOverride_ : f.speed;
                float flyBackSpeed = (flightBackSpeedOverride_ > 0.0f && flightBackSpeedOverride_ < 200.0f
                                      && !std::isnan(flightBackSpeedOverride_))
                                         ? flightBackSpeedOverride_ : flyFwdSpeed * 0.5f;
                float flySpeed = (f.nowBackward && !f.nowForward) ? flyBackSpeed : flyFwdSpeed;
                targetPos += flyMove * flySpeed * f.physicsDeltaTime;
            }
            targetPos.z += verticalVelocity * f.physicsDeltaTime;

            // The ground holds a flying mount up - see flightStepAgainstGround.
            // Not where a WMO floor lies at the feet under the heightfield: that
            // is a tunnel burrowing under unholed ground, flown into, not over.
            if (terrainManager) {
                // Half a heightfield cell, as the slope reading in
                // sampleFloorUnderFeet uses, for the same reason.
                constexpr float kGroundNormalSpacing = 2.0f;
                const glm::vec3 slid = movement::flightStepAgainstGround(
                    [this](float x, float y) -> std::optional<float> {
                        if (terrainManager->isHoleAt(x, y)) return std::nullopt;
                        return terrainManager->getHeightAt(x, y);
                    },
                    flightFrom, targetPos, kGroundNormalSpacing);
                if (slid != targetPos &&
                    !(wmoRenderer && wmoRenderer->getFloorHeight(
                          targetPos.x, targetPos.y, targetPos.z + 1.5f))) {
                    targetPos = slid;
                }
            }
            // Skip all ground physics - go straight to collision/WMO sections
        } else {

        const float moveLenSq = glm::dot(f.movement, f.movement);
        if (moveLenSq > 1e-6f) f.movement *= glm::inversesqrt(moveLenSq);
        if (grounded) {
            // Running jumps retain takeoff velocity even when facing, input,
            // or movement speed changes in flight. Stationary jumps have a
            // distinct drift mode and do not acquire running-jump momentum.
            const float takeoffSpeed = startWaterJump
                ? (swimSpeedOverride_ > 0.0f ? swimSpeedOverride_ : f.speed * SWIM_SPEED_FACTOR)
                : f.speed;
            airborneHorizontalVelocity_ = f.movement * takeoffSpeed;
            preserveAirborneMomentum_ = moveLenSq > 1e-6f;
            airborneMomentumStrafe_ = (f.nowStrafeLeft || f.nowStrafeRight) && !f.nowBackward;
        } else if (!preserveAirborneMomentum_ && moveLenSq > 1e-6f && !knockbackActive_) {
            // Airborne without momentum (a standing jump): the first direction
            // pressed drifts the character at a fixed speed in that direction
            // until landing, held or released, and later keys do not change
            // it. Native: 2.50 yards/s forward, backward or sideways, in run
            // and walk mode alike (horizontalSpeed 2.5 in its packets).
            airborneHorizontalVelocity_ = f.movement * kAirborneDriftSpeed;
            preserveAirborneMomentum_ = true;
            airborneMomentumStrafe_ = (f.nowStrafeLeft || f.nowStrafeRight) && !f.nowBackward;
        }
        const glm::vec3 horizontalVelocity =
            (!grounded || startWaterJump) && preserveAirborneMomentum_ && !knockbackActive_
                ? airborneHorizontalVelocity_ : f.movement * f.speed;
        targetPos += horizontalVelocity * f.physicsDeltaTime;

        // Apply server-driven knockback horizontal velocity (decays over time).
        if (knockbackActive_) {
            targetPos.x += knockbackHorizVel_.x * f.physicsDeltaTime;
            targetPos.y += knockbackHorizVel_.y * f.physicsDeltaTime;
            // Exponential drag: reduce each frame so the player decelerates naturally.
            float drag = std::exp(-KNOCKBACK_HORIZ_DRAG * f.physicsDeltaTime);
            knockbackHorizVel_ *= drag;
            // Once negligible, clear the flag so collision/grounding work normally.
            if (glm::dot(knockbackHorizVel_, knockbackHorizVel_) < 0.0025f) {
                knockbackActive_ = false;
                knockbackHorizVel_ = glm::vec2(0.0f);
            }
        }

        // Jump with input buffering and coyote time
        if (f.nowJump) jumpBufferTimer = JUMP_BUFFER_TIME;
        if (grounded) coyoteTimer = COYOTE_TIME;

        // Mounted, jump is the same jump on the move; standing still the
        // mount rears instead (AnimationController, MountSpecial) and stays
        // on the ground, as on native 2.4.3.
        bool canJump = (coyoteTimer > 0.0f) && (jumpBufferTimer > 0.0f) &&
                       !(mounted_ && moveLenSq <= 1e-6f);
        if (canJump) {
            f.startedJump = true;
            verticalVelocity = startWaterJump ? WATER_JUMP_VELOCITY : f.jumpVel;
            grounded = false;
            jumpBufferTimer = 0.0f;
            coyoteTimer = 0.0f;
        }

        jumpBufferTimer -= f.physicsDeltaTime;
        coyoteTimer -= f.physicsDeltaTime;

        // Apply gravity (skip when server has disabled f.gravity, e.g. Levitate spell)
        //
        // ...and skip it while the ground has not arrived. An unloaded tile and
        // a hole both answer nothing when asked for a height, and only one of
        // them should be fallen through. Entering the world is faster than
        // streaming the tile under it on a slow device, so without this the
        // character falls from the spawn point until the server kills it.
        if (seatedInChair_) {
            // In a chair, and the chair's height is the server's to decide.
            //
            // Gravity ran anyway: the character fell off the seat toward the
            // floor, the server put them back, and they fell again. In the
            // Undercity barber shop that is a bob of about a yard - the seat
            // is at -42.07, the floor under it at -43.02, and the server holds
            // the character at -42.73 between them. Reported as the view
            // bobbing up and down in the chair; it is the character bobbing,
            // and the camera only following.
            //
            // Grounding already stands down for a seat. Falling had to as
            // well, or standing the one down just leaves the other to do it.
            verticalVelocity = 0.0f;
        } else if (groundNotStreamedYet(targetPos.x, targetPos.y)) {
            verticalVelocity = 0.0f;
        } else if (gravityDisabled_) {
            // Float in place: bleed off any downward velocity, allow upward to decay slowly
            if (verticalVelocity < 0.0f) verticalVelocity = 0.0f;
            else verticalVelocity *= std::max(0.0f, 1.0f - 3.0f * f.physicsDeltaTime);
        } else {
            // Constant-acceleration position matches the arc reconstructed by
            // native clients, independent of the number of physics substeps.
            targetPos.z -= 0.5f * f.gravity * f.physicsDeltaTime * f.physicsDeltaTime;
            verticalVelocity += f.gravity * f.physicsDeltaTime;
            // Feather Fall / Slow Fall: cap downward terminal velocity to ~2 m/s
            if (featherFallActive_ && verticalVelocity < -2.0f)
                verticalVelocity = -2.0f;
        }
        targetPos.z += verticalVelocity * f.physicsDeltaTime;
        // Resolve re-entry in the step that crosses the swimming height.
        // Waiting until the next step exposes one frame below the surface
        // and lets the camera follow that overshoot before the clamp runs.
        if (waterJumpAirborne_ && waterH && verticalVelocity <= 0.0f &&
            targetPos.z <= *waterH - swimSurfaceOffset_) {
            if (waterJumpCrossedSurface_) targetPos.z = *waterH - swimSurfaceOffset_;
            verticalVelocity = 0.0f;
            waterJumpAirborne_ = false;
            swimming = true;
            grounded = false;
        }
        } // end ground physics (not airborne in flight)
        } // end !inWater
    } else {
        // External follow (e.g., taxi): trust server position without grounding.
        swimming = false;
        grounded = true;
        verticalVelocity = 0.0f;
    }

    // Refresh inside-WMO state before collision/grounding so we don't use stale
    // terrain-first caches while entering enclosed tunnel/building spaces.
    if (wmoRenderer && !externalFollow_) {
        glm::vec3 insideDelta = targetPos - lastInsideStateCheckPos_;
        float insideDistSq = glm::dot(insideDelta, insideDelta);
        if (++insideStateCheckCounter_ >= 2 || insideDistSq > 0.1225f) {
            insideStateCheckCounter_ = 0;
            lastInsideStateCheckPos_ = targetPos;

            bool prevInside = cachedInsideWMO;
            bool prevInsideInterior = cachedInsideInteriorWMO;
            cachedInsideWMO = wmoRenderer->isInsideWMO(targetPos.x, targetPos.y, targetPos.z + 1.0f, nullptr);
            cachedInsideInteriorWMO = cachedInsideWMO &&
                wmoRenderer->isInsideInteriorWMO(targetPos.x, targetPos.y, targetPos.z + 1.0f);
            if (cachedInsideWMO != prevInside || cachedInsideInteriorWMO != prevInsideInterior) {
                hasCachedFloor_ = false;
                hasCachedCamFloor = false;
                cachedPivotLift_ = 0.0f;
            }
        }
    }

    // Sweep collisions in small steps to reduce tunneling through thin walls/floors.
    // Skip entirely when stationary to avoid wasting collision calls.
    // Use tighter steps when inside WMO for more precise collision.
    if (terrainManager && !externalFollow_ && !swimming && !isFlightAirborne() && !ignoreSlopeLimit_) {
        const auto terrainHeight = [this](float x, float y) -> std::optional<float> {
            if (terrainManager->isHoleAt(x, y)) return std::nullopt;
            return terrainManager->getHeightAt(x, y);
        };
        const auto terrainNormal = [this](float x, float y) {
            glm::vec3 normal(0.0f, 0.0f, 1.0f);
            if (!terrainManager->getHeightAt(x, y, &normal)) {
                LOG_ERROR("Terrain normal requested outside loaded terrain: ", x, ", ", y);
                throw std::logic_error("Terrain normal requires loaded terrain");
            }
            return normal;
        };
        if (terrainHeight(targetPos.x, targetPos.y)) {
            targetPos = movement::walkStepAgainstGround(terrainHeight, *followTarget,
                targetPos, terrainNormal(targetPos.x, targetPos.y));
        }
        const auto height = terrainHeight(followTarget->x, followTarget->y);
        if (height && std::abs(followTarget->z - *height) < 0.25f &&
            !movement::isWalkableNormal(terrainNormal(followTarget->x, followTarget->y).z)) {
            targetPos = movement::surfaceStepAgainstGround(terrainHeight,
                *followTarget, targetPos, terrainNormal);
        }
    }
    targetPos = sweepAgainstWalls(*followTarget, targetPos, true);

    return targetPos;
}

// Put the character on the floor, and publish where it ended up.
//
// The expensive half: terrain, WMO and doodad floor queries, the step-up
// budget, the slope limit, the cache that skips all of it when barely moving.
// Then the follow target is written, and a fall through the world is caught.
// What the floor queries answer directly under the character.
//
// The expensive part of grounding: three renderers asked where their surface
// is, two of them on other threads, and a cache that skips all of it when the
// character has barely moved. The three centre samples come back with the
// chosen floor because the recovery probes above need to know which surfaces
// existed at all, not just which one won.
CameraController::FloorSample CameraController::sampleFloorUnderFeet(const glm::vec3& targetPos,
                                                                     float stepUpBudget) {
    std::optional<float> groundH;
    std::optional<float> centerTerrainH;
    std::optional<float> centerWmoH;
    std::optional<float> centerM2H;
    // Collision cache: skip expensive checks if barely moved (15cm threshold)
    float dmx = targetPos.x - lastCollisionCheckPos_.x;
    float dmy = targetPos.y - lastCollisionCheckPos_.y;
    float distMovedSq = dmx * dmx + dmy * dmy;
    constexpr float kCollisionCacheDistSq = COLLISION_CACHE_DISTANCE * COLLISION_CACHE_DISTANCE;
    bool useCached = grounded && hasCachedFloor_ && distMovedSq < kCollisionCacheDistSq;
    if (useCached) {
        // Never trust cached ground while actively descending or when
        // vertical drift from cached floor is meaningful.
        float dzCached = std::abs(targetPos.z - cachedFloorHeight_);
        if (verticalVelocity < -0.4f || dzCached > 0.35f) {
            useCached = false;
        }
    }

    if (useCached) {
        groundH = cachedFloorHeight_;
    } else {
        // Full collision check - run terrain/WMO/M2 queries in parallel
        std::optional<float> terrainH;
        std::optional<float> wmoH;
        std::optional<float> m2H;
        // When airborne, anchor probe to last ground level so the
        // ceiling doesn't rise with the jump and catch roof geometry.
        float wmoBaseZ = grounded ? std::max(targetPos.z, lastGroundZ) : lastGroundZ;
        float wmoProbeZ = wmoBaseZ + stepUpBudget + 0.5f;
        float wmoNormalZ = 1.0f;

        // Launch WMO + M2 floor queries asynchronously while terrain runs on this thread.
        // Collision scratch buffers are thread_local so concurrent calls are safe.
        using FloorResult = std::pair<std::optional<float>, float>;
        std::future<FloorResult> wmoFuture;
        std::future<FloorResult> m2Future;
        bool wmoAsync = false, m2Async = false;
        float px = targetPos.x, py = targetPos.y;
        if (wmoRenderer) {
            wmoAsync = true;
            // Closest to the feet, not the highest below the probe.
            //
            // Under an overhang - the doorways and portals of
            // Undercity - the floor of the level above sits about a
            // metre over the one you stand on, both inside the
            // step-up window. The default highest-floor pick snapped
            // the player up to the level above even standing still,
            // the server pulled them back to the real floor, and the
            // two fought once a second. Anchoring the pick to the
            // player's own ground keeps them where they are; a real
            // step onto a ledge still wins once the feet are level
            // with it.
            //
            // The reference is the feet themselves, NOT wmoBaseZ:
            // wmoBaseZ is max(z, lastGroundZ) so the probe can reach
            // up for a step, but feeding that same raised value back
            // as the arbitration anchor ratchets the pick upward -
            // one stray upper-floor frame lifts lastGroundZ, and the
            // reference then stays on the level above even after the
            // server drags the player back down, which is the very
            // yo-yo this pick was meant to stop. Grounded, the true
            // feet are targetPos.z; airborne, aim at the last ground.
            //
            // Except that a player cannot be standing on a floor while
            // their feet are a long way under it, and the flag says
            // grounded anyway. At the Undercity elevator ramp the feet
            // were 2.81 below the landing they had been standing on -
            // still a candidate, at -41.4273 against a last ground of
            // -41.4245 - and 2.39 above the Trade Quarter floor five
            // yards further down. Measured from the feet the lower one
            // is nearer by 0.42, so the pick took it and the player
            // dropped through the ramp.
            //
            // Below the last ground by more than a step is falling,
            // whatever the flag says, so anchor where the airborne case
            // already anchors: the ground they were last on. That floor
            // then wins by 5.2 instead of losing by 0.42. It cannot pull
            // anyone up onto a floor they truly left, because a floor
            // they have walked off is not a candidate at their feet.
            const float feetRef = movement::floorArbitrationAnchor(
                grounded, targetPos.z, lastGroundZ);
            wmoFuture = core::ThreadPool::frameWorkers().submit(
                [this, px, py, wmoProbeZ, feetRef]() -> FloorResult {
                    float nz = 1.0f;
                    auto h = wmoRenderer->getFloorHeight(px, py, wmoProbeZ, &nz, feetRef);
                    return {h, nz};
                });
        }
        if (m2Renderer && !externalFollow_) {
            m2Async = true;
            m2Future = core::ThreadPool::frameWorkers().submit(
                [this, px, py, wmoProbeZ]() -> FloorResult {
                    float nz = 1.0f;
                    auto h = m2Renderer->getFloorHeight(px, py, wmoProbeZ, &nz);
                    return {h, nz};
                });
        }
        if (terrainManager) {
            terrainH = terrainManager->getHeightAt(targetPos.x, targetPos.y);
            // Keep the surface available to grounding. Horizontal movement
            // applies the slope limit before this floor query.

        }
        if (wmoAsync) {
            try { auto [h, nz] = wmoFuture.get(); wmoH = h; wmoNormalZ = nz; }
            catch (const std::exception& e) { LOG_ERROR("WMO floor query: ", e.what()); }
        }
        if (m2Async) {
            try {
                auto [h, nz] = m2Future.get();
                m2H = h;
                if (m2H && !ignoreSlopeLimit_ && nz < MIN_WALKABLE_NORMAL_M2) {
                    m2H = std::nullopt;
                }
            } catch (const std::exception& e) { LOG_ERROR("M2 floor query: ", e.what()); }
        }

        // A tunnel mouth can overlap the outdoor heightfield before the
        // player's eye point is inside the WMO bounds.  Treat a nearby WMO
        // floor below that terrain as transition space immediately; waiting
        // for cachedInsideWMO makes the outdoor terrain win one frame at a
        // time (climbing through the roof), while rejecting a steep entrance
        // ramp here makes the player fall through it.
        bool atTunnelSeam = false;
        if (terrainH && wmoH) {
            const float terrainAboveWmo = *terrainH - *wmoH;
            const float wmoDropFromPlayer = targetPos.z - *wmoH;
            atTunnelSeam = terrainAboveWmo > 1.2f && terrainAboveWmo < 12.0f &&
                           wmoDropFromPlayer >= -0.4f && wmoDropFromPlayer < 1.8f;
        }
        // A real tunnel mouth burrows into rising ground: the heightfield
        // just ahead climbs above head height (or stops in a hole cut for
        // the passage). WMO ramps that merely run beneath flat walkable
        // streets must not steal the player from the terrain above them -
        // that pulled players through the ground at the Stormwind gate
        // ramparts and down ramps into the void. Inside an interior WMO
        // group (tram entrance buildings) the heightfield under the city
        // is meaningless, so it gets no veto - otherwise entry becomes
        // dependent on approach angle.
        if (atTunnelSeam && !cachedInsideInteriorWMO && terrainManager) {
            glm::vec3 moveDir = targetPos - lastCollisionCheckPos_;
            moveDir.z = 0.0f;
            const float moveLen = glm::length(moveDir);
            if (moveLen < 1e-3f) {
                atTunnelSeam = false;  // stationary - nothing to enter
            } else {
                const glm::vec3 aheadPos = targetPos + moveDir * (2.5f / moveLen);
                auto terrainAhead = terrainManager->getHeightAt(aheadPos.x, aheadPos.y);
                atTunnelSeam = !terrainAhead ||
                               *terrainAhead > targetPos.z + 2.2f;
            }
        }

        // Reject steep WMO slopes. Tunnel ramps use the more permissive WMO
        // limit even at the boundary, where isInsideWMO is not reliable yet.
        float minWalkableWmo = (cachedInsideWMO || atTunnelSeam)
            ? MIN_WALKABLE_NORMAL_WMO : MIN_WALKABLE_NORMAL_TERRAIN;
        if (wmoH && !ignoreSlopeLimit_ && wmoNormalZ < minWalkableWmo) {
            wmoH = std::nullopt;  // Treat as unwalkable
        }

        // Reject WMO floors far above last known ground when airborne
        // (prevents snapping to roof/ceiling surfaces during jumps)
        if (wmoH && !grounded && *wmoH > lastGroundZ + stepUpBudget + 0.5f) {
            wmoH = std::nullopt;
            centerWmoH = std::nullopt;
        }
        // An M2 doodad's collision sitting well below a valid WMO
        // floor is BENEATH that floor - a decoration or structural
        // base under the walkway, not a surface to stand on. Letting
        // it win drops the player through the WMO floor: in Undercity
        // the pick took an m2 floor ~6m below the wmo one (feet -48.8,
        // wmo -48.15, m2 -54.23) and fell. When a WMO floor is present
        // here, reject an m2 floor more than 1.5m below it. (If the
        // player were standing ON the m2 platform, the higher WMO
        // floor would be above the probe and not returned, so this
        // cannot strand them off a legitimate lower deck.)
        if (m2H && wmoH && *m2H < *wmoH - 1.5f) {
            m2H = std::nullopt;
        }
        // A terrain hole is the artist saying there is no ground
        // here - it is how cave mouths and sunken entrances get
        // opened up, and the mesh builder already skips those
        // quads, so nothing is drawn over them. getHeightAt does
        // not ask: it interpolates straight across the gap and
        // reports a surface right at the player's feet, which then
        // wins the floor selection against the real floor below.
        // At Gadgetzan's auction house that put the player out over
        // the stairwell on ground that is not there, unable to walk
        // down the steps and looking into a hole they were standing
        // on. Every seam guard below is downstream of this and none
        // of them could see it, because to them the terrain sample
        // was real.
        //
        // Only ever hand the floor to the building underneath - a
        // hole with nothing below it keeps the heightfield it has
        // always had, so this cannot open a pit anywhere new.
        if (terrainH && wmoH && terrainManager &&
            terrainManager->isHoleAt(targetPos.x, targetPos.y)) {
            terrainH = std::nullopt;
        }

        // Inside an interior WMO group - Undercity's halls, a
        // building's rooms - the outdoor heightfield is the roof far
        // overhead, never the floor. Veto it there so a brief gap in
        // the WMO floor query does not kick the player up to the
        // surface. (The seam case is handled above and keeps
        // cachedInsideInteriorWMO false at entrances.)
        //
        // Only when it really is overhead, though. isInsideInteriorWMO
        // is a bounding-box containment test, and an underground WMO's
        // interior box reaches up through the ground above it - so
        // standing on the grass over a cave counted as being inside
        // it. Vetoing terrain there left the WMO as the only
        // candidate, and the nearest WMO surface below is the cave's
        // ceiling: the player dropped through the hillside they were
        // walking on and stood on the roof of the room underneath.
        //
        // Higher than the player could step onto is the test, because
        // that is the whole of what the veto meant: ground you cannot
        // reach is not the ground you are standing on. Undercity's
        // surface sits ~113m over the halls and is still vetoed; the
        // hillside at your feet is not. The rule itself lives in
        // movement_limits.hpp, where it is pinned by a test.
        if (terrainH && movement::terrainIsOverheadRoof(
                cachedInsideInteriorWMO, *terrainH,
                targetPos.z, stepUpBudget)) {
            terrainH = std::nullopt;
        }

        centerTerrainH = terrainH;
        centerWmoH = wmoH;
        centerM2H = m2H;

        // Guard against extremely bad WMO void ramps, but keep normal tunnel
        // transitions valid. Only reject when the WMO sample is implausibly far
        // below terrain and player is not already descending.
        if (terrainH && wmoH && !cachedInsideWMO) {
            float terrainMinusWmo = *terrainH - *wmoH;
            if (terrainMinusWmo > 12.0f && verticalVelocity > -8.0f) {
                wmoH = std::nullopt;
                centerWmoH = std::nullopt;
            }
        }

        if ((cachedInsideWMO || atTunnelSeam) && wmoH) {
            // The terrain wins at a seam unless it is not really there.
            //
            // Two ways it can fail to be. The quad is cut by a hole, which is
            // how the map opens a cave mouth or a below-ground entrance - that
            // is vetoed further up, before any of this, so there is simply no
            // terrain candidate left here. Or it is standing overhead out of
            // reach, which is what the heightfield is once the player is
            // inside a passage burrowing under unholed ground: taking it would
            // lift them back out. See wmoFloorIsWayIn.
            //
            // Anything else is real ground under real feet, and a WMO floor
            // that happens to run beneath it is not a way in. Orgrimmar's
            // valley entrance is the case: its ramp passes about 1.3 yards
            // under the ground beside it, and there is no hole anywhere in
            // that tile.
            const bool preferWmoAtSeam =
                atTunnelSeam &&
                (!terrainH ||
                 movement::wmoFloorIsWayIn(*terrainH, targetPos.z, stepUpBudget));
            if (preferWmoAtSeam) {
                groundH = wmoH;
            } else if (terrainH) {
                // Both real and both reachable: whichever is actually under the
                // feet, rather than oscillating between the ground above and a
                // deep WMO floor below.
                groundH = selectClosestFloor(terrainH, wmoH, targetPos.z);
            } else {
                groundH = selectReachableFloor3(terrainH, wmoH, m2H, targetPos.z, stepUpBudget);
            }
        } else {
            groundH = selectReachableFloor3(terrainH, wmoH, m2H, targetPos.z, stepUpBudget);
        }

        // Nothing to stand on at all, with a building overhead.
        //
        // The jump dump below only fires when a floor is chosen and it is a
        // long way from the last one. Stepping onto an Undercity lift produces
        // no floor whatsoever - the deck is an M2 transport and the shaft has
        // no WMO floor under it - so the player simply falls, and the one dump
        // that would name the deck never ran. UndeadElevator ships 68
        // collision triangles and is not marked skipCollision, so what is
        // wanted here is which test the instance fails, or whether it is a
        // candidate at all.
        if (!groundH && m2Renderer) {
            static std::chrono::steady_clock::time_point lastNoFloorDump{};
            const auto nowNoFloor = std::chrono::steady_clock::now();
            if (nowNoFloor - lastNoFloorDump > std::chrono::seconds(5)) {
                lastNoFloorDump = nowNoFloor;
                // The centre pick's answer, not the final one: the recovery
                // passes in groundFollowedCharacter run after this and often
                // find a floor the one ray missed. Said plainly because the
                // first wording read as "the player is over a void" and is
                // what sent this hunt after the wrong thing.
                LOG_WARNING("Centre floor probe found nothing (feet ", targetPos.z,
                            ") - the recovery passes have not run yet");
                if (floorDiagEnabled()) {
                    if (wmoRenderer) {
                        wmoRenderer->debugDumpGroupsAtPosition(
                            targetPos.x, targetPos.y, targetPos.z);
                    }
                    m2Renderer->debugDumpFloorCandidatesAt(
                        targetPos.x, targetPos.y, targetPos.z);
                }
            }
        }

        // Just arrived, and the buildings have not.
        //
        // On entering a world the heightfield is there before the WMOs are, and
        // over anything that runs underground it lies well above where the
        // player actually is. Logging out in the Undercity entrance tunnel and
        // back in put them on its roof, and once the tunnel loaded its roof was
        // a perfectly good floor to keep them there. Nothing far above the
        // position the server gave is their floor while that is still true.
        //
        // Generous, because the server's Z and the floor under it disagree by a
        // little at the best of times; two yards admits that and refuses forty.
        if (entryFloorAnchorTimer_ > 0.0f && groundH &&
            *groundH > entryFloorAnchorZ_ + 2.0f) {
            groundH = std::nullopt;
        }

        // A storey down, with the floor still under the next step.
        //
        // One ray at the character's centre misses the lip of a surface, and
        // the answer it comes back with is not "no floor" but whatever lies at
        // the bottom of the room - at the top of the Undercity lift that is the
        // shaft floor, ninety-six yards down. groundFollowedCharacter samples a
        // foot cross afterwards and recovers, which is why this reads as almost
        // falling rather than falling, but the frame in between is a dip the
        // player sees.
        //
        // So before handing back a drop of more than a few yards, ask the four
        // points around the feet. Only on that frame, so the hot path still
        // casts one ray. A drop nothing around the feet can account for is a
        // real one and is passed through untouched - walking off a ledge still
        // works.
        if (groundH && grounded && wmoRenderer &&
            *groundH < lastGroundZ - 4.0f && targetPos.z > lastGroundZ - 4.0f) {
            const float lipProbeZ = std::max(targetPos.z, lastGroundZ) + stepUpBudget + 0.6f;
            auto lip = highestWalkableFloor(
                [this](float x, float y, float z, float* nz) {
                    return wmoRenderer->getFloorHeight(x, y, z, nz);
                },
                targetPos.x, targetPos.y, feetCross(0.35f), lipProbeZ,
                MIN_WALKABLE_NORMAL_WMO,
                {.minZ = lastGroundZ - 3.5f, .maxZ = targetPos.z + stepUpBudget});
            if (lip) groundH = lip;
        }

        // The local player's own floor pick, named when it jumps.
        //
        // The movingEntityFloor log covers creatures and other
        // players; this is the reporter's own character in
        // Undercity. It says which floor was chosen for the player
        // and the three candidates it chose among, so a jump that
        // survived the closest-to-feet change shows what is still
        // competing - and whether both floors are even offered here
        // or only one is, which decides whether the fix belongs in
        // the selection or deeper in the WMO query.
        // Threshold sits below a floor gap (~1m) on purpose: the
        // ratchet oscillation the feetRef fix targets stays under a
        // metre frame to frame, so a 1.0 gate saw nothing while the
        // player visibly bobbed. 0.4 catches the sub-metre bob.
        if (groundH && std::abs(*groundH - lastGroundZ) > 0.4f) {
            static std::chrono::steady_clock::time_point lastPlayerFloorLog{};
            const auto now = std::chrono::steady_clock::now();
            if (now - lastPlayerFloorLog > std::chrono::seconds(1)) {
                lastPlayerFloorLog = now;
                core::Logger::getInstance().warning(
                    "Player floor jump: x=", targetPos.x, " y=", targetPos.y,
                    " feet=", targetPos.z,
                    " lastGround=", lastGroundZ, " -> chose ", *groundH,
                    " grounded=", grounded ? 1 : 0,
                    " (terrain=", terrainH ? *terrainH : -99999.0f,
                    " wmo=", wmoH ? *wmoH : -99999.0f,
                    " m2=", m2H ? *m2H : -99999.0f,
                    " seam=", atTunnelSeam ? 1 : 0, ")");
            }
            // A jump - the floor query landed a level away, not a
            // step - is the Undercity pull, and the one thing needed
            // to solve it is which WMO groups and floors exist under
            // the player when it happens. That is exactly what F8's
            // dump prints, so dump it here automatically on a jump
            // (rate-limited hard, it is verbose): the log then carries
            // the answer from ordinary play, with no key to remember.
            // Threshold matches the line above it, not a metre.
            //
            // 0.9 was chosen for a pull that crossed an overhang, and it
            // never fired for the one actually reported in Undercity: the
            // sink there is 0.41, 0.60, 0.56, 0.40 - the thickness of the
            // floor slab, the player dropping onto the ceiling of the room
            // beneath - and every one of those passed under the gate with
            // the jump line printed and no group data behind it. Whatever
            // is worth naming is worth dumping; the five second limiter is
            // what keeps it cheap, not the size of the drop.
            if (wmoRenderer && std::abs(*groundH - lastGroundZ) > 0.35f) {
                static std::chrono::steady_clock::time_point lastFloorDump{};
                if (floorDiagEnabled() && now - lastFloorDump > std::chrono::seconds(5)) {
                    lastFloorDump = now;
                    core::Logger::getInstance().warning(
                        "Player floor jump: dumping WMO groups at the "
                        "pull (feet ", targetPos.z, " -> ", *groundH, ")");
                    wmoRenderer->debugDumpGroupsAtPosition(
                        targetPos.x, targetPos.y, targetPos.z);
                    // And the doodads, which is where the Undercity pull
                    // actually lives: every one of these jumps happens while
                    // the player is attached to a lift, and m2 comes back
                    // empty each time even though UndeadElevator ships 68
                    // collision triangles. The WMO dump alone could never say
                    // why, because the deck it is looking for is not a WMO.
                    if (m2Renderer) {
                        m2Renderer->debugDumpFloorCandidatesAt(
                            targetPos.x, targetPos.y, targetPos.z);
                    }
                }
            }
        }

        // Update cache
        lastCollisionCheckPos_ = targetPos;
        if (groundH) {
            cachedFloorHeight_ = *groundH;
            hasCachedFloor_ = true;
            // Ground found - cancel gravity suspension (WMO floor loaded)
            if (gravitySuspendTimer_ > 0.0f) gravitySuspendTimer_ = 0.0f;
        } else {
            hasCachedFloor_ = false;
        }
    }
    return {.floor = groundH, .terrain = centerTerrainH, .wmo = centerWmoH, .m2 = centerM2H};
}

void CameraController::groundFollowedCharacter(float deltaTime, FrameInput& f,
                                               glm::vec3& targetPos,
                                               const glm::vec3& prevTargetPos) {
    // Seated: the server placed the character, and this pass would place them
    // again from whatever is underneath.
    //
    // A chair is an M2 with collision whose seat is about a metre off the
    // floor and nothing here can tell a seat from a step, so the feet went on
    // top of the chair and the character sat in the air above it. Dropping the
    // chair from the floor candidates was not enough: it only applied to the
    // stand states that name a chair, and a server that answers a chair with a
    // plain SIT - which is what the inn's chairs did - left the whole rule
    // switched off while the pose said otherwise.
    //
    // Nothing drifts while this stands down: movement is blocked for as long
    // as the character is seated, and the moment they stand up the pass runs
    // again from wherever the server left them.
    //
    // One thing is still enforced: the floor. Standing the pass down entirely
    // left nothing to catch a seat that ends up under the ground - a tile that
    // streamed in after the sit, a server seating the character at a Z the
    // floor here does not agree with - and a character below the floor is a
    // character who falls through it when they stand. So the floor under the
    // feet is still sampled and used one way only, upward, and never from the
    // chair's own M2, which is what standing on the furniture was.
    if (sitting) {
        const FloorSample seated = sampleFloorUnderFeet(targetPos, movement::kMaxStepUp);
        const std::optional<float> under = selectReachableFloor3(
            seated.terrain, seated.wmo, std::nullopt, targetPos.z, movement::kMaxStepUp);
        if (under && targetPos.z < *under) {
            targetPos.z = *under;
            lastGroundZ = *under;
            hasRealGround_ = true;
            grounded = true;
            verticalVelocity = 0.0f;
        }
        return;
    }

    // Ground the character to terrain or WMO floor
    // Skip entirely while swimming - the swim floor clamp handles vertical bounds.
    if (!swimming) {
        float stepUpBudget = grounded ? movement::kMaxStepUp : 1.2f;
        // 1. Center-only sample for terrain/WMO floor selection.
        //    Using only the center prevents tunnel entrances from snapping
        //    to terrain when offset samples miss the WMO floor geometry.
        // Slope limit: reject surfaces too steep to walk (prevent clipping).
        // WMO tunnel/bridge ramps are often steeper than outdoor terrain ramps.
        // The floor directly under the feet, and the surfaces that answered.
        FloorSample sample = sampleFloorUnderFeet(targetPos, stepUpBudget);
        std::optional<float> groundH = sample.floor;
        const std::optional<float> centerTerrainH = sample.terrain;
        const std::optional<float> centerWmoH = sample.wmo;
        const std::optional<float> centerM2H = sample.m2;

        // Transition safety: if no reachable floor was selected, choose the higher
        // of terrain/WMO center surfaces when it is still near the player.
        // This avoids dropping into void gaps at terrain<->WMO seams.
        // Whether there is structure here at all, which gates every recovery
        // below it - the multi-sample pass, the wider rescue rings, all of it.
        //
        // It rested on a ray at the character's centre, and a centre ray is the
        // one thing that misses the lip of a surface: at the top of the
        // Undercity lift the landing lies a third of a yard away in +y at the
        // height of the feet, the centre finds nothing, so this said there is
        // no structure nearby and shut off the passes that would have found it.
        // The player steps off the deck and dips into the gap.
        //
        // The hint asks the foot cross now, the same four points the pass it
        // guards would use. It is the last thing standing between the player
        // and a fall, so it should not fail for the reason those passes exist.
        const bool nearWmoSpace = cachedInsideWMO || centerWmoH.has_value();
        bool nearStructureSpace = nearWmoSpace || centerM2H.has_value();
        if (!nearStructureSpace && hasRealGround_) {
            const auto nearLastGround = [&](std::optional<float> h) {
                return h && std::abs(*h - lastGroundZ) <= 2.0f;
            };
            if (wmoRenderer) {
                if (nearLastGround(highestWalkableFloor(
                        [this](float x, float y, float z, float* nz) {
                            return wmoRenderer->getFloorHeight(x, y, z, nz);
                        },
                        targetPos.x, targetPos.y, feetCross(0.35f), lastGroundZ + 1.5f,
                        MIN_WALKABLE_NORMAL_WMO,
                        {.minZ = lastGroundZ - 2.0f, .maxZ = lastGroundZ + 2.0f}))) {
                    nearStructureSpace = true;
                }
            }
            if (!nearStructureSpace && m2Renderer && !externalFollow_) {
                if (nearLastGround(highestWalkableFloor(
                        [this](float x, float y, float z, float* nz) {
                            return m2Renderer->getFloorHeight(x, y, z, nz);
                        },
                        targetPos.x, targetPos.y, feetCross(0.35f), lastGroundZ + 1.5f,
                        MIN_WALKABLE_NORMAL_M2,
                        {.minZ = lastGroundZ - 2.0f, .maxZ = lastGroundZ + 2.0f}))) {
                    nearStructureSpace = true;
                }
            }
        }
        if (!groundH) {
            auto highestCenter = selectHighestFloor(centerTerrainH, centerWmoH, centerM2H);
            if (highestCenter) {
                float dz = targetPos.z - *highestCenter;
                // Keep this fallback narrow: only for WMO seam cases, or very short
                // transient misses while still almost touching the last floor.
                bool allowFallback = nearStructureSpace || (noGroundTimer_ < 0.10f && dz < 0.6f);
                if (allowFallback && dz >= -0.5f && dz < 2.0f) {
                    groundH = highestCenter;
                }
            }
        }

        // Continuity guard only for WMO seam overlap: avoid instantly switching to a
        // much lower floor sample at tunnel mouths (bad WMO ramp chains into void).
        if (groundH && hasRealGround_ && nearWmoSpace && !cachedInsideInteriorWMO) {
            float dropFromLast = lastGroundZ - *groundH;
            if (dropFromLast > 1.5f) {
                if (centerTerrainH && *centerTerrainH > *groundH + 1.5f) {
                    groundH = centerTerrainH;
                }
            }
        }

        // Floor heights must come from collision geometry. A per-frame limit
        // relative to lastGroundZ creates an unsupported descending platform,
        // continually resetting gravity and suppressing the falling state.
        // The footprint and segment probes below recover real seam support.

        // Void recovery: far beneath the terrain heightfield with no structure
        // floor anywhere below usually means a seam heuristic already failed.
        // Never use the heightfield as a rescue target while WMO containment says
        // the player is inside, though: Ironforge's valid interior floor is over
        // 200 units below the mountain terrain, and a transient floor-query miss
        // must not teleport the player onto the mountain above the city.
        if (!groundH && centerTerrainH && !cachedInsideWMO &&
            targetPos.z < *centerTerrainH - 60.0f) {
            LOG_WARNING("Void recovery: player at z=", targetPos.z,
                        " with terrain at ", *centerTerrainH, " and no floor below");
            targetPos.z = *centerTerrainH + 0.5f;
            verticalVelocity = 0.0f;
            groundH = centerTerrainH;
        }

        // WMO-only maps (Deeprun Tram, instances), and deep WMO interiors on
        // terrain maps (Ironforge), must recover to the last structural floor
        // rather than the unrelated outdoor heightfield above them.
        if (!groundH && (!centerTerrainH || cachedInsideWMO) && hasLastGroundedPos_ &&
            noGroundTimer_ > 2.5f && targetPos.z < lastGroundZ - 40.0f) {
            LOG_WARNING("Void recovery (WMO/ no heightfield): player at z=", targetPos.z,
                        " returning to last grounded pos (", lastGroundedPos_.x, ", ",
                        lastGroundedPos_.y, ", ", lastGroundedPos_.z, ")");
            targetPos = lastGroundedPos_ + glm::vec3(0.0f, 0.0f, 0.5f);
            verticalVelocity = 0.0f;
            groundH = lastGroundedPos_.z;
        }

        // 1b. Multi-sample WMO floors when in/near WMO space to avoid
        // falling through narrow board/plank gaps where center ray misses.
        if (wmoRenderer && nearWmoSpace) {
            const float wmoMultiBaseZ = grounded ? std::max(targetPos.z, lastGroundZ) : lastGroundZ;
            const float wmoProbeZ = wmoMultiBaseZ + stepUpBudget + 0.6f;
            const float minWalkableWmo =
                cachedInsideWMO ? MIN_WALKABLE_NORMAL_WMO : MIN_WALKABLE_NORMAL_TERRAIN;
            // Airborne, anything above the last known ground is a roof rather
            // than a step, so the ceiling of the band comes down.
            float maxZ = targetPos.z + stepUpBudget;
            if (!grounded) maxZ = std::min(maxZ, lastGroundZ + stepUpBudget + 0.5f);

            auto wh = highestWalkableFloor(
                [this](float x, float y, float z, float* nz) {
                    return wmoRenderer->getFloorHeight(x, y, z, nz);
                },
                targetPos.x, targetPos.y, feetCross(0.35f), wmoProbeZ, minWalkableWmo,
                {.minZ = lastGroundZ - 3.5f, .maxZ = maxZ});
            if (wh && (!groundH || *wh > *groundH)) groundH = wh;
        }

        // WMO recovery probe: when no floor is found while descending, do a wider
        // footprint sample around the player to catch narrow plank/stair misses.
        if (!groundH && wmoRenderer && hasRealGround_ && verticalVelocity <= 0.0f) {
            const float rescueProbeZ = std::max(lastGroundZ, targetPos.z) + stepUpBudget + 1.2f;
            auto rescueFloor = highestWalkableFloor(
                [this](float x, float y, float z, float* nz) {
                    return wmoRenderer->getFloorHeight(x, y, z, nz);
                },
                targetPos.x, targetPos.y, feetRing(0.65f), rescueProbeZ,
                MIN_WALKABLE_NORMAL_WMO,
                {.minZ = lastGroundZ - 6.0f, .maxZ = lastGroundZ + stepUpBudget + 0.75f});
            if (rescueFloor) groundH = rescueFloor;
        }

        // M2 recovery probe: Booty Bay-style wooden platforms can be represented
        // as M2 collision where center probes intermittently miss.
        if (!groundH && m2Renderer && !externalFollow_ && hasRealGround_ && verticalVelocity <= 0.0f) {
            const float rescueProbeZ = std::max(lastGroundZ, targetPos.z) + stepUpBudget + 1.4f;
            auto rescueFloor = highestWalkableFloor(
                [this](float x, float y, float z, float* nz) {
                    return m2Renderer->getFloorHeight(x, y, z, nz);
                },
                targetPos.x, targetPos.y, feetRing(0.75f), rescueProbeZ,
                MIN_WALKABLE_NORMAL_M2,
                {.minZ = lastGroundZ - 6.0f, .maxZ = lastGroundZ + stepUpBudget + 0.90f});
            if (rescueFloor) groundH = rescueFloor;
        }

        // Path recovery probe: sample structure floors along the movement segment
        // (prev -> current) to catch narrow plank gaps missed at endpoints.
        if (!groundH && hasRealGround_ && (wmoRenderer || (m2Renderer && !externalFollow_))) {
            std::optional<float> segmentFloor;
            const float probeZ = std::max(lastGroundZ, targetPos.z) + stepUpBudget + 1.2f;
            const float ts[] = {0.25f, 0.5f, 0.75f};
            for (float t : ts) {
                float sx = prevTargetPos.x + (targetPos.x - prevTargetPos.x) * t;
                float sy = prevTargetPos.y + (targetPos.y - prevTargetPos.y) * t;

                if (wmoRenderer) {
                    float nz = 1.0f;
                    auto wh = wmoRenderer->getFloorHeight(sx, sy, probeZ, &nz);
                    if (wh && nz >= MIN_WALKABLE_NORMAL_WMO &&
                        *wh <= lastGroundZ + stepUpBudget + 0.9f &&
                        *wh >= lastGroundZ - 3.0f) {
                        if (!segmentFloor || *wh > *segmentFloor) segmentFloor = wh;
                    }
                }
                if (m2Renderer && !externalFollow_) {
                    float nz = 1.0f;
                    auto mh = m2Renderer->getFloorHeight(sx, sy, probeZ, &nz);
                    if (mh && nz >= MIN_WALKABLE_NORMAL_M2 &&
                        *mh <= lastGroundZ + stepUpBudget + 0.9f &&
                        *mh >= lastGroundZ - 3.0f) {
                        if (!segmentFloor || *mh > *segmentFloor) segmentFloor = mh;
                    }
                }
            }
            if (segmentFloor) {
                groundH = segmentFloor;
            }
        }

        // 2. Multi-sample for M2 objects (rugs, planks, bridges, ships) -
        //    these are narrow and need offset probes to detect reliably.
        if (m2Renderer && !externalFollow_) {
            // Not highestWalkableFloor: this one prefers an M2 floor even when
            // it is slightly lower than the terrain, so a ship's deck wins over
            // the water under it.
            // Sample the swept body's lower envelope. A nearby prop can touch
            // its beveled side without raising the sole to the prop's top.
            const auto offsets = movement::bodySupportPoints(collisionRadius_);
            float m2ProbeZ = std::max(targetPos.z, lastGroundZ) + 6.0f;
            for (const auto& o : offsets) {
                float m2NormalZ = 1.0f;
                auto m2H = m2Renderer->getFloorHeight(
                    targetPos.x + o.x, targetPos.y + o.y, m2ProbeZ, &m2NormalZ);

                // Reject steep M2 slopes
                if (m2H && m2NormalZ < MIN_WALKABLE_NORMAL_TERRAIN) {
                    continue;  // Skip unwalkable M2 surface
                }

                if (m2H) *m2H -= o.z;

                // Prefer M2 floors (ships, platforms) even if slightly lower than terrain
                // to prevent falling through ship decks to water below
                if (m2H && *m2H <= targetPos.z + stepUpBudget) {
                    if (!groundH || *m2H > *groundH ||
                        (*m2H >= targetPos.z - 0.5f && *groundH < targetPos.z - 1.0f)) {
                        groundH = m2H;
                    }
                }
            }
        }

        // Outdoors the heightfield has exactly one surface per column, so feet
        // below it means the player is inside the hill, which is never a valid
        // position. Climbing a slope that rises faster than the step-up budget
        // - a steep hill, or an ordinary one crossed in a long frame - got the
        // terrain rejected as unreachable by selectReachableFloor3. Once inside,
        // every later sample was rejected the same way and the gap only widened
        // as the player fell, so nothing recovered until the void check fired 60
        // yards down. Push back out to the surface instead.
        //
        // Only where there is nothing else the player could be standing in or
        // under: a cave, a tunnel or Ironforge is legitimately beneath the
        // heightfield, and must never be yanked up onto the mountain above it.
        // Standing on something settles it before any of the probing below.
        // Grounding has already resolved a floor for this exact position; if
        // that floor sits under the heightfield and is right at the feet, the
        // player is on a structure beneath the terrain, not buried inside it.
        // That is better evidence than the probe further down, which asks
        // from a fixed height above the feet and can miss what grounding
        // already found.
        //
        // Stairs descending below ground are the case that needs it: a
        // stairwell cut into a keep passes under the heightfield within a step
        // or two, and being hauled back to the surface each time reads as not
        // being able to walk down them at all.
        const bool standingOnStructure =
            groundH && centerTerrainH &&
            *groundH < *centerTerrainH - 0.5f &&
            *groundH >= targetPos.z - 2.0f &&
            *groundH <= targetPos.z + 0.6f;

        // Feet inside ground they could step onto, held there by a structure
        // floor running just under it. The rescue below stands down near any
        // structure, because under a cave, tunnel or city the heightfield is
        // overhead; within a step of the feet it is not overhead, and no
        // structure makes it so. See movement::feetInsideGround.
        const bool feetInsideGround =
            !swimming && !hoverActive_ && !externalFollow_ && terrainManager &&
            centerTerrainH && verticalVelocity <= 0.0f &&
            movement::feetInsideGround(*centerTerrainH, targetPos.z, stepUpBudget,
                                       terrainManager->isHoleAt(targetPos.x, targetPos.y),
                                       cachedInsideInteriorWMO);
        if (feetInsideGround) {
            if (!terrainRescueActive_) {
                terrainRescueActive_ = true;
                LOG_INFO("Terrain penetration rescue: feet ", *centerTerrainH - targetPos.z,
                         " inside unholed ground at (", targetPos.x, ", ", targetPos.y,
                         "), structure floor ", groundH ? std::to_string(*groundH) : "none");
            }
            targetPos.z = *centerTerrainH;
            verticalVelocity = 0.0f;
            groundH = centerTerrainH;
            lastGroundZ = *centerTerrainH;
        } else if (!swimming && !hoverActive_ && !externalFollow_ &&
            !cachedInsideWMO && !nearStructureSpace && !standingOnStructure &&
            centerTerrainH && verticalVelocity <= 0.0f) {
            // In flight as well: flightStepAgainstGround keeps a mount from
            // flying into a hill, and this is what brings one back out that is
            // already in it.
            const float penetration = *centerTerrainH - targetPos.z;
            // Below the shallow bound is ordinary contact and sampling jitter;
            // above the deep bound is somewhere this heuristic cannot vouch for,
            // which the void recovery above already handles.
            constexpr float kMinPenetration = 0.10f;
            constexpr float kMaxPenetration = 12.0f;
            if (penetration > kMinPenetration && penetration < kMaxPenetration) {
                // Everywhere the player may legitimately stand below the
                // heightfield - the Darkshire crypts, the tunnel under the hill
                // into Booty Bay, any cave - is a structure sitting under the
                // terrain at this column. Probe for one directly instead of
                // trusting cachedInsideWMO and centerWmoH: the first lags by
                // design, and the second is cleared outright for floors more
                // than 12 yards below terrain while containment still reads
                // false, which is exactly what descending a tunnel mouth looks
                // like. Finding anything at all here means hands off - the
                // player belongs under the terrain, and lifting them out would
                // put them on the hillside above the entrance they just walked
                // into.
                // Probed from the player, not from the terrain surface: the
                // floor query culls any group whose top sits more than 4 yards
                // below the probe height, so probing from up at the heightfield
                // would skip the crypt or the tunnel entirely and report clear
                // ground - the exact opposite of the truth. From here the
                // structure the player is standing in is right above them.
                const float probeZ = targetPos.z + 1.5f;
                bool structureBelowTerrain = false;
                if (wmoRenderer &&
                    wmoRenderer->getFloorHeight(targetPos.x, targetPos.y, probeZ)) {
                    structureBelowTerrain = true;
                }
                if (!structureBelowTerrain && m2Renderer &&
                    m2Renderer->getFloorHeight(targetPos.x, targetPos.y, probeZ)) {
                    structureBelowTerrain = true;
                }
                // A hole-cut chunk is how a cave mouth or a below-ground
                // entrance is opened in the first place. getHeightAt
                // interpolates straight across the hole, so the surface this
                // rescue would push up to is not there at all - and the
                // structure underneath may still be too far below to have been
                // found by the probes above.
                if (!structureBelowTerrain && terrainManager &&
                    terrainManager->chunkHasHoles(targetPos.x, targetPos.y)) {
                    structureBelowTerrain = true;
                }

                if (!structureBelowTerrain) {
                    if (!terrainRescueActive_) {
                        terrainRescueActive_ = true;
                        LOG_INFO("Terrain penetration rescue: feet ", penetration,
                                 " below the heightfield at (", targetPos.x, ", ",
                                 targetPos.y, ") with no structure under it");
                    }
                    targetPos.z = *centerTerrainH;
                    verticalVelocity = 0.0f;
                    groundH = centerTerrainH;
                    lastGroundZ = *centerTerrainH;
                } else {
                    terrainRescueActive_ = false;
                }
            } else {
                terrainRescueActive_ = false;
            }
        } else {
            terrainRescueActive_ = false;
        }

        // On the ground under a building's floor.
        //
        // A raised floor sits a yard or so over the heightfield it is built on,
        // which is more than a step. Once the feet are down on that ground -
        // one frame at the doorway where the floor query misses the threshold
        // is enough - the floor is out of reach of the step-up, so it is never
        // chosen again, and the player walks about inside the building sunk to
        // the chest in its floor. Reported entering a house at an angle.
        //
        // Nowhere legitimate to stand has a walkable floor across the whole
        // footprint at waist height: the body would be inside it. Put the feet
        // back on it. The centre is asked first, so the four around it cost
        // nothing on the frames that matter least - every one where the answer
        // is no.
        if (!isFlightAirborne() && !hoverActive_ && !externalFollow_ && wmoRenderer &&
            nearWmoSpace && groundH && centerTerrainH && *groundH == *centerTerrainH) {
            constexpr float kWaistHeight = 1.8f;
            const float feetZ = targetPos.z;
            const auto slabAt = [&](const glm::vec2& o) -> std::optional<float> {
                float nz = 1.0f;
                const auto h = wmoRenderer->getFloorHeight(
                    targetPos.x + o.x, targetPos.y + o.y, feetZ + kWaistHeight, &nz);
                if (!h || nz < MIN_WALKABLE_NORMAL_WMO) return std::nullopt;
                if (*h <= feetZ + stepUpBudget || *h >= feetZ + kWaistHeight) return std::nullopt;
                return h;
            };
            const auto cross = feetCross(0.35f);
            std::optional<float> slab = slabAt(cross[0]);
            for (std::size_t i = 1; slab && i < cross.size(); ++i) {
                if (!slabAt(cross[i])) slab = std::nullopt;
            }
            if (slab) {
                LOG_WARNING("Under a floor: feet ", feetZ, " on the ground at (", targetPos.x,
                            ", ", targetPos.y, ") with a WMO floor at ", *slab,
                            " across the footprint - back onto it");
                targetPos.z = *slab;
                groundH = slab;
                lastGroundZ = *slab;
                verticalVelocity = 0.0f;
            }
        }

        // A falling body cannot step onto an offset prop surface above its
        // previous feet. Keep lower crossed floors visible to landing instead
        // of repeatedly choosing an unreachable log rim or bridge edge.
        if (!grounded && verticalVelocity <= 0.0f) {
            groundH = movement::descendingContactFloor(prevTargetPos.z, groundH,
                centerTerrainH, centerWmoH, centerM2H,
                terrainManager && !terrainManager->isHoleAt(prevTargetPos.x, prevTargetPos.y)
                    ? terrainManager->getHeightAt(prevTargetPos.x, prevTargetPos.y)
                    : std::nullopt);
        }

        // WOWEE_FLOOR_DEBUG=1 - what every floor query answered and which
        // one won, four times a second.
        //
        // Two fixes for one report of not being able to walk down a
        // staircase were both aimed at the wrong thing, because from the
        // outside "standing on nothing" looks the same whether the floor
        // below was never found, was found and rejected, or was found and
        // lost a selection. Those are different bugs in different places
        // and this is the line that tells them apart.
        {
            static constexpr const char* kFloorDebugPath = "/tmp/wowee_floor.log";
            static const bool kFloorDebug = [] {
                const char* v = std::getenv("WOWEE_FLOOR_DEBUG");
                return v && v[0] && v[0] != '0';
            }();
            if (kFloorDebug) {
                floorDebugTimer_ += deltaTime;
                if (floorDebugTimer_ >= 0.25f) {
                    floorDebugTimer_ = 0.0f;
                    auto say = [](const std::optional<float>& v) {
                        return v ? std::to_string(*v) : std::string("-");
                    };
                    std::ostringstream line;
                    line << "FLOOR at (" << targetPos.x << ", " << targetPos.y
                         << ", " << targetPos.z << ")"
                         << " terrain=" << say(centerTerrainH)
                         << " wmo="     << say(centerWmoH)
                         << " m2="      << say(centerM2H)
                         << " chose="   << say(groundH)
                         << " hole="    << (terrainManager &&
                                terrainManager->isHoleAt(targetPos.x, targetPos.y))
                         << " insideWMO="      << cachedInsideWMO
                         << " insideInterior=" << cachedInsideInteriorWMO
                         << " grounded="       << grounded
                         << " vz="             << verticalVelocity
                         << " lastGroundZ="    << lastGroundZ;
                    LOG_WARNING(line.str());
                    // Also to a file. The console is where this used to go
                    // and it scrolled past - asking someone to reproduce a
                    // bug and then hand back terminal scrollback is asking
                    // twice. Truncated on the first line of a run so the
                    // file is always this session's.
                    static std::ofstream trace(kFloorDebugPath,
                                               std::ios::out | std::ios::trunc);
                    if (trace) trace << line.str() << '\n' << std::flush;
                }
            }
        }

        if (groundH) {
            hasRealGround_ = true;
            noGroundTimer_ = 0.0f;
            lastGroundedPos_ = glm::vec3(targetPos.x, targetPos.y, *groundH);
            hasLastGroundedPos_ = true;
            float feetZ = targetPos.z;
            float stepUp = stepUpBudget;
            stepUp += 0.05f;
            float fallCatch = 3.0f;
            float dz = *groundH - feetZ;

            // Only snap when:
            // 1. Near ground (within step-up range above) - handles walking
            // 2. Airborne feet crossed the floor while descending. Proximity
            //    alone must not truncate the final part of the jump arc.
            // 3. Was grounded + ground is close (grace for slopes)
            bool nearGround = (grounded && dz >= 0.0f && dz <= stepUp);
            bool airFalling = (!grounded && verticalVelocity <= 0.0f
                               && dz >= 0.0f && prevTargetPos.z >= *groundH);
            if (!grounded && centerTerrainH && terrainManager &&
                std::abs(*groundH - *centerTerrainH) < 0.001f &&
                !terrainManager->isHoleAt(prevTargetPos.x, prevTargetPos.y)) {
                const auto previousTerrain = terrainManager->getHeightAt(
                    prevTargetPos.x, prevTargetPos.y);
                if (previousTerrain) {
                    airFalling = movement::crossesGroundSurface(
                        prevTargetPos.z, *previousTerrain, feetZ, *groundH);
                }
            }
            // Not while climbing in flight: a frame's climb at a high frame
            // rate is less than the quarter yard this reaches down, so taking
            // off was pulled back to the ground every frame and never left it.
            bool slopeGrace = (grounded && verticalVelocity > -1.0f &&
                               dz >= -0.25f && dz <= stepUp * 1.5f) &&
                              !(isFlightAirborne() && f.spaceHeld);

            if (dz >= -fallCatch && (nearGround || airFalling || slopeGrace)) {
                // HOVER: float at fixed height above ground instead of standing on it
                static constexpr float HOVER_HEIGHT = 4.0f;  // ~4 yards above ground
                float snapH = hoverActive_ ? (*groundH + HOVER_HEIGHT) : *groundH;
                bool steepBodyContact = false;
                if (m2Renderer && !externalFollow_ && !hoverActive_ && snapH < feetZ) {
                    float supportZ = snapH;
                    float supportNormalZ = 1.0f;
                    if (m2Renderer->constrainFloorSnap(targetPos, snapH, collisionRadius_,
                            collisionHeight_, supportZ, supportNormalZ)) {
                        snapH = supportZ;
                        steepBodyContact = !movement::isWalkableNormal(supportNormalZ);
                    }
                }
                static const bool traceContact = core::envFlagEnabled("WOWEE_BODY_CONTACT_TRACE");
                if (traceContact && movementTraceM2Contacts_ > 0) {
                    LOG_INFO("Ground snap after M2 contact: feet=", feetZ, " floor=", snapH,
                        " grounded=", grounded, " slopeGrace=", slopeGrace);
                }
                targetPos.z = snapH;
                bool steepTerrain = false;
                if (terrainManager && !ignoreSlopeLimit_ && !hoverActive_ && !externalFollow_) {
                    glm::vec3 normal(0.0f, 0.0f, 1.0f);
                    const auto height = terrainManager->getHeightAt(targetPos.x, targetPos.y, &normal);
                    steepTerrain = height && !terrainManager->isHoleAt(targetPos.x, targetPos.y) &&
                        std::abs(*height - *groundH) < 0.05f && !movement::isWalkableNormal(normal.z);
                }
                // Steep surfaces block penetration but cannot support standing.
                // Preserve falling velocity for downhill contact projection.
                if (!steepTerrain && !steepBodyContact) verticalVelocity = 0.0f;
                grounded = !steepTerrain && !steepBodyContact;
                lastGroundZ = snapH;
            } else {
                grounded = false;
                lastGroundZ = *groundH;
            }
            } else {
                hasRealGround_ = false;
                noGroundTimer_ += f.physicsDeltaTime;

                float dropFromLastGround = lastGroundZ - targetPos.z;
                bool seamSizedGap = dropFromLastGround <= (nearStructureSpace ? 2.5f : 0.35f);
                if (noGroundTimer_ < NO_GROUND_GRACE && seamSizedGap) {
                    // Near WMO floors, prefer continuity over falling on transient
                    // floor-query misses (stairs/planks/portal seams).
                    float maxSlip = nearStructureSpace ? 1.0f : 0.10f;
                    targetPos.z = std::max(targetPos.z, lastGroundZ - maxSlip);
                    if (nearStructureSpace && verticalVelocity < -2.0f) {
                        verticalVelocity = -2.0f;
                    }
                    grounded = false;
                } else if (nearStructureSpace && noGroundTimer_ < 1.0f && dropFromLastGround <= 3.0f) {
                    // Extended WMO rescue window: hold close to last valid floor so we
                    // do not tunnel through walkable geometry during short hitches.
                    targetPos.z = std::max(targetPos.z, lastGroundZ - 0.35f);
                    if (verticalVelocity < -1.5f) {
                        verticalVelocity = -1.5f;
                    }
                    grounded = false;
                } else if (nearStructureSpace && noGroundTimer_ < 1.20f && dropFromLastGround <= 4.0f && !f.nowJump) {
                    // Extended adhesion for sparse dock/bridge collision: keep us on the
                    // last valid support long enough for adjacent structure probes to hit.
                    targetPos.z = std::max(targetPos.z, lastGroundZ - 0.10f);
                    if (verticalVelocity < -0.5f) verticalVelocity = -0.5f;
                    grounded = true;
                } else {
                    grounded = false;
                }
            }
        }

    // Update follow target position
    *followTarget = targetPos;

    // --- Safe position caching + void fall detection ---
    if (grounded && hasRealGround_ && !swimming && verticalVelocity >= 0.0f) {
        // Player is safely on real geometry - save periodically
        continuousFallTime_ = 0.0f;
        autoUnstuckFired_ = false;
        safePosSaveTimer_ += f.physicsDeltaTime;
        if (safePosSaveTimer_ >= SAFE_POS_SAVE_INTERVAL) {
            safePosSaveTimer_ = 0.0f;
            lastSafePos_ = targetPos;
            hasLastSafe_ = true;
        }
    } else if (!grounded && !swimming && !externalFollow_) {
        // Falling (or standing on nothing past grace period) - accumulate fall time
        continuousFallTime_ += f.physicsDeltaTime;
        if (continuousFallTime_ >= AUTO_UNSTUCK_FALL_TIME && !autoUnstuckFired_) {
            autoUnstuckFired_ = true;
            if (autoUnstuckCallback_) {
                autoUnstuckCallback_();
            }
        }
    }
}

// The camera itself: where it sits relative to the character it follows.
//
// Pivot at the neck, zoom smoothing, the raycast that pulls the camera in when
// a wall is behind it, the floor clearance that stops it sinking through a
// tunnel roof, and the first-person threshold where the character is hidden.
void CameraController::updateOrbitCamera(float deltaTime, FrameInput& f,
                                         const glm::vec3& targetPos) {
    // ===== WoW-style orbit camera =====
    // Pivot point at upper chest/neck.
    float mountedOffset = mounted_ ? mountHeightOffset_ : 0.0f;
    float pivotLift = 0.0f;
    if (terrainManager && !externalFollow_ && !cachedInsideInteriorWMO) {
        float plx = targetPos.x - lastPivotLiftQueryPos_.x;
        float ply = targetPos.y - lastPivotLiftQueryPos_.y;
        float movedSq = plx * plx + ply * ply;
        constexpr float kPivotLiftPosSq = PIVOT_LIFT_POS_THRESHOLD * PIVOT_LIFT_POS_THRESHOLD;
        float distDelta = std::abs(currentDistance - lastPivotLiftDistance_);
        bool queryLift = (++pivotLiftQueryCounter_ >= PIVOT_LIFT_QUERY_INTERVAL) ||
                         (movedSq >= kPivotLiftPosSq) ||
                         (distDelta >= PIVOT_LIFT_DIST_THRESHOLD);
        if (queryLift) {
            pivotLiftQueryCounter_ = 0;
            lastPivotLiftQueryPos_ = targetPos;
            lastPivotLiftDistance_ = currentDistance;

            // Estimate where camera sits horizontally and ensure enough terrain clearance.
            glm::vec3 probeCam = targetPos + (-f.forward3D) * currentDistance;
            auto terrainAtCam = terrainManager->getHeightAt(probeCam.x, probeCam.y);
            auto terrainAtPivot = terrainManager->getHeightAt(targetPos.x, targetPos.y);

            float desiredLift = 0.0f;
            if (terrainAtCam) {
                // Keep the camera high enough that its ray does not cut
                // through a hill behind the character.
                //
                // Measured at the camera, which is the thing that has to
                // clear the ground. It was measured at the pivot, against a
                // fixed two metres above the terrain - and an absolute height
                // undoes any attempt to place the pivot on the character, by
                // exactly the amount that attempt moved it. On flat ground a
                // gnome's pivot at 0.65 was lifted 1.35 and a human's at 1.60
                // was lifted 0.40, both landing on 2.00 above the feet. Every
                // race got the same pivot, every time, and which character was
                // being followed made no difference to it.
                //
                // That is why zooming in on a gnome put them at the bottom of
                // the frame: the camera was aiming at a point nearly twice
                // their height, on level ground, with no hill in sight.
                const float basePivotZ = targetPos.z + followedPivotHeight() + mountedOffset;
                // The camera sits behind the character along the look
                // direction, which carries the pitch with it.
                desiredLift = terrainPivotLift(basePivotZ, (-f.forward3D).z,
                                               currentDistance, *terrainAtCam);
            }
            // If character is already below local terrain sample, avoid lifting aggressively.
            if (terrainAtPivot && targetPos.z < *terrainAtPivot - 0.2f) {
                desiredLift = 0.0f;
            }
            // Said when it starts and when it stops, because a lift is the
            // camera being moved by the ground rather than by the player and
            // there was no way to tell the two apart from outside. This one
            // was applying on flat ground in every session ever played.
            const bool lifting = desiredLift > 0.01f;
            if (lifting != pivotLiftActive_) {
                pivotLiftActive_ = lifting;
                LOG_WARNING("Camera pivot: terrain lift ",
                            lifting ? "on, raising the pivot by " : "off, was ",
                            desiredLift, " (pivot ", followedPivotHeight(),
                            " above the feet, ground at the camera ",
                            terrainAtCam ? *terrainAtCam - targetPos.z : 0.0f,
                            " relative to them)");
            }
            cachedPivotLift_ = desiredLift;
        }
        pivotLift = cachedPivotLift_;
    } else if (cachedInsideInteriorWMO) {
        // Inside WMO volumes (including tunnel/cave shells): terrain-above samples
        // are not relevant for camera pivoting.
        cachedPivotLift_ = 0.0f;
    }
    glm::vec3 pivot = targetPos + glm::vec3(0.0f, 0.0f, followedPivotHeight() + mountedOffset + pivotLift);
    // Surface swimmers keep the eye above water; divers keep it below.
    // Apply the same half-space constraint before and after camera smoothing.
    std::optional<float> waterCameraBoundary;
    bool cameraSubmerged = false;
    if (swimming && waterRenderer) {
        const auto surface = waterRenderer->getNearestWaterHeightAt(
            targetPos.x, targetPos.y, targetPos.z, 400.0f);
        if (surface) {
            cameraSubmerged = targetPos.z + swimSurfaceOffset_ < *surface - 0.1f;
            const float clearance = std::max(0.05f, camera->getNearPlane()
                * std::tan(glm::radians(camera->getFovDegrees()) * 0.5f));
            waterCameraBoundary = *surface + (cameraSubmerged ? -clearance : clearance);
            pivot.z = cameraSubmerged ? std::min(pivot.z, *waterCameraBoundary)
                                      : std::max(pivot.z, *waterCameraBoundary);
        }
    }

    // Camera direction from yaw/pitch (already computed as forward3D)
    glm::vec3 camDir = -f.forward3D;  // Camera looks at pivot, so it's behind
    // Contact below the pivot (build 8606, Goldshire and Elwynn terrain,
    // Shattrath walls and floor): once an obstacle stops the camera and the
    // camera is below the pivot, pitching up further keeps the camera where
    // it is and turns only the view; pitching back down below that orbit
    // pitch moves the camera again.
    if (floorContactPitch_ && pitch <= *floorContactPitch_) floorContactPitch_.reset();
    const float orbitPitch = floorContactPitch_ ? *floorContactPitch_ : pitch;
    if (floorContactPitch_) {
        const float ry = glm::radians(yaw), rp = glm::radians(orbitPitch);
        camDir = -glm::vec3(std::cos(ry) * std::cos(rp), std::sin(ry) * std::cos(rp), std::sin(rp));
    }

    // Throttle the active-group refresh: only recheck every 10 frames or when
    // position changes >2 units.
    if (wmoRenderer) {
        glm::vec3 wmoCheckDelta = targetPos - lastInsideWMOCheckPos;
        float distFromLastCheckSq = glm::dot(wmoCheckDelta, wmoCheckDelta);
        if (++insideWMOCheckCounter >= 10 || distFromLastCheckSq > 4.0f) {
            wmoRenderer->updateActiveGroup(targetPos.x, targetPos.y, targetPos.z + 1.0f);
            insideWMOCheckCounter = 0;
            lastInsideWMOCheckPos = targetPos;
        }
    }

    // Indoors has no limit of its own: in the Shattrath bank, the Lion's Pride
    // Inn and the Deadmines the original zooms to its full distance and only
    // the room's geometry stops it.
    advanceZoom(deltaTime);

    // ===== Camera collision (WMO/M2 raycasts + terrain heightfield march) =====
    // Cast a ray from the pivot toward the camera direction to find the
    // nearest building or doodad, and march the terrain heightfield along the same
    // ray so hills between the pivot and camera pull the camera in too.
    // Pull-in is immediate; recovery follows easedCollisionLimit.
    collisionDistance = currentDistance;

    if ((wmoRenderer || m2Renderer || terrainManager) && currentDistance > MIN_DISTANCE) {
        // Geometry pulls the camera inward immediately. Terrain has its own
        // slower response because sampled slope heights vary between frames.
        auto geometryLimitAlong = [&](const glm::vec3& dir) -> float {
            traceWmoHit_ = wmoRenderer ? wmoRenderer->raycastCamera(pivot, dir, currentDistance) : currentDistance;
            traceM2Hit_ = m2Renderer ? m2Renderer->raycastBoundingBoxes(pivot, dir, currentDistance) : currentDistance;
            const float hit = std::min(traceWmoHit_, traceM2Hit_);
            // hit == currentDistance means no hit (returns maxDistance on miss)
            if (hit >= currentDistance) return currentDistance;
            return std::max(MIN_DISTANCE, hit - CAMERA_WALL_STANDOFF);
        };

        float rawLimit = geometryLimitAlong(camDir);

        // Preserve the player's orbit direction. Obstacles only shorten the
        // camera ray; terrain uses separate smoothing to avoid slope jitter.
        if (!cachedInsideInteriorWMO) {
            const float terrainLimit =
                raymarchTerrainCameraLimit(pivot, camDir, currentDistance);
            traceTerrainLimit_ = terrainLimit;
            if (smoothedTerrainDist_ < 0.0f) smoothedTerrainDist_ = terrainLimit;
            // Slower coming in than the built geometry above, which is what
            // makes a slope feel like a slope rather than a series of nudges.
            const float terrainTau = (terrainLimit < smoothedTerrainDist_) ? 0.18f : 0.45f;
            const float terrainAlpha = 1.0f - std::exp(-deltaTime / terrainTau);
            smoothedTerrainDist_ += (terrainLimit - smoothedTerrainDist_) * terrainAlpha;
            rawLimit = std::min(rawLimit, smoothedTerrainDist_);
        } else {
            smoothedTerrainDist_ = -1.0f;
        }

        const float applied = easedCollisionLimit(
            rawLimit < currentDistance ? std::optional<float>(rawLimit) : std::nullopt, deltaTime);
        collisionDistance = std::min(collisionDistance, applied);
        // Any obstacle holds it once the camera is below the pivot: native
        // against a Terrace wall slid along the ray down to orbit -1 deg and
        // held at +2 (camera 1.65 yd above the feet, not on the floor); on
        // terrain it held at the first blocked pitch (Elwynn 5.5, Goldshire 1).
        if (!floorContactPitch_ && rawLimit < currentDistance - 0.01f && camDir.z < 0.0f) {
            floorContactPitch_ = orbitPitch;
        }
    } else {
        smoothedCollisionDist_ = -1.0f;   // Reset when no collision sources available
        collisionEaseTime_ = kCollisionEaseSeconds;
        smoothedTerrainDist_ = -1.0f;
    }

    // Camera collision: terrain-only floor clamping
    auto getTerrainFloorAt = [&](float x, float y) -> std::optional<float> {
        if (terrainManager) {
            return terrainManager->getHeightAt(x, y);
        }
        return std::nullopt;
    };

    // Use collision distance (don't exceed user target)
    float actualDist = std::min(currentDistance, collisionDistance);

    // Compute actual camera position
    glm::vec3 actualCam;
    if (actualDist < FIRST_PERSON_DISTANCE) {
        // First-person: position camera at pivot (player's eyes)
        actualCam = pivot + f.forward3D * 0.1f;  // Slightly forward to not clip head
    } else {
        actualCam = pivot + camDir * actualDist;
    }
    if (waterCameraBoundary) {
        actualCam = constrainWaterCamera(pivot, actualCam, *waterCameraBoundary, cameraSubmerged);
    }

    // Prefer a view clearly above the water or clearly below it.
    //
    // Sitting exactly on the surface is the one place the water has nothing
    // sensible to draw: the near plane cuts it, and what is left is a seam
    // across the middle of the view. Lean out of a band either side - whichever
    // side the camera is already nearer - and let it settle there.
    //
    // Applied to the target, not to the smoothed position. Nudging the smoothed
    // position after the fact fought the follow, which re-aims at this target
    // every frame: the two pulled opposite ways and the camera hovered on the
    // waterline crossing it several times a second, which is exactly the state
    // this is meant to avoid. Moving the target instead lets the existing
    // smoothing carry it, and it stays where it is put.
    //
    // The band is the near plane's half-height, because that is the depth range
    // over which the near plane can be half in the water. Lifting eases the
    // camera back a little and dropping draws it in, so the character stays
    // framed rather than sliding down the screen.
    if (!waterCameraBoundary && waterRenderer && actualDist >= FIRST_PERSON_DISTANCE) {
        const float band = std::max(0.05f, camera->getNearPlane()
                                           * std::tan(glm::radians(camera->getFovDegrees()) * 0.5f));
        auto surfaceZ = waterRenderer->getNearestWaterHeightAt(
            actualCam.x, actualCam.y, actualCam.z, 30.0f);
        // Clear of the band, not resting on its edge.
        //
        // Parking exactly at the edge puts the camera on the same threshold the
        // underwater overlay switches at, so the smallest movement flipped it on
        // and off and the view changed brightness with it. Overshooting settles
        // the camera on one side of that decision.
        float wantOffset = 0.0f;
        if (surfaceZ) {
            const float above = actualCam.z - *surfaceZ;
            if (std::abs(above) < band) {
                const float clear = band * 1.35f;
                wantOffset = ((above >= 0.0f) ? (*surfaceZ + clear) : (*surfaceZ - clear))
                           - actualCam.z;
            }
        }
        // Eased, and eased back to nothing when there is no water under the
        // camera at all. Applying it the instant the query finds a surface, and
        // dropping it the instant it does not, made the camera jump at the
        // water's edge - which is where a player crossing in and out of a lake
        // spends their time.
        const float ease = 1.0f - std::exp(-deltaTime / 0.18f);
        waterNudgeZ_ += (wantOffset - waterNudgeZ_) * ease;
        if (std::abs(waterNudgeZ_) > 1e-3f) {
            actualCam.z += waterNudgeZ_;
            glm::vec2 outward(actualCam.x - pivot.x, actualCam.y - pivot.y);
            const float outLen = glm::length(outward);
            if (outLen > 1e-3f) {
                outward /= outLen;
                actualCam.x += outward.x * waterNudgeZ_ * 0.6f;
                actualCam.y += outward.y * waterNudgeZ_ * 0.6f;
            }
        }
    } else {
        waterNudgeZ_ = 0.0f;
    }

    // Smooth camera position to avoid jitter
    if (glm::dot(smoothedCamPos, smoothedCamPos) < 1e-4f) {
        smoothedCamPos = actualCam;  // Initialize
    }
    bool activelyRotating = mouseButtonDown || f.nowTurnLeft || f.nowTurnRight;
    float camLerp = (activelyRotating && !smoothCameraFollow_)
        ? 1.0f : (1.0f - std::exp(-camSmoothSpeed_ * deltaTime));
    smoothedCamPos += (actualCam - smoothedCamPos) * camLerp;

    // ===== Final floor clearance check =====
    // Use WMO-aware floor so the camera doesn't pop above tunnels/caves.
    constexpr float MIN_FLOOR_CLEARANCE = 0.35f;
    if (!cachedInsideWMO) {
        std::optional<float> camTerrainH;
        if (!cachedInsideInteriorWMO) {
            camTerrainH = getTerrainFloorAt(smoothedCamPos.x, smoothedCamPos.y);
        }
        std::optional<float> camWmoH;
        if (wmoRenderer) {
            // Skip expensive WMO floor query if camera barely moved
            float cdx = smoothedCamPos.x - lastCamFloorQueryPos.x;
            float cdy = smoothedCamPos.y - lastCamFloorQueryPos.y;
            float camDeltaSq = cdx * cdx + cdy * cdy;
            if (camDeltaSq < 0.09f && hasCachedCamFloor) {
                camWmoH = cachedCamWmoFloor;
            } else {
                float camFloorProbeZ = smoothedCamPos.z;
                if (cachedInsideInteriorWMO) {
                    // Inside tunnels/buildings, probe near player height so roof
                    // triangles above the camera don't get treated as floor.
                    camFloorProbeZ = std::min(smoothedCamPos.z, targetPos.z + 1.0f);
                }
                camWmoH = wmoRenderer->getFloorHeight(
                    smoothedCamPos.x, smoothedCamPos.y, camFloorProbeZ);

                if (cachedInsideInteriorWMO && camWmoH) {
                    // Never let camera floor clamp latch to tunnel ceilings / upper decks.
                    float maxValidIndoorFloor = targetPos.z + 0.9f;
                    if (*camWmoH > maxValidIndoorFloor) {
                        camWmoH = std::nullopt;
                    }
                }
                cachedCamWmoFloor = camWmoH;
                hasCachedCamFloor = true;
                lastCamFloorQueryPos = smoothedCamPos;
            }
        }
        // When camera/character are inside a WMO, force WMO floor usage for camera
        // clearance to avoid snapping toward terrain above enclosed tunnels/caves.
        std::optional<float> camFloorH;
        if (cachedInsideWMO && camWmoH && camTerrainH) {
            // Transition seam: avoid terrain-above clamp near tunnel entrances.
            float camDropFromPlayer = targetPos.z - *camWmoH;
            if ((*camTerrainH - *camWmoH) > 1.2f &&
                (*camTerrainH - *camWmoH) < 8.0f &&
                camDropFromPlayer >= -0.4f &&
                camDropFromPlayer < 1.8f) {
                camFloorH = camWmoH;
            } else {
                camFloorH = selectClosestFloor(camTerrainH, camWmoH, smoothedCamPos.z);
            }
        } else {
            camFloorH = selectReachableFloor(
                camTerrainH, camWmoH, smoothedCamPos.z, 0.5f);
        }
        // Terrain has no underside. The reachability filter above is for
        // structure floors, where something half a yard over the camera may be
        // a deck it is under; applied to the heightfield it meant that a
        // camera which had sunk more than half a yard into a hill - coming in
        // from a taxi, or on a sharp rise - was left there, the ground now
        // counting as a ceiling. While the character stands on or above the
        // terrain, the camera belongs above it too.
        if (!cachedInsideInteriorWMO) {
            auto groundH = cameraTerrainFloor(smoothedCamPos.x, smoothedCamPos.y, targetPos);
            if (groundH && (!camFloorH || *groundH > *camFloorH)) camFloorH = groundH;
        }
        if (camFloorH && smoothedCamPos.z < *camFloorH + MIN_FLOOR_CLEARANCE) {
            smoothedCamPos.z = *camFloorH + MIN_FLOOR_CLEARANCE;
        }
    }
    // Land camera clearance does not apply to swimmers. Underwater, looking
    // upward puts the eye below the feet; only real geometry may obstruct it.
    if (!swimming) smoothedCamPos.z = std::max(smoothedCamPos.z, targetPos.z + 0.15f);

    if (waterCameraBoundary) {
        smoothedCamPos = constrainWaterCamera(
            pivot, smoothedCamPos, *waterCameraBoundary, cameraSubmerged);
    }

    camera->setPosition(smoothedCamPos);

    // Measure the followed model and hide it when the camera enters its head.
    if (characterRenderer && playerInstanceId > 0) {
        // How tall whoever is being followed actually is, so the pivot lands
        // on them rather than at a fixed distance above their feet. Measured
        // here because it changes with the model: a race change, a mount, a
        // shapeshift. Kept if it ever fails to measure, so a frame that could
        // not answer does not snap the camera.
        float height = 0.0f;
        // Measured on the model as authored: the original's pivot does not
        // grow with the character's display scale. Pair 22, pivot over the
        // feet: tauren male (display 59 at 1.35) 1.83, human 1.86, gnome
        // (1563 at 1.15) 0.88 - a gnome's or tauren's camera stays where a
        // scale-1 model of the same race puts it
        // (_experiments/20261118T160000Z.player_scale).
        const float drawnScale = std::max(0.001f, characterRenderer->getInstanceScale(playerInstanceId));
        if (characterRenderer->getInstanceHeight(playerInstanceId, height) && height > 0.01f) {
            height /= drawnScale;
            // The head bone, which is what the pivot is placed against. A
            // skeleton that does not name one leaves this zero and falls back
            // to the height.
            //
            // A frame that cannot answer keeps the last answer. The height
            // above is already guarded that way by the enclosing `if`; the
            // head bone was not, and zeroing it dropped the pivot to a lower
            // tier of pivotHeightFor - a gnome went 0.65 to 0.88, or to the
            // flat 1.6 when the height was missing too. Entering a building is
            // where this showed: the WMO transition re-registers the instance
            // and the bone query returns false for a frame, so the camera rose
            // on the doorstep and stayed up.
            float headZ = 0.0f;
            const bool measuredHead =
                characterRenderer->getInstanceKeyBonePivotZ(playerInstanceId, kKeyBoneHead, headZ)
                && headZ > 0.01f;
            if (measuredHead) headZ /= drawnScale;
            if (!measuredHead) headZ = followedHeadZ_;
            if (std::abs(height - followedHeight_) > 0.01f ||
                std::abs(headZ - followedHeadZ_) > 0.01f) {
                // Said when it changes, which is when a character is first
                // drawn and whenever its model does. These numbers settle
                // whether the camera is pivoting where it should, and none of
                // them was knowable from outside: a report of "still too high"
                // can mean nothing was measured, or that it was and the
                // proportion is wrong, and they tell the two apart.
                LOG_WARNING("Camera pivot: character is ", height,
                            " tall with its head bone at ", headZ,
                            ", pivoting at ", pivotHeightFor(pivotHeight_, height, headZ),
                            " (setting ", pivotHeight_, " against a ",
                            kPivotReferenceHeadZ, " head reference)");
            }
            followedHeight_ = height;
            if (!characterRenderer->getInstanceCollisionDimensions(
                    playerInstanceId, collisionRadius_, collisionHeight_)) {
                LOG_ERROR("Collision bounds missing after successful character height query");
                throw std::logic_error("Character collision bounds unavailable");
            }
            if (measuredHead) followedHeadZ_ = headZ;
        }

        // Collision can put the eye inside the head despite a distant zoom
        // request. Share this visibility decision with the renderer so its
        // recovery path cannot immediately reveal the intersecting mesh again.
        const float eyeDistance = glm::distance(smoothedCamPos, pivot);
        // Build 8606 (zoom steps at the Terrace): opaque at 1.31 yd and above,
        // partly transparent at 1.06, about half at 0.79-0.63, faint at 0.43,
        // gone by 0.17-0.23: a linear fade from 1.2 down to 0.2 yd.
        playerCameraOpacity_ = glm::clamp((eyeDistance - 0.2f) / 1.0f, 0.0f, 1.0f);
        characterRenderer->setInstanceCameraOpacity(playerInstanceId,
            playerCameraOpacity_);
        characterRenderer->setInstanceVisible(playerInstanceId, !shouldHidePlayer());

        // Note: the Renderer's CharAnimState machine drives player character animations
        // (Run, Walk, Jump, Swim, etc.) - no additional animation driving needed here.
    }
}

void CameraController::updateThirdPersonCamera(float deltaTime, FrameInput& f) {
    movementTraceWmoContacts_ = 0;
    movementTraceM2Contacts_ = 0;
    glm::vec3 prevTargetPos(0.0f);
    // Consume the entire frame in collision-safe steps. Dropping time makes
    // movement lag the network clock and forces remote heartbeat corrections.
    const int steps = std::max(1, static_cast<int>(std::ceil(deltaTime / kMaxPhysicsDelta)));
    f.physicsDeltaTime = deltaTime / steps;
    glm::vec3 targetPos(0.0f);
    const bool jumpPressed = f.nowJump;
    // Touchdown within the frame: the landing packet reports that instant
    // and place, not the end of the frame (the character runs on or stops
    // after it).
    touchdownSeconds_.reset();
    bool airborneBeforeStep = !grounded;
    for (int step = 0; step < steps; ++step) {
        f.nowJump = jumpPressed && step == 0;
        targetPos = moveFollowedCharacter(f.physicsDeltaTime, f, prevTargetPos);
        groundFollowedCharacter(f.physicsDeltaTime, f, targetPos, prevTargetPos);
        if (airborneBeforeStep && grounded && !touchdownSeconds_) {
            touchdownSeconds_ = f.physicsDeltaTime * static_cast<float>(step + 1);
            touchdownPosition_ = *followTarget;
        }
        airborneBeforeStep = !grounded;
    }
    f.nowJump = jumpPressed;
    // Landing: flying down onto real ground, or skimming it, without climbing.
    // Grounding has just put the feet on the floor if they were within reach
    // of it; on a floor that was actually found, that is touching down.
    if (isFlightAirborne() && grounded && hasRealGround_ && !f.spaceHeld &&
        std::abs(targetPos.z - lastGroundZ) < 0.3f) {
        flightAirborne_ = false;
        LOG_WARNING("Flight: landed");
    }
    updateOrbitCamera(deltaTime, f, viewTarget_ ? *viewTarget_ : targetPos);
    static const bool traceMovement = core::envFlagEnabled("WOWEE_MOVEMENT_TRACE");
    if (traceMovement && followTarget) {
        // Render coordinates, yards, degrees, seconds. SDL ticks are process-relative.
        // Contact counts report sweep hits, not distinct objects or defect verdicts.
        LOG_INFO("PLAYTESTER_MOVEMENT {\"tick_ns\":", SDL_GetTicksNS(),
            ",\"dt\":", deltaTime, ",\"physics_dt\":", f.physicsDeltaTime * steps,
            ",\"position\":[", followTarget->x, ",", followTarget->y, ",", followTarget->z,
            "],\"previous\":[", prevTargetPos.x, ",", prevTargetPos.y, ",", prevTargetPos.z,
            "],\"facing_deg\":", facingYaw, ",\"camera_yaw_deg\":", yaw,
            ",\"vertical_velocity\":", verticalVelocity, ",\"floor_z\":", lastGroundZ,
            ",\"grounded\":", grounded ? "true" : "false",
            ",\"real_ground\":", hasRealGround_ ? "true" : "false",
            ",\"swimming\":", swimming ? "true" : "false",
            ",\"forward\":", f.nowForward ? "true" : "false",
            ",\"backward\":", f.nowBackward ? "true" : "false",
            ",\"strafe_left\":", f.nowStrafeLeft ? "true" : "false",
            ",\"strafe_right\":", f.nowStrafeRight ? "true" : "false",
            ",\"jump_pressed\":", f.nowJump ? "true" : "false",
            ",\"ui_keyboard\":", f.uiWantsKeyboard ? "true" : "false",
            ",\"rooted\":", movementRooted_ ? "true" : "false",
            ",\"wmo_contacts\":", movementTraceWmoContacts_,
            ",\"m2_contacts\":", movementTraceM2Contacts_,
            ",\"zoom_distance\":", currentDistance,
            ",\"camera_distance\":", std::min(currentDistance, collisionDistance),
            ",\"camera_pitch_deg\":", pitch,
            ",\"camera_wmo_hit\":", traceWmoHit_, ",\"camera_m2_hit\":", traceM2Hit_,
            ",\"camera_terrain_limit\":", traceTerrainLimit_, "}");
    }
}

// The camera that flies itself, used when nothing is being followed.
void CameraController::updateFreeFlyCamera(float /*deltaTime*/, FrameInput& f) {
    // Free-fly camera mode (original behavior)
    glm::vec3 newPos = camera->getPosition();
    if (wmoRenderer) {
        wmoRenderer->setCollisionFocus(newPos, COLLISION_FOCUS_RADIUS_FREE_FLY);
    }
    if (m2Renderer) {
        m2Renderer->setCollisionFocus(newPos, COLLISION_FOCUS_RADIUS_FREE_FLY);
    }
    float feetZ = newPos.z - eyeHeight;

    // Check for water at feet position
    std::optional<float> waterH;
    if (waterRenderer) {
        waterH = waterRenderer->getNearestWaterHeightAt(newPos.x, newPos.y, feetZ, 400.0f);
    }
    constexpr float MAX_SWIM_DEPTH_FROM_SURFACE = 12.0f;
    bool inWater = false;
    if (waterH && feetZ < *waterH) {
        std::optional<uint16_t> waterType;
        if (waterRenderer) {
            waterType = waterRenderer->getWaterTypeAt(newPos.x, newPos.y);
        }
        bool isOcean = false;
        if (waterType && *waterType != 0) {
            isOcean = (((*waterType - 1) % 4) == 1);
        }
        bool depthAllowed = isOcean || ((*waterH - feetZ) <= MAX_SWIM_DEPTH_FROM_SURFACE);
        if (!depthAllowed) {
            inWater = false;
        } else {
        std::optional<float> terrainH;
        std::optional<float> wmoH;
        std::optional<float> m2H;
        if (terrainManager)
            terrainH = liquidBedTerrain(terrainManager->getHeightAt(newPos.x, newPos.y), feetZ + 2.0f);
        if (wmoRenderer) wmoH = wmoRenderer->getFloorHeight(newPos.x, newPos.y, feetZ + 2.0f);
        if (m2Renderer && !externalFollow_) m2H = m2Renderer->getFloorHeight(newPos.x, newPos.y, feetZ + 1.0f);
        auto floorH = selectHighestFloor(terrainH, wmoH, m2H);
        // Hysteresis: starting to swim needs deeper water than continuing to
        // swim does. With one threshold, a character wading at the boundary
        // flipped between swim and walk every frame - each flip restarts the
        // locomotion animation and sends a START_SWIM/STOP_SWIM pair, which
        // is what made walking out of water stutter.
        // Entering takes the character's own swimming depth: native 2.4.3
        // (human male) stands without MOVEFLAG_SWIMMING on floors 0.05-1.3 yd
        // under Blackfathom MLIQ water and sends START_SWIM 1.58 yd under it
        // (_experiments/20261117T174000Z.bfd_liquid_pair7). Exit is unmeasured.
        const float SWIM_ENTER_WATER_DEPTH = swimSurfaceOffset_;
        constexpr float SWIM_EXIT_WATER_DEPTH  = 0.85f;
        const float MIN_SWIM_WATER_DEPTH =
            swimming ? SWIM_EXIT_WATER_DEPTH : SWIM_ENTER_WATER_DEPTH;
        inWater = (floorH && ((*waterH - *floorH) >= MIN_SWIM_WATER_DEPTH)) || (isOcean && !floorH);
        }
    }


    if (inWater) {
        swimming = true;
        float swimSpeed = (swimSpeedOverride_ > 0.0f && swimSpeedOverride_ < 100.0f && !std::isnan(swimSpeedOverride_))
                              ? swimSpeedOverride_ : f.speed * SWIM_SPEED_FACTOR;
        float waterSurfaceCamZ = waterH ? (*waterH - swimSurfaceOffset_ + eyeHeight) : newPos.z;
        bool diveIntent = f.nowForward && (f.forward3D.z < -0.28f);

        float movLenSq = glm::dot(f.movement, f.movement);
        if (movLenSq > 1e-6f) {
            f.movement *= glm::inversesqrt(movLenSq);
            newPos += f.movement * swimSpeed * f.physicsDeltaTime;
        }

        if (f.nowJump) {
            verticalVelocity = SWIM_BUOYANCY;
        } else {
            verticalVelocity += SWIM_GRAVITY * f.physicsDeltaTime;
            if (verticalVelocity < SWIM_SINK_SPEED) {
                verticalVelocity = SWIM_SINK_SPEED;
            }
            if (!diveIntent) {
                float surfaceErr = (waterSurfaceCamZ - newPos.z);
                verticalVelocity += surfaceErr * 7.0f * f.physicsDeltaTime;
                verticalVelocity *= std::max(0.0f, 1.0f - 3.2f * f.physicsDeltaTime);
                if (std::abs(surfaceErr) < 0.06f && std::abs(verticalVelocity) < 0.35f) {
                    verticalVelocity = 0.0f;
                }
            }
        }

        newPos.z += verticalVelocity * f.physicsDeltaTime;

        // Don't rise above water surface (feet at water level)
        if (waterH && (newPos.z - eyeHeight) > *waterH - swimSurfaceOffset_) {
            newPos.z = *waterH - swimSurfaceOffset_ + eyeHeight;
            if (verticalVelocity > 0.0f) verticalVelocity = 0.0f;
        }

        grounded = false;
    } else {
        swimming = false;

        float movLenSq2 = glm::dot(f.movement, f.movement);
        if (movLenSq2 > 1e-6f) {
            f.movement *= glm::inversesqrt(movLenSq2);
            newPos += f.movement * f.speed * f.physicsDeltaTime;
        }

        // Jump with input buffering and coyote time
        if (f.nowJump) jumpBufferTimer = JUMP_BUFFER_TIME;
        if (grounded) coyoteTimer = COYOTE_TIME;

        if (coyoteTimer > 0.0f && jumpBufferTimer > 0.0f && !(mounted_ && movLenSq2 <= 1e-6f)) {
            f.startedJump = true;
            verticalVelocity = f.jumpVel;
            grounded = false;
            jumpBufferTimer = 0.0f;
            coyoteTimer = 0.0f;
        }

        jumpBufferTimer -= f.physicsDeltaTime;
        coyoteTimer -= f.physicsDeltaTime;

        // Apply f.gravity, unless the ground under the character has not
        // streamed in yet. See the note at the other gravity site.
        if (groundNotStreamedYet(newPos.x, newPos.y)) {
            verticalVelocity = 0.0f;
        } else {
            verticalVelocity += f.gravity * f.physicsDeltaTime;
            newPos.z += verticalVelocity * f.physicsDeltaTime;
        }
    }

    // Wall sweep collision before grounding (skip when stationary).
    {
        // Swept at the feet, not the eyes. Doodads are left out here, which
        // is the one thing this camera does differently.
        const glm::vec3 eyeOffset(0, 0, eyeHeight);
        newPos = sweepAgainstWalls(camera->getPosition() - eyeOffset,
                                   newPos - eyeOffset, false) + eyeOffset;
    }

    // Ground to terrain or WMO floor
    {
        auto sampleGround = [&](float x, float y) -> std::optional<float> {
            std::optional<float> terrainH;
            std::optional<float> wmoH;
            std::optional<float> m2H;
            if (terrainManager) {
                terrainH = terrainManager->getHeightAt(x, y);
            }
            float feetZ = newPos.z - eyeHeight;
            float wmoProbeZ = std::max(feetZ, lastGroundZ) + 1.5f;
            float m2ProbeZ = std::max(feetZ, lastGroundZ) + 6.0f;
            if (wmoRenderer) {
                wmoH = wmoRenderer->getFloorHeight(x, y, wmoProbeZ);
            }
            if (m2Renderer && !externalFollow_) {
                m2H = m2Renderer->getFloorHeight(x, y, m2ProbeZ);
            }
            auto base = selectReachableFloor(terrainH, wmoH, feetZ, 1.0f);
            if (m2H && *m2H <= feetZ + 1.0f && (!base || *m2H > *base)) {
                base = m2H;
            }
            return base;
        };

        // Single center probe.
        std::optional<float> groundH = sampleGround(newPos.x, newPos.y);

        if (groundH) {
            float feetZ = newPos.z - eyeHeight;
            float stepUp = 1.0f;
            float fallCatch = 3.0f;
            float dz = *groundH - feetZ;

            // Only snap when:
            // 1. Near ground (within step-up range above) - handles walking
            // 2. Actually falling from height (was airborne + falling fast)
            // 3. Was grounded + ground is close (grace for slopes)
            bool nearGround = (grounded && dz >= 0.0f && dz <= stepUp);
            bool airFalling = (!grounded && verticalVelocity < -5.0f);
            bool slopeGrace = (grounded && dz >= -1.0f && dz <= stepUp * 2.0f);

            if (dz >= -fallCatch && (nearGround || airFalling || slopeGrace)) {
                newPos.z = *groundH + eyeHeight;
                verticalVelocity = 0.0f;
                grounded = true;
                lastGroundZ = *groundH;
                swimming = false;
            } else if (!swimming) {
                grounded = false;
                lastGroundZ = *groundH;
            }
        } else if (!swimming) {
            newPos.z = lastGroundZ + eyeHeight;
            verticalVelocity = 0.0f;
            grounded = true;
        }
    }

    camera->setPosition(newPos);
}

void CameraController::update(float deltaTime) {
    if (!enabled || !camera) {
        return;
    }
    // Keep physics integration stable during render hitches to avoid floor tunneling.
    const float physicsDeltaTime = std::min(deltaTime, kMaxPhysicsDelta);
    intoxicationTime_ += deltaTime;

    // During taxi flights, skip movement logic but keep camera orbit/zoom controls.
    if (externalFollow_) {
        // Cancel any active intro/idle orbit so mouse panning works during taxi.
        // The intro handling code (below) is unreachable during externalFollow_.
        introActive = false;
        idleOrbit_ = false;
        idleTimer_ = 0.0f;

        camera->setRotation(yaw, pitch);
        advanceZoom(deltaTime);
        collisionDistance = currentDistance;

        // Position camera behind character during taxi
        if (thirdPerson && followTarget) {
            glm::vec3 targetPos = viewTarget_ ? *viewTarget_ : *followTarget;
            glm::vec3 forward3D = camera->getForward();

            // Pivot point at upper chest/neck
            float mountedOffset = mounted_ ? mountHeightOffset_ : 0.0f;
            glm::vec3 pivot = targetPos + glm::vec3(0.0f, 0.0f, followedPivotHeight() + mountedOffset);

            // Camera direction from yaw/pitch
            glm::vec3 camDir = -forward3D;

            // Use current distance
            float actualDist = std::min(currentDistance, collisionDistance);

            // Compute camera position
            glm::vec3 actualCam;
            // Small offset prevents the camera from clipping into the character
            // model when collision pushes it to near-minimum distance.
            constexpr float kCameraClipEpsilon = 0.1f;
            if (actualDist < MIN_DISTANCE + kCameraClipEpsilon) {
                actualCam = pivot + forward3D * kCameraClipEpsilon;
            } else {
                actualCam = pivot + camDir * actualDist;
            }

            // Smooth camera position (1:1 while actively dragging, see main update()).
            if (glm::dot(smoothedCamPos, smoothedCamPos) < 1e-4f) {
                smoothedCamPos = actualCam;
            }
            float camLerp = (mouseButtonDown && !smoothCameraFollow_)
                ? 1.0f : (1.0f - std::exp(-camSmoothSpeed_ * deltaTime));
            smoothedCamPos += (actualCam - smoothedCamPos) * camLerp;

            // A flight path skims ridges and lands on hillsides.
            if (auto groundH = cameraTerrainFloor(smoothedCamPos.x, smoothedCamPos.y, targetPos)) {
                constexpr float kTaxiFloorClearance = 0.35f;
                smoothedCamPos.z = std::max(smoothedCamPos.z, *groundH + kTaxiFloorClearance);
            }

            camera->setPosition(smoothedCamPos);
        }

        // The ground limits are not kept up in flight. Left as they were at
        // takeoff, the first frames on the ground eased toward them from a
        // limit measured somewhere else entirely; starting over takes the
        // ground as it is at the landing.
        smoothedCollisionDist_ = -1.0f;
        collisionEaseTime_ = kCollisionEaseSeconds;
        smoothedTerrainDist_ = -1.0f;

        return;
    }

    auto& input = core::Input::getInstance();

    // Don't process keyboard input when UI text input (e.g. chat box) has focus.
    // Both interfaces are asked: the flag below is ImGui's own and says nothing
    // about a chat box FrameXML draws, so on its own it let the character walk
    // away while someone was typing.
    bool uiWantsKeyboard = ImGui::GetIO().WantTextInput ||
                           ui::interfaceTakingTypedInput();

    // Suppress movement input after teleport/portal (keys may still be held)
    if (movementSuppressTimer_ > 0.0f) {
        movementSuppressTimer_ -= deltaTime;
    }
    // While the server holds the character no key moves, turns or jumps it.
    bool movementSuppressed = movementSuppressTimer_ > 0.0f || serverControlled_;
    if (serverControlled_) clearMovementInputs();

    auto& bindings = ui::KeybindingManager::getInstance();
    if (bindings.isCommandPressed("TOGGLERUN")) walking_ = !walking_;
    if (bindings.isCommandPressed("CAMERAZOOMIN")) processMouseWheel(1.0f);
    if (bindings.isCommandPressed("CAMERAZOOMOUT")) processMouseWheel(-1.0f);
    if (bindings.isCommandPressed("WOWEERESETCAMERA")) resetAngles();
    if (bindings.isCommandPressed("NEXTVIEW")) nextView();
    if (bindings.isCommandPressed("PREVVIEW")) prevView();
    for (int view = 1; view <= kViewCount; ++view) {
        const std::string n = std::to_string(view);
        if (bindings.isCommandPressed("SETVIEW" + n)) setView(view);
        if (view >= 2 && bindings.isCommandPressed("SAVEVIEW" + n)) saveView(view);
        if (view >= 2 && bindings.isCommandPressed("RESETVIEW" + n)) resetView(view);
    }

    // Determine current key states
    // Arrow keys mirror WASD the way the original client maps them:
    // Up/Down move, Left/Right turn (strafe stays on Q/E).
    bool keyW = !uiWantsKeyboard && !sitting && !movementSuppressed && bindings.isCommandDown("MOVEFORWARD");
    bool keyS = !uiWantsKeyboard && !sitting && !movementSuppressed && bindings.isCommandDown("MOVEBACKWARD");
    bool keyA = !uiWantsKeyboard && !sitting && !movementSuppressed && bindings.isCommandDown("TURNLEFT");
    bool keyD = !uiWantsKeyboard && !sitting && !movementSuppressed && bindings.isCommandDown("TURNRIGHT");
    bool keyQ = !uiWantsKeyboard && !sitting && !movementSuppressed && bindings.isCommandDown("STRAFELEFT");
    bool keyE = !uiWantsKeyboard && !sitting && !movementSuppressed && bindings.isCommandDown("STRAFERIGHT");
    bool shiftDown = !uiWantsKeyboard && (input.isKeyPressed(SDL_SCANCODE_LSHIFT) || input.isKeyPressed(SDL_SCANCODE_RSHIFT));
    bool ctrlDown = !uiWantsKeyboard && (input.isKeyPressed(SDL_SCANCODE_LCTRL) || input.isKeyPressed(SDL_SCANCODE_RCTRL));
    // Ctrl temporarily forces walking without changing the bindable toggle.
    const bool walking = walking_ || ctrlDown;
    bool nowJump = !uiWantsKeyboard && !sitting && !movementSuppressed && bindings.isCommandPressed("JUMP");
    // Swimming and flying need the held state, not the press edge: on land
    // space is a one-shot jump, but in water and in the air it is continuous
    // ascent, and an edge gave a single impulse that then bled away - or, in
    // flight, a single frame of climb per press.
    bool spaceHeld = !uiWantsKeyboard && !sitting && !movementSuppressed && bindings.isCommandDown("JUMP");

    // Idle camera: any input resets the timer; timeout triggers a slow orbit pan
    bool anyInput = leftMouseDown || rightMouseDown || keyW || keyS || keyA || keyD || keyQ || keyE || nowJump;
    if (anyInput) {
        idleTimer_ = 0.0f;
    } else if (!introActive && idleOrbitEnabled_) {
        idleTimer_ += deltaTime;
        if (idleTimer_ >= IDLE_TIMEOUT) {
            idleTimer_ = 0.0f;
            startIntroPan(30.0f, 360.0f); // Slow casual orbit over 30 seconds
            idleOrbit_ = true;
        }
    } else if (!idleOrbitEnabled_) {
        idleTimer_ = 0.0f;
    }

    if (introActive) {
        if (anyInput) {
            introActive = false;
            idleOrbit_ = false;
            idleTimer_ = 0.0f;
        } else {
            introTimer += deltaTime;
            if (idleOrbit_) {
                // Continuous smooth rotation - no lerp endpoint, just constant angular velocity
                float degreesPerSec = introOrbitDegrees / introDuration;
                yaw -= degreesPerSec * deltaTime;
                camera->setRotation(yaw, pitch);
                if (!seatedInChair_ && !serverControlled_) facingYaw = yaw;
            } else {
                float t = (introDuration > 0.0f) ? std::min(introTimer / introDuration, 1.0f) : 1.0f;
                yaw = introStartYaw + (introEndYaw - introStartYaw) * t;
                pitch = introStartPitch + (introEndPitch - introStartPitch) * t;
                currentDistance = introStartDistance + (introEndDistance - introStartDistance) * t;
                userTargetDistance = introEndDistance;
                camera->setRotation(yaw, pitch);
                if (!seatedInChair_ && !serverControlled_) facingYaw = yaw;
                if (t >= 1.0f) {
                    introActive = false;
                }
            }
        }
        // Suppress player movement/input during intro.
        keyW = keyS = keyA = keyD = keyQ = keyE = nowJump = spaceHeld = false;
    }

    // The same action the keybinding panel rebinds; forward/backward cancels it.
    if (!uiWantsKeyboard && !serverControlled_ && ui::KeybindingManager::getInstance().isActionPressed(
            ui::KeybindingManager::Action::TOGGLE_AUTORUN)) {
        autoRunning = !autoRunning;
    }
    // Helper: cancel auto-follow and notify game handler
    auto doCancelAutoFollow = [&]() {
        if (autoFollowTarget_) {
            autoFollowTarget_ = nullptr;
            if (autoFollowCancelCallback_) autoFollowCancelCallback_();
        }
    };

    if (keyW || keyS) {
        autoRunning = false;
        doCancelAutoFollow();
    }

    bool mouseAutorun = !uiWantsKeyboard && !sitting && !serverControlled_ &&
                        leftMouseDown && rightMouseDown;
    if (mouseAutorun) {
        autoRunning = false;
        doCancelAutoFollow();
    }

    // Auto-follow: face target and run toward them when within range
    bool autoFollowMove = false;
    if (autoFollowTarget_ && followTarget && !movementRooted_ && !serverControlled_) {
        glm::vec3 myPos = *followTarget;
        glm::vec3 tgtPos = *autoFollowTarget_;
        float dx = tgtPos.x - myPos.x;
        float dy = tgtPos.y - myPos.y;
        float distSq2D = dx * dx + dy * dy;

        if (distSq2D > FOLLOW_MAX_DIST * FOLLOW_MAX_DIST) {
            doCancelAutoFollow();
        } else if (distSq2D > FOLLOW_STOP_DIST * FOLLOW_STOP_DIST) {
            // Match forward = (cos(yaw), sin(yaw)) in render coordinates.
            float targetYawRad = std::atan2(dy, dx);
            float targetYawDeg = targetYawRad * 180.0f / core::coords::PI;
            facingYaw = targetYawDeg;
            yaw = targetYawDeg;
            autoFollowMove = true;
        }
        // else: within stop distance, stay put

        // Cancel on strafe/turn keys
        if (keyA || keyD || keyQ || keyE) {
            doCancelAutoFollow();
            autoFollowMove = false;
        }
    }

    // When the server has rooted the player, suppress all horizontal movement input.
    const bool movBlocked = movementRooted_;
    // Auto-follow uses run speed (same as auto-run), not walk speed
    bool nowForward = !movBlocked && (keyW || mouseAutorun || autoRunning || autoFollowMove);
    bool nowBackward = !movBlocked && keyS;
    bool nowStrafeLeft = false;
    bool nowStrafeRight = false;
    bool nowTurnLeft = false;
    bool nowTurnRight = false;

    // WoW-like third-person keyboard behavior:
    // - RMB held: A/D strafe
    // - RMB released: A/D turn character+camera, Q/E strafe
    // Turning is allowed even while rooted; only positional movement is blocked.
    if (thirdPerson && !rightMouseDown) {
        nowTurnLeft = keyA;
        nowTurnRight = keyD;
        nowStrafeLeft = !movBlocked && keyQ;
        nowStrafeRight = !movBlocked && keyE;
    } else {
        nowStrafeLeft = !movBlocked && (keyA || keyQ);
        nowStrafeRight = !movBlocked && (keyD || keyE);
    }

    // Keyboard turning updates camera yaw (character follows yaw in renderer).
    // Use server turn rate (rad/s) when set; otherwise fall back to WOW_TURN_SPEED (deg/s).
    const float activeTurnSpeedDeg = (turnRateOverride_ > 0.0f && turnRateOverride_ < 20.0f
                                       && !std::isnan(turnRateOverride_))
                                         ? glm::degrees(turnRateOverride_)
                                         : WOW_TURN_SPEED;
    // The character turns from its own facing and the camera turns with it.
    // Facing and camera yaw differ after a server-set facing (teleport) or a
    // camera orbit; copying the camera yaw snapped the character to it on
    // the first turning frame.
    const float turnDeltaDeg = (nowTurnLeft == nowTurnRight) ? 0.0f
        : (nowTurnLeft ? activeTurnSpeedDeg : -activeTurnSpeedDeg) * deltaTime;
    if (nowTurnLeft || nowTurnRight) {
        yaw += turnDeltaDeg;
        facingYaw += turnDeltaDeg;
        camera->setRotation(yaw, pitch);
    }

    // Tick down gravity suspension timer (used after world entry to prevent
    // falling through WMO floors before collision is loaded)
    if (entryFloorAnchorTimer_ > 0.0f) entryFloorAnchorTimer_ -= deltaTime;
    if (gravitySuspendTimer_ > 0.0f) {
        gravitySuspendTimer_ -= deltaTime;
    }

    // Select physics constants based on mode
    float gravity = useWoWSpeed ? WOW_GRAVITY : GRAVITY;
    float jumpVel = useWoWSpeed ? WOW_JUMP_VELOCITY : JUMP_VELOCITY;

    // Suspend gravity after world entry - hold Z position until timer expires
    // OR a floor is detected. This prevents falling through unloaded WMO floors.
    if (gravitySuspendTimer_ > 0.0f) {
        gravity = 0.0f;
        verticalVelocity = 0.0f;
    }

    // Calculate movement speed based on direction and modifiers
    float speed;
    if (useWoWSpeed) {
        // Movement speeds (WoW-like: Ctrl walk, default run, backpedal slower)
        if (nowBackward && !nowForward) {
            speed = (runBackSpeedOverride_ > 0.0f && runBackSpeedOverride_ < 100.0f
                     && !std::isnan(runBackSpeedOverride_))
                        ? runBackSpeedOverride_ : WOW_BACK_SPEED;
        } else if (walking) {
            speed = (walkSpeedOverride_ > 0.0f && walkSpeedOverride_ < 100.0f && !std::isnan(walkSpeedOverride_))
                        ? walkSpeedOverride_ : WOW_WALK_SPEED;
        } else if (runSpeedOverride_ > 0.0f && runSpeedOverride_ < 100.0f && !std::isnan(runSpeedOverride_)) {
            speed = runSpeedOverride_;
        } else {
            speed = WOW_RUN_SPEED;
        }
    } else {
        // Exploration mode (original behavior)
        speed = movementSpeed;
        if (shiftDown) {
            speed *= sprintMultiplier;
        }
        if (ctrlDown) {
            speed *= slowMultiplier;
        }
    }

    bool hasMoveInput = nowForward || nowBackward || nowStrafeLeft || nowStrafeRight;
    if (useWoWSpeed) {
        // "Sprinting" flag drives run animation/stronger footstep set.
        // In WoW mode this means running pace (not walk/backpedal), not Shift.
        runPace = hasMoveInput && !walking && !nowBackward;
    } else {
        runPace = hasMoveInput && shiftDown;
    }
    if (serverControlled_) runPace = serverDrivenRun_;

    // Get camera axes - project forward onto XY plane for walking
    glm::vec3 forward3D = camera->getForward();
    // Left-button orbit changes the view while movement retains character
    // facing. Right-button/touch steering owns facing on land and in water.
    // steering_ is the touch controls saying a finger is dragging the view.
    // On a phone that is what the right mouse button is on a desktop: the
    // character turns to face where the player is looking, so that walking
    // forward goes where the screen points.
    // A chair owns body orientation. Mouse/touch orbit may move the camera,
    // but must not replace the server's seat heading or send a new facing.
    // Only once the mouse has actually turned the view during this hold; see
    // CameraController::cameraDrivesFacing.
    // The server's spline owns the facing while it holds the character; the
    // mouse still orbits the camera.
    bool cameraDrivesFacing = !serverControlled_ && CameraController::cameraDrivesFacing(
        rightMouseDown, rotateArmed_, mouseAutorun, steering_, seatedInChair_);
    // During taxi flights, orientation is controlled by the flight path, not player input
    if (cameraDrivesFacing && !externalFollow_) {
        facingYaw = yaw;
    }
    float moveYaw = cameraDrivesFacing ? yaw : facingYaw;
    if (intoxication_ > 0.34f) {
        // Drunk and smashed characters weave while trying to walk. Keep facing
        // authoritative; only the travel direction stumbles side to side.
        moveYaw += std::sin(intoxicationTime_ * 2.1f) * 11.0f * intoxication_;
    }
    float moveYawRad = glm::radians(moveYaw);
    glm::vec3 forward(std::cos(moveYawRad), std::sin(moveYawRad), 0.0f);
    glm::vec3 right(-std::sin(moveYawRad), std::cos(moveYawRad), 0.0f);

    // Flight goes where the mount faces, not where the camera looks. Orbiting
    // the camera with the left button is looking around, and it took the
    // flight direction with it: W flew toward the camera's view, whatever the
    // mount was pointing at. The pitch follows the camera only while the
    // camera steers, as the facing does, and holds otherwise.
    if (cameraDrivesFacing && !externalFollow_) {
        const float len = glm::length(forward3D);
        if (len > 1e-4f) {
            flightPitchDeg_ = glm::degrees(std::asin(std::clamp(forward3D.z / len, -1.0f, 1.0f)));
        }
    }
    if (!isFlightAirborne()) flightPitchDeg_ = 0.0f;
    if (!swimming) swimPitchDeg_ = 0.0f;
    if (swimming && cameraDrivesFacing && !externalFollow_) {
        const float len = glm::length(forward3D);
        assert(len > 1e-4f);
        swimPitchDeg_ = glm::degrees(std::asin(std::clamp(forward3D.z / len, -1.0f, 1.0f)));
    }
    const float swimPitchRad = glm::radians(swimPitchDeg_);
    const glm::vec3 swimForward(std::cos(moveYawRad) * std::cos(swimPitchRad),
                               std::sin(moveYawRad) * std::cos(swimPitchRad),
                               std::sin(swimPitchRad));
    const float flightPitchRad = glm::radians(flightPitchDeg_);
    const glm::vec3 flightForward(std::cos(moveYawRad) * std::cos(flightPitchRad),
                                  std::sin(moveYawRad) * std::cos(flightPitchRad),
                                  std::sin(flightPitchRad));

    // Retain short taps/wheel pulses for sitting; diving still needs held input.
    bool prevSitting = sitting;
    bool xDown = !serverControlled_ && bindings.isCommandDown("SITORSTAND");
    if (!serverControlled_ && bindings.isCommandPressed("SITORSTAND") && !mounted_ && !swimming) {
        sitting = !sitting;
    }
    if (mounted_) sitting = false;

    // Stand up on any movement input or jump while sitting (WoW behaviour)
    //
    // Both mouse buttons is a way of walking forward, and it was missing from
    // this list while every key that does the same was on it. Sitting gates
    // the autorun it produces, so the two together meant that after eating
    // there was no way to move with the mouse at all: the input that would
    // have stood the character up was the one input that could not.
    if (sitting && !movementSuppressed) {
        const bool anyMoveKey = bindings.isCommandDown("MOVEFORWARD") ||
            bindings.isCommandDown("MOVEBACKWARD") || bindings.isCommandDown("TURNLEFT") ||
            bindings.isCommandDown("TURNRIGHT") || bindings.isCommandDown("STRAFELEFT") ||
            bindings.isCommandDown("STRAFERIGHT") || bindings.isCommandDown("JUMP");
        // Not gated on uiWantsKeyboard, which is a different question, and not
        // on the interface wanting the mouse either: both flags are already
        // cleared at the press when it did.
        const bool mouseWalk = leftMouseDown && rightMouseDown;
        if (anyMoveKey || mouseWalk) sitting = false;
    }

    // Notify server when the player stands up via local input
    if (prevSitting && !sitting && standUpCallback_) {
        standUpCallback_();
    }

    // Notify server when the player sits down via local input
    if (!prevSitting && sitting && sitDownCallback_) {
        sitDownCallback_();
    }

    // Update eye height based on crouch state (smooth transition)
    float targetEyeHeight = sitting ? CROUCH_EYE_HEIGHT : STAND_EYE_HEIGHT;
    float heightLerpSpeed = 10.0f * deltaTime;
    eyeHeight = eyeHeight + (targetEyeHeight - eyeHeight) * std::min(1.0f, heightLerpSpeed);

    // Calculate horizontal movement vector
    glm::vec3 movement(0.0f);

    if (nowForward) movement += forward;
    if (nowBackward) movement -= forward;
    if (nowStrafeLeft) movement += right;
    if (nowStrafeRight) movement -= right;

    // Everything the two camera modes need from the work above.
    FrameInput f;
    f.physicsDeltaTime = physicsDeltaTime;
    f.gravity = gravity;
    f.jumpVel = jumpVel;
    f.speed = speed;
    groundLocomotionSpeed_ = glm::dot(movement, movement) > 0.0001f ? speed : 0.0f;
    f.forward = forward;
    f.right = right;
    f.forward3D = forward3D;
    f.flightForward = flightForward;
    f.swimForward = swimForward;
    f.movement = movement;
    f.nowForward = nowForward;
    f.nowBackward = nowBackward;
    f.nowStrafeLeft = nowStrafeLeft;
    f.nowStrafeRight = nowStrafeRight;
    f.nowTurnLeft = nowTurnLeft;
    f.nowTurnRight = nowTurnRight;
    f.nowJump = nowJump;
    f.spaceHeld = spaceHeld;
    f.xDown = xDown;
    f.uiWantsKeyboard = uiWantsKeyboard;

    // Two cameras: one orbits a character and moves it, one flies itself.
    if (thirdPerson && followTarget) {
        updateThirdPersonCamera(deltaTime, f);
    } else {
        updateFreeFlyCamera(deltaTime, f);
    }

    // The modes adjust the move - the swim clamp and the flight path both do -
    // and the movement opcodes below are decided from what actually happened.
    movement = f.movement;

    // The lower body aims where the character travels. In the air that is the
    // momentum it left the ground with (or the drift of a standing jump), not
    // the keys: native 2.4.3 keeps a strafing jumper's legs turned to the side
    // to the landing when the keys switch to the other side, are released, or
    // forward gains a strafe; the legs take the new keys on touching down.
    const bool airborneMomentum = !grounded && !swimming && preserveAirborneMomentum_ && !knockbackActive_;
    const glm::vec3 travel = airborneMomentum ? airborneHorizontalVelocity_ : movement;
    if (glm::dot(travel, travel) > 0.0001f) {
        travelYaw_ = glm::degrees(std::atan2(travel.y, travel.x));
        legsFollowTravel_ = airborneMomentum ? airborneMomentumStrafe_
                                             : (nowStrafeLeft || nowStrafeRight) && !nowBackward;
    } else {
        travelYaw_ = facingYaw;
        legsFollowTravel_ = false;
    }

    // --- Edge-detection: send movement opcodes on state transitions ---
    if (movementCallback) {
        // Walk/run mode goes out when it changes, moving or not (native
        // MSG_MOVE_SET_WALK_MODE 0x100 / MSG_MOVE_SET_RUN_MODE), before a
        // start in the same frame so that start already carries it.
        if (walking != sentWalking_) {
            sentWalking_ = walking;
            movementCallback(static_cast<uint32_t>(walking ? game::Opcode::MSG_MOVE_SET_WALK_MODE
                                                           : game::Opcode::MSG_MOVE_SET_RUN_MODE));
        }
        // Forward/backward
        if (nowForward && !wasMovingForward) {
            movementCallback(static_cast<uint32_t>(game::Opcode::MSG_MOVE_START_FORWARD));
        }
        if (nowBackward && !wasMovingBackward) {
            movementCallback(static_cast<uint32_t>(game::Opcode::MSG_MOVE_START_BACKWARD));
        }
        if ((!nowForward && wasMovingForward) || (!nowBackward && wasMovingBackward)) {
            if (!nowForward && !nowBackward) {
                movementCallback(static_cast<uint32_t>(game::Opcode::MSG_MOVE_STOP));
            }
        }

        // STOP_STRAFE clears both wire flags. Clear released directions first,
        // then restore any held direction, including a direct left/right switch.
        const bool releasedStrafe = (!nowStrafeLeft && wasStrafingLeft) ||
                                    (!nowStrafeRight && wasStrafingRight);
        if (releasedStrafe) {
            movementCallback(static_cast<uint32_t>(game::Opcode::MSG_MOVE_STOP_STRAFE));
        }
        if (nowStrafeLeft && (!wasStrafingLeft || releasedStrafe)) {
            movementCallback(static_cast<uint32_t>(game::Opcode::MSG_MOVE_START_STRAFE_LEFT));
        }
        if (nowStrafeRight && (!wasStrafingRight || releasedStrafe)) {
            movementCallback(static_cast<uint32_t>(game::Opcode::MSG_MOVE_START_STRAFE_RIGHT));
        }

        // Turning
        if (nowTurnLeft && !wasTurningLeft) {
            movementCallback(static_cast<uint32_t>(game::Opcode::MSG_MOVE_START_TURN_LEFT));
        }
        if (nowTurnRight && !wasTurningRight) {
            movementCallback(static_cast<uint32_t>(game::Opcode::MSG_MOVE_START_TURN_RIGHT));
        }
        if ((!nowTurnLeft && wasTurningLeft) || (!nowTurnRight && wasTurningRight)) {
            if (!nowTurnLeft && !nowTurnRight) {
                movementCallback(static_cast<uint32_t>(game::Opcode::MSG_MOVE_STOP_TURN));
            }
        }

        // Physics has already cleared grounded. Report its successful takeoff
        // event, including buffered jumps, rather than rechecking input/ground.
        if (f.startedJump) {
            jumpReported_ = true;
            movementCallback(static_cast<uint32_t>(game::Opcode::MSG_MOVE_JUMP));
        }

        // Fall landing, for a fall that actually happened.
        //
        // A bump at riding speed breaks ground contact for a frame, and the
        // frame after it this read as a landing: the packet went out, and
        // every client in range played the landing animation on the mount.
        // Crossing open ground did that several times a second, which is what
        // other players see as a mount behaving erratically.
        //
        // Long enough to be a fall rather than a step. Well under the shortest
        // real drop - a jump is airborne for about a second - so nothing a
        // player would call a fall is lost.
        constexpr float kFallReportSeconds = 0.15f;
        if (grounded && (jumpReported_ ||
                (wasFalling && airborneSeconds_ >= kFallReportSeconds))) {
            movementCallback(static_cast<uint32_t>(game::Opcode::MSG_MOVE_FALL_LAND));
            jumpReported_ = false;
        }
    }

    // Swimming state transitions
    if (movementCallback) {
        if (swimming && !wasSwimming) {
            movementCallback(static_cast<uint32_t>(game::Opcode::MSG_MOVE_START_SWIM));
            jumpReported_ = false;
        } else if (!swimming && wasSwimming && !f.startedJump) {
            // JUMP already leaves swimming. A second transition would restart
            // the observer's movement prediction at the takeoff position.
            movementCallback(static_cast<uint32_t>(game::Opcode::MSG_MOVE_STOP_SWIM));
        }
    }

    // Flight ascend/descend transitions (Space = ascend, X = descend while mounted+flying)
    if (movementCallback && !externalFollow_) {
        const bool airborne = isFlightAirborne();
        const bool nowAscending = airborne && spaceHeld;
        const bool nowDescending = airborne && xDown && mounted_;

        if (airborne) {
            if (nowAscending && !wasAscending_) {
                movementCallback(static_cast<uint32_t>(game::Opcode::MSG_MOVE_START_ASCEND));
            } else if (!nowAscending && wasAscending_) {
                movementCallback(static_cast<uint32_t>(game::Opcode::MSG_MOVE_STOP_ASCEND));
            }
            if (nowDescending && !wasDescending_) {
                movementCallback(static_cast<uint32_t>(game::Opcode::MSG_MOVE_START_DESCEND));
            } else if (!nowDescending && wasDescending_) {
                // No separate STOP_DESCEND opcode; STOP_ASCEND ends all vertical movement
                movementCallback(static_cast<uint32_t>(game::Opcode::MSG_MOVE_STOP_ASCEND));
            }
        } else {
            // Left flight mode: clear any lingering vertical movement states
            if (wasAscending_) {
                movementCallback(static_cast<uint32_t>(game::Opcode::MSG_MOVE_STOP_ASCEND));
            } else if (wasDescending_) {
                movementCallback(static_cast<uint32_t>(game::Opcode::MSG_MOVE_STOP_ASCEND));
            }
        }
        wasAscending_ = nowAscending;
        wasDescending_ = nowDescending;
    }

    // Update previous-frame state
    wasSwimming = swimming;
    wasMovingForward = nowForward;
    wasMovingBackward = nowBackward;
    wasStrafingLeft = nowStrafeLeft;
    wasStrafingRight = nowStrafeRight;
    moveForwardActive = nowForward;
    moveBackwardActive = nowBackward;
    strafeLeftActive = nowStrafeLeft;
    strafeRightActive = nowStrafeRight;
    turningLeftActive = nowTurnLeft;
    turningRightActive = nowTurnRight;
    wasTurningLeft = nowTurnLeft;
    wasTurningRight = nowTurnRight;
    wasJumping = nowJump;
    wasFalling = !grounded && verticalVelocity <= 0.0f;
    // This is also the wire fallTime: swimming is not part of a ballistic arc.
    // Takeoff starts a new clock even when no ground contact preceded it.
    // Include this frame's physics, since heartbeats sample its final position.
    airborneSeconds_ = (grounded || swimming || isFlightAirborne()) ? 0.0f :
        (f.startedJump ? deltaTime : airborneSeconds_ + deltaTime);

    // R key is now handled above with chat safeguard (WantTextInput check)

    // Camera shake (SMSG_CAMERA_SHAKE): apply sinusoidal offset to final camera position.
    if (shakeElapsed_ < shakeDuration_) {
        shakeElapsed_ += deltaTime;
        float t = shakeElapsed_ / shakeDuration_;
        // Envelope: fade out over the last 30% of shake duration
        float envelope = (t < 0.7f) ? 1.0f : (1.0f - (t - 0.7f) / 0.3f);
        float theta = shakeElapsed_ * shakeFrequency_ * core::coords::TWO_PI;
        glm::vec3 offset(
            shakeMagnitude_ * envelope * std::sin(theta),
            shakeMagnitude_ * envelope * std::cos(theta * 1.3f),
            shakeMagnitude_ * envelope * std::sin(theta * 0.7f) * 0.5f
        );
        if (camera) {
            camera->setPosition(camera->getPosition() + offset);
        }
    }

    // Scaled by the same setting as the shake above, because it is the same
    // thing from the player's side: the view moving without being asked to.
    //
    // The stumble in the travel direction is deliberately not scaled with it.
    // That one is what being drunk does to the character rather than to the
    // picture, and turning it off would be an advantage rather than a comfort.
    if (intoxication_ > 0.0f && camera) {
        const float swayYaw = std::sin(intoxicationTime_ * 1.35f) * 2.5f * intoxication_ * shakeScale_;
        const float swayPitch = std::sin(intoxicationTime_ * 1.8f + 0.7f) * 1.6f * intoxication_ * shakeScale_;
        camera->setRotation(yaw + swayYaw,
                            glm::clamp(pitch + swayPitch, MIN_PITCH, MAX_PITCH));
    }
}

void CameraController::processMouseMotion(const SDL_MouseMotionEvent& event) {
    if (!enabled || !camera) {
        return;
    }
    if (introActive) {
        return;
    }

    if (!mouseButtonDown) {
        return;
    }
    if (rotationSuppressed_) {
        return;
    }

    // Hold rotation until the cursor has actually gone somewhere, so a click
    // that means "target this" does not swing the view on the way.
    //
    // The same distance the click test uses, measured the same way: the net
    // offset from the press, not the length of the path taken to get there.
    if (!rotateArmed_) {
        dragOffsetX_ += static_cast<float>(event.xrel);
        dragOffsetY_ += static_cast<float>(event.yrel);
        const float threshold = core::clickDragThreshold();
        if (dragOffsetX_ * dragOffsetX_ + dragOffsetY_ * dragOffsetY_ <
            threshold * threshold) {
            return;
        }
        rotateArmed_ = true;  // past the dead-zone: this is a deliberate drag
    }

    // Directly update stored yaw/pitch (no lossy forward-vector derivation).
    // Build 8606 turns cameraYawMoveSpeed/800 degrees of yaw and
    // cameraPitchMoveSpeed/600 of pitch per pixel (defaults 180 and 90:
    // 0.225 and 0.15, at 1280x720 and at 800x600 alike; mouseSpeed has no
    // effect).
    yaw -= event.xrel * yawDegreesPerPixel();
    // SDL yrel > 0 = mouse moved DOWN. In WoW, mouse-down = look down = pitch decreases.
    // invertMouse flips to flight-sim style (mouse-down = look up).
    float invert = invertMouse ? 1.0f : -1.0f;
    pitch += event.yrel * pitchDegreesPerPixel() * invert;

    // WoW-style pitch limits: can look almost straight down, limited upward
    pitch = glm::clamp(pitch, MIN_PITCH, MAX_PITCH);

    camera->setRotation(yaw, pitch);
}

void CameraController::processMouseButton(const SDL_MouseButtonEvent& event) {
    if (!enabled && event.down) {
        return;
    }

    // Don't capture mouse when ImGui wants it (hovering UI windows), or when
    // FrameXML does. FrameXML draws into ImGui's background draw list, so
    // WantCaptureMouse is false over every frame it owns - pressing a bag item
    // turned the camera as well as pressing the item, and dragging one swung
    // the view around.
    bool uiWantsMouse = ImGui::GetIO().WantCaptureMouse || ui::frameXmlOwnsMouse();

    if (event.button == SDL_BUTTON_LEFT) {
        leftMouseDown = (event.down) && !uiWantsMouse;
        if (event.down && event.clicks >= 2) {
            autoRunning = false;
        }
    }
    if (event.button == SDL_BUTTON_RIGHT) {
        rightMouseDown = (event.down) && !uiWantsMouse;
    }

    bool anyDown = leftMouseDown || rightMouseDown;
    if (anyDown && !mouseButtonDown) {
        mouseCapturePosition_ = {event.x, event.y};
        // SDL3 captures per window rather than globally. The focused window is the one the player is pointing at, and relative mode means nothing for any other - so that is the one to ask.
        if (!SDL_SetWindowRelativeMouseMode(SDL_GetKeyboardFocus(), true)) {
            LOG_ERROR("Cannot enable mouse look: ", SDL_GetError());
            leftMouseDown = false;
            rightMouseDown = false;
            mouseButtonDown = false;
            return;
        }
        LOG_DEBUG("Mouse look: relative input enabled");
        // Throw away the delta the switch itself makes. Entering relative
        // mode hands over the movement since the last relative read, which on
        // a press is everything the cursor did on its way to the thing being
        // clicked - a whole screen's worth, delivered as one motion event
        // that cleared any dead-zone on its own.
        SDL_GetRelativeMouseState(nullptr, nullptr);
        // Arm the rotation dead-zone fresh for this press.
        rotateArmed_ = false;
        dragOffsetX_ = 0.0f;
        dragOffsetY_ = 0.0f;
    } else if (!anyDown && mouseButtonDown) {
        // SDL accumulates relative motion into its virtual pointer position.
        // WoW restores the press anchor when mouse-look ends. Warp while still
        // relative so this does not generate another camera-motion event.
        SDL_WarpMouseInWindow(SDL_GetKeyboardFocus(), mouseCapturePosition_.x, mouseCapturePosition_.y);
        SDL_SetWindowRelativeMouseMode(SDL_GetKeyboardFocus(), false);
        rotateArmed_ = false;
    }
    mouseButtonDown = anyDown;
}

void CameraController::releaseMouseCapture() {
    if (mouseButtonDown)
        SDL_WarpMouseInWindow(SDL_GetKeyboardFocus(), mouseCapturePosition_.x, mouseCapturePosition_.y);
    leftMouseDown = false;
    rightMouseDown = false;
    mouseButtonDown = false;
    rotateArmed_ = false;
    dragOffsetX_ = 0.0f;
    dragOffsetY_ = 0.0f;
    SDL_SetWindowRelativeMouseMode(SDL_GetKeyboardFocus(), false);
    SDL_ShowCursor();
}

void CameraController::setView(int view) {
    assert(view >= 1 && view <= kViewCount);
    currentView_ = view;
    userTargetDistance = views_[view].distance;
    zoomPending_ = 0.0f;
    pitch = glm::clamp(views_[view].pitch, MIN_PITCH, MAX_PITCH);
    // Back behind the character, as the original does.
    yaw = facingYaw;
    if (camera) camera->setRotation(yaw, pitch);
}

void CameraController::saveView(int view) {
    assert(view >= 1 && view <= kViewCount);
    views_[view] = {userTargetDistance, pitch};
    if (viewSavedCallback_ && view >= 2) viewSavedCallback_(view, views_[view].distance, views_[view].pitch);
}

void CameraController::resetView(int view) {
    assert(view >= 1 && view <= kViewCount);
    views_[view] = kDefaultViews[view];
    if (viewSavedCallback_ && view >= 2) viewSavedCallback_(view, views_[view].distance, views_[view].pitch);
    if (view == currentView_) setView(view);
}

void CameraController::resetAngles() {
    if (!camera) return;
    yaw = defaultYaw;
    facingYaw = defaultYaw;
    pitch = defaultPitch;
    camera->setRotation(yaw, pitch);
}

void CameraController::reset() {
    waterJumpAirborne_ = false;
    if (!camera) {
        return;
    }

    yaw = defaultYaw;
    facingYaw = defaultYaw;
    pitch = defaultPitch;
    verticalVelocity = 0.0f;
    grounded = true;
    swimming = false;
    sitting = false;
    autoRunning = false;
    noGroundTimer_ = 0.0f;
    autoUnstuckFired_ = false;

    // Clear edge-state so movement packets can re-start cleanly after respawn.
    wasMovingForward = false;
    wasMovingBackward = false;
    wasStrafingLeft = false;
    wasStrafingRight = false;
    wasTurningLeft = false;
    wasTurningRight = false;
    wasJumping = false;
    wasFalling = false;
    wasSwimming = false;
    moveForwardActive = false;
    moveBackwardActive = false;
    strafeLeftActive = false;
    strafeRightActive = false;
    turningLeftActive = false;
    turningRightActive = false;

    glm::vec3 spawnPos = defaultPosition;

    auto evalFloorAt = [&](float x, float y, float refZ) -> std::optional<float> {
        std::optional<float> terrainH;
        std::optional<float> wmoH;
        std::optional<float> m2H;
        if (terrainManager) {
            terrainH = terrainManager->getHeightAt(x, y);
        }
        // Probe from the highest of terrain, refZ (server position), and defaultPosition.z
        // so we don't miss WMO floors above terrain (e.g. Stormwind city surface).
        float floorProbeZ = std::max(terrainH.value_or(refZ), refZ);
        if (wmoRenderer) {
            wmoH = wmoRenderer->getFloorHeight(x, y, floorProbeZ + 4.0f);
        }
        if (m2Renderer && !externalFollow_) {
            m2H = m2Renderer->getFloorHeight(x, y, floorProbeZ + 4.0f);
        }
        auto h = selectReachableFloor(terrainH, wmoH, refZ, 16.0f);
        if (!h) {
            h = selectHighestFloor(terrainH, wmoH, m2H);
        }
        return h;
    };

    // The server supplies the occupied floor. Searching upward from terrain
    // can select a ceiling or roof when the player is inside a building.
    // Normal movement resolves contact once the collision meshes are loaded.
    if (onlineMode) {
        lastGroundZ = spawnPos.z;
        hasCachedFloor_ = false;

        camera->setRotation(yaw, pitch);
        glm::vec3 forward3D = camera->getForward();

        if (thirdPerson && followTarget) {
            *followTarget = spawnPos;
            currentDistance = userTargetDistance;
            collisionDistance = currentDistance;
            float mountedOffset = mounted_ ? mountHeightOffset_ : 0.0f;
            glm::vec3 pivot = spawnPos + glm::vec3(0.0f, 0.0f, followedPivotHeight() + mountedOffset);
            glm::vec3 camDir = -forward3D;
            glm::vec3 camPos = pivot + camDir * currentDistance;
            smoothedCamPos = camPos;
            camera->setPosition(camPos);
        } else {
            spawnPos.z += eyeHeight;
            smoothedCamPos = spawnPos;
            camera->setPosition(spawnPos);
        }

        LOG_INFO("Camera reset to server position (online mode)");
        return;
    }

    // Search nearby for a stable, non-steep spawn floor to avoid waterfall/ledge spawns.
    float bestScore = std::numeric_limits<float>::max();
    glm::vec3 bestPos = spawnPos;
    bool foundBest = false;
    constexpr float radiiOffline[] = {0.0f, 6.0f, 12.0f, 18.0f, 24.0f, 32.0f};
    const float* radii = radiiOffline;
    const int radiiCount = 6;
    constexpr int ANGLES = 16;
    constexpr float PI = core::coords::PI;
    for (int ri = 0; ri < radiiCount; ri++) {
        float r = radii[ri];
        int steps = (r <= 0.01f) ? 1 : ANGLES;
        for (int i = 0; i < steps; i++) {
            float a = (2.0f * PI * static_cast<float>(i)) / static_cast<float>(steps);
            float x = defaultPosition.x + r * std::cos(a);
            float y = defaultPosition.y + r * std::sin(a);
            auto h = evalFloorAt(x, y, defaultPosition.z);
            if (!h) continue;

            // Allow large downward snaps, but avoid snapping onto high roofs/odd geometry.
            constexpr float MAX_SPAWN_SNAP_UP = 16.0f;
            if (*h > defaultPosition.z + MAX_SPAWN_SNAP_UP) continue;

            float score = r * 0.02f;
            if (terrainManager) {
                // Penalize steep/unstable spots.
                int slopeSamples = 0;
                float slopeAccum = 0.0f;
                constexpr float off = 2.5f;
                const float dx[4] = {off, -off, 0.0f, 0.0f};
                const float dy[4] = {0.0f, 0.0f, off, -off};
                for (int s = 0; s < 4; s++) {
                    auto hn = terrainManager->getHeightAt(x + dx[s], y + dy[s]);
                    if (!hn) continue;
                    slopeAccum += std::abs(*hn - *h);
                    slopeSamples++;
                }
                if (slopeSamples > 0) {
                    score += (slopeAccum / static_cast<float>(slopeSamples)) * 2.0f;
                }
            }
            if (waterRenderer) {
                auto wh = waterRenderer->getWaterHeightAt(x, y);
                if (wh && *h < *wh - 0.2f) {
                    score += 8.0f;
                }
            }
            if (wmoRenderer) {
                const glm::vec3 from(x, y, *h + 0.20f);
                const bool insideWMO = wmoRenderer->isInsideWMO(x, y, *h + 1.5f, nullptr);

                // Prefer outdoors for default hearth-like spawn points (offline only).
                // In online mode, trust the server position even if inside a WMO.
                if (insideWMO && !onlineMode) {
                    score += 120.0f;
                }

                // Reject points embedded in nearby walls by probing tiny cardinal moves.
                int wallHits = 0;
                constexpr float probeStep = 0.85f;
                const glm::vec3 probes[4] = {
                    glm::vec3(x + probeStep, y, *h + 0.20f),
                    glm::vec3(x - probeStep, y, *h + 0.20f),
                    glm::vec3(x, y + probeStep, *h + 0.20f),
                    glm::vec3(x, y - probeStep, *h + 0.20f),
                };
                for (const auto& to : probes) {
                    glm::vec3 adjusted;
                    if (wmoRenderer->checkWallCollision(from, to, adjusted)) {
                        wallHits++;
                    }
                }
                if (wallHits >= 2) {
                    continue; // Likely wedged in geometry.
                }
                if (wallHits == 1) {
                    score += 30.0f;
                }

                // If the point is inside a WMO, ensure there is an easy escape path.
                // If almost all directions are blocked, treat it as invalid spawn.
                if (insideWMO) {
                    int blocked = 0;
                    constexpr int radialChecks = 12;
                    constexpr float radialDist = 2.2f;
                    for (int ri = 0; ri < radialChecks; ri++) {
                        float ang = (2.0f * PI * static_cast<float>(ri)) / static_cast<float>(radialChecks);
                        glm::vec3 to(
                            x + std::cos(ang) * radialDist,
                            y + std::sin(ang) * radialDist,
                            *h + 0.20f
                        );
                        glm::vec3 adjusted;
                        if (wmoRenderer->checkWallCollision(from, to, adjusted)) {
                            blocked++;
                        }
                    }
                    if (blocked >= 9) {
                        continue; // Enclosed by interior/wall geometry.
                    }
                    score += static_cast<float>(blocked) * 3.0f;
                }
            }

            if (score < bestScore) {
                bestScore = score;
                bestPos = glm::vec3(x, y, *h + 0.05f);
                foundBest = true;
            }
        }
    }
    if (foundBest) {
        spawnPos = bestPos;
        lastGroundZ = spawnPos.z - 0.05f;
    }

    camera->setRotation(yaw, pitch);
    glm::vec3 forward3D = camera->getForward();

    if (thirdPerson && followTarget) {
        // In follow mode, respawn the character (feet position), then place camera behind it.
        *followTarget = spawnPos;

        currentDistance = userTargetDistance;
        collisionDistance = currentDistance;

        float mountedOffset = mounted_ ? mountHeightOffset_ : 0.0f;
        glm::vec3 pivot = spawnPos + glm::vec3(0.0f, 0.0f, followedPivotHeight() + mountedOffset);
        glm::vec3 camDir = -forward3D;
        glm::vec3 camPos = pivot + camDir * currentDistance;
        smoothedCamPos = camPos;
        camera->setPosition(camPos);
    } else {
        // Free-fly mode keeps camera eye-height above ground.
        if (foundBest) {
            spawnPos.z += eyeHeight;
        }
        smoothedCamPos = spawnPos;
        camera->setPosition(spawnPos);
    }

    LOG_INFO("Camera reset to default position");
}

void CameraController::teleportTo(const glm::vec3& pos) {
    waterJumpAirborne_ = false;
    if (!camera) return;

    verticalVelocity = 0.0f;
    grounded = true;
    swimming = false;
    sitting = false;
    lastGroundZ = pos.z;
    noGroundTimer_ = 0.0f;  // Reset grace period so terrain has time to stream
    autoUnstuckFired_ = false;
    continuousFallTime_ = 0.0f;

    // Invalidate active WMO group so it's re-detected at new position
    if (wmoRenderer) {
        wmoRenderer->updateActiveGroup(pos.x, pos.y, pos.z + 1.0f);
    }

    if (thirdPerson && followTarget) {
        *followTarget = pos;
        camera->setRotation(yaw, pitch);
        glm::vec3 forward3D = camera->getForward();
        float mountedOffset = mounted_ ? mountHeightOffset_ : 0.0f;
        glm::vec3 pivot = pos + glm::vec3(0.0f, 0.0f, followedPivotHeight() + mountedOffset);
        glm::vec3 camDir = -forward3D;
        glm::vec3 camPos = pivot + camDir * currentDistance;
        smoothedCamPos = camPos;
        camera->setPosition(camPos);
    } else {
        glm::vec3 camPos = pos + glm::vec3(0.0f, 0.0f, eyeHeight);
        smoothedCamPos = camPos;
        camera->setPosition(camPos);
    }

    LOG_INFO("Teleported to (", pos.x, ", ", pos.y, ", ", pos.z, ")");
}

bool CameraController::groundNotStreamedYet(float x, float y) const {
    return terrainManager != nullptr && terrainManager->isGroundPendingAt(x, y);
}

void CameraController::applyLookDelta(float dxPixels, float dyPixels) {
    applyLookDegrees(dxPixels * yawDegreesPerPixel(), dyPixels * pitchDegreesPerPixel());
}

void CameraController::applyLookDegrees(float yawDegrees, float pitchDegrees) {
    if (!enabled || !camera || introActive) return;

    yaw -= yawDegrees;
    const float invert = invertMouse ? 1.0f : -1.0f;
    pitch += pitchDegrees * invert;
    pitch = glm::clamp(pitch, MIN_PITCH, MAX_PITCH);
    camera->setRotation(yaw, pitch);
}

void CameraController::processMouseWheel(float delta) {
    // A notch is the CAMERAZOOMIN / CAMERAZOOMOUT binding, whose body in
    // Bindings.xml is CameraZoomIn(1.0) / CameraZoomOut(1.0): one yard.
    const float yards = -delta;
    if (yards == 0.0f) return;
    zoomPending_ = (yards > 0.0f) == (zoomPending_ > 0.0f) ? zoomPending_ + yards : yards;
}

void CameraController::advanceZoom(float deltaTime) {
    const float step = distanceMoveSpeed_ * deltaTime;
    if (zoomPending_ != 0.0f) {
        const float direction = zoomPending_ > 0.0f ? 1.0f : -1.0f;
        const float remaining = std::abs(zoomPending_) - step;
        zoomPending_ = remaining > 0.0f ? direction * remaining : 0.0f;
        const float wanted = userTargetDistance + direction * step;
        userTargetDistance = glm::clamp(wanted, MIN_DISTANCE, std::max(MIN_DISTANCE, maxZoomDistance()));
        if (userTargetDistance != wanted) {
            // A zoom past a lowered limit lands on it at once: at 30 yards,
            // factor 2 -> 1 then CameraZoomOut(1) read 15.0 on the next frame.
            zoomPending_ = 0.0f;
            currentDistance = userTargetDistance;
        }
    }
    // Distances set directly (barber chair, camera reset) travel the same way.
    const float gap = userTargetDistance - currentDistance;
    currentDistance = std::abs(gap) <= step ? userTargetDistance
                                            : currentDistance + (gap > 0.0f ? step : -step);
}

float CameraController::easedCollisionLimit(std::optional<float> obstruction, float deltaTime) {
    const float limit = obstruction.value_or(std::numeric_limits<float>::infinity());
    // Geometry closing in or holding still restarts the ease; only geometry
    // falling away lets it run. Held 1.5 s against a wall then turned clear,
    // the original still took the full 2 s back; turning away gradually, the
    // ease ran from the start of the turn.
    if (smoothedCollisionDist_ >= 0.0f && limit <= smoothedCollisionDist_) {
        smoothedCollisionDist_ = limit;
        collisionEaseFrom_ = limit;
        collisionEaseTime_ = 0.0f;
        return limit;
    }
    if (smoothedCollisionDist_ < 0.0f && obstruction) {
        // First frame with a collision source: start against the obstruction.
        smoothedCollisionDist_ = limit;
        collisionEaseFrom_ = limit;
        collisionEaseTime_ = 0.0f;
        return limit;
    }
    collisionEaseTime_ = std::min(collisionEaseTime_ + deltaTime, kCollisionEaseSeconds);
    if (collisionEaseTime_ >= kCollisionEaseSeconds) {
        smoothedCollisionDist_ = limit;
        return limit;
    }
    const float progress = collisionEaseTime_ / kCollisionEaseSeconds;
    const float eased = collisionEaseFrom_ + (currentDistance - collisionEaseFrom_) *
        0.5f * (1.0f - std::cos(glm::pi<float>() * progress));
    smoothedCollisionDist_ = std::min(limit, eased);
    return smoothedCollisionDist_;
}

void CameraController::setFollowTarget(glm::vec3* target) {
    followTarget = target;
    if (target) {
        thirdPerson = true;
        LOG_INFO("Third-person camera enabled");
    } else {
        thirdPerson = false;
        LOG_INFO("Free-fly camera enabled");
    }
}

bool CameraController::isMoving() const {
    if (!enabled || !camera) {
        return false;
    }
    if (externalMoving_ || serverDrivenMoving_) return true;
    return moveForwardActive || moveBackwardActive || strafeLeftActive || strafeRightActive || autoRunning;
}

void CameraController::clearMovementInputs() {
    moveForwardActive = false;
    moveBackwardActive = false;
    strafeLeftActive = false;
    strafeRightActive = false;
    turningLeftActive = false;
    turningRightActive = false;
    autoRunning = false;
}

bool CameraController::isSprinting() const {
    return enabled && camera && runPace;
}


void CameraController::applyKnockBack(float vcos, float vsin, float hspeed, float vspeed) {
    // The server sends (vcos, vsin) as the 2D direction vector in server/wire
    // coordinate space.  After the server→canonical→render swaps, the direction
    // in render space is simply (vcos, vsin) - the two swaps cancel each other.
    knockbackHorizVel_ = glm::vec2(vcos, vsin) * hspeed;
    knockbackActive_ = true;

    // vspeed in the wire packet is negative when the server wants to launch the
    // player upward (matches TrinityCore: data << float(-speedZ)).  Negate it
    // here to obtain the correct upward initial velocity.
    verticalVelocity = -vspeed;
    grounded = false;
    coyoteTimer = 0.0f;
    jumpBufferTimer = 0.0f;

    // Notify the server that the player left the ground so the FALLING flag is
    // set in subsequent movement heartbeats.  The normal jump detection
    // (nowJump && grounded) does not fire during a server-driven knockback.
    if (movementCallback) {
        movementCallback(static_cast<uint32_t>(game::Opcode::MSG_MOVE_JUMP));
    }
}

} // namespace rendering
} // namespace wowee
