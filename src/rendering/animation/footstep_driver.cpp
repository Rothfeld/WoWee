#include "rendering/animation/footstep_driver.hpp"
#include "rendering/renderer.hpp"
#include "rendering/camera_controller.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/terrain_manager.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/water_renderer.hpp"
#include "rendering/swim_effects.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/footstep_manager.hpp"
#include "audio/mount_sound_manager.hpp"
#include "rendering/footprint_renderer.hpp"
#include "core/logger.hpp"
#include "audio/movement_sound_manager.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <initializer_list>

namespace wowee {
namespace rendering {

// ── Footstep event detection (moved from AnimationController) ────────────────

namespace {

bool containsAnyToken(const std::string& text, std::initializer_list<const char*> tokens) {
    for (const char* token : tokens) {
        if (text.find(token) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

bool FootstepDriver::EventTracker::shouldTriggerFootstepEvent(uint32_t animationId, float animationTimeMs, float animationDurationMs,
                                                const std::vector<uint32_t>* eventTimesMs) {
    if (animationDurationMs <= 1.0f) {
        initialized = false;
        return false;
    }

    float wrappedTime = animationTimeMs;
    while (wrappedTime >= animationDurationMs) {
        wrappedTime -= animationDurationMs;
    }
    if (wrappedTime < 0.0f) wrappedTime += animationDurationMs;
    float norm = wrappedTime / animationDurationMs;

    if (animationId != lastAnimationId) {
        lastAnimationId = animationId;
        lastNormTime = norm;
        initialized = true;
        return false;
    }

    if (!initialized) {
        initialized = true;
        lastNormTime = norm;
        return false;
    }

    auto crossed = [&](float eventNorm) {
        if (lastNormTime <= norm) {
            return lastNormTime < eventNorm && eventNorm <= norm;
        }
        return lastNormTime < eventNorm || eventNorm <= norm;
    };

    // Authored $FSD footfall keyframes from the M2. A sequence without them
    // makes no footstep sound.
    bool trigger = false;
    if (eventTimesMs) {
        for (uint32_t t : *eventTimesMs) {
            float eventNorm = std::min(static_cast<float>(t) / animationDurationMs, 0.999f);
            if (crossed(eventNorm)) {
                trigger = true;
                break;
            }
        }
    }
    lastNormTime = norm;
    return trigger;
}

bool FootstepDriver::shouldTriggerFootstepEvent(uint32_t animationId, float animationTimeMs, float animationDurationMs,
                                                const std::vector<uint32_t>* eventTimesMs) {
    return playerFootsteps_.shouldTriggerFootstepEvent(animationId, animationTimeMs, animationDurationMs,
                                                       eventTimesMs);
}

audio::FootstepSurface FootstepDriver::resolveFootstepSurface(Renderer* renderer) const {
    auto* cameraController = renderer->getCameraController();
    if (!cameraController || !cameraController->isThirdPerson()) {
        return audio::FootstepSurface::STONE;
    }

    const glm::vec3& p = renderer->getCharacterPosition();

    float distSq = glm::dot(p - cachedFootstepPosition_, p - cachedFootstepPosition_);
    if (distSq < 2.25f && cachedFootstepUpdateTimer_ < 0.5f) {
        return cachedFootstepSurface_;
    }

    cachedFootstepPosition_ = p;
    cachedFootstepUpdateTimer_ = 0.0f;

    if (cameraController->isSwimming()) {
        cachedFootstepSurface_ = audio::FootstepSurface::WATER;
        return audio::FootstepSurface::WATER;
    }

    auto* waterRenderer = renderer->getWaterRenderer();
    if (waterRenderer) {
        auto waterH = waterRenderer->getWaterHeightAt(p.x, p.y);
        if (waterH && p.z < (*waterH + 0.25f)) {
            cachedFootstepSurface_ = audio::FootstepSurface::WATER;
            return audio::FootstepSurface::WATER;
        }
    }

    auto* wmoRenderer = renderer->getWMORenderer();
    auto* terrainManager = renderer->getTerrainManager();
    if (wmoRenderer) {
        auto wmoFloor = wmoRenderer->getFloorHeight(p.x, p.y, p.z + 1.5f);
        auto terrainFloor = terrainManager ? terrainManager->getHeightAt(p.x, p.y) : std::nullopt;
        const bool standingOnWmo = wmoFloor && std::abs(*wmoFloor - p.z) <= 2.5f;
        if (standingOnWmo && (cameraController->isInsideWMO() ||
                             !terrainFloor || *wmoFloor >= *terrainFloor - 0.1f)) {
            cachedFootstepSurface_ = audio::FootstepSurface::STONE;
            return audio::FootstepSurface::STONE;
        }
    }

    audio::FootstepSurface surface = audio::FootstepSurface::STONE;

    if (terrainManager) {
        auto texture = terrainManager->getDominantTextureAt(p.x, p.y);
        if (texture) {
            std::string t = *texture;
            for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (containsAnyToken(t, {"snow", "ice"})) surface = audio::FootstepSurface::SNOW;
            else if (containsAnyToken(t, {"farm", "field", "soil", "plow", "crop", "wheat", "dirt", "mud", "sand"})) surface = audio::FootstepSurface::DIRT;
            else if (containsAnyToken(t, {"grass", "moss", "leaf"})) surface = audio::FootstepSurface::GRASS;
            else if (containsAnyToken(t, {"wood", "timber"})) surface = audio::FootstepSurface::WOOD;
            else if (containsAnyToken(t, {"metal", "iron"})) surface = audio::FootstepSurface::METAL;
            else if (containsAnyToken(t, {"stone", "rock", "cobble", "brick"})) surface = audio::FootstepSurface::STONE;
        }
    }

    cachedFootstepSurface_ = surface;
    return surface;
}

void FootstepDriver::playWaterStepExtras(Renderer* renderer) const {
    if (renderer->getAudioCoordinator()->getMovementSoundManager()) {
        renderer->getAudioCoordinator()->getMovementSoundManager()->playWaterFootstep(audio::MovementSoundManager::CharacterSize::MEDIUM);
    }
    auto* swimEffects = renderer->getSwimEffects();
    auto* waterRenderer = renderer->getWaterRenderer();
    if (swimEffects && waterRenderer) {
        const glm::vec3& characterPosition = renderer->getCharacterPosition();
        auto wh = waterRenderer->getWaterHeightAt(characterPosition.x, characterPosition.y);
        if (wh) {
            swimEffects->spawnFootSplash(characterPosition, *wh);
        }
    }
}

// ── Footstep update (moved from AnimationController::updateFootsteps) ────────

void FootstepDriver::update(float deltaTime, Renderer* renderer,
                             bool mounted, uint32_t mountInstanceId, bool taxiFlight,
                             bool isFootstepState) {
    auto* footstepManager = renderer->getAudioCoordinator()->getFootstepManager();
    if (!footstepManager) return;

    auto* characterRenderer = renderer->getCharacterRenderer();
    auto* cameraController = renderer->getCameraController();
    uint32_t characterInstanceId = renderer->getCharacterInstanceId();

    footstepManager->update(deltaTime);
    cachedFootstepUpdateTimer_ += deltaTime;

    bool canPlayFootsteps = characterRenderer && characterInstanceId > 0 &&
        cameraController && cameraController->isThirdPerson() &&
        cameraController->isGrounded() && !cameraController->isSwimming();

    if (canPlayFootsteps && mounted && mountInstanceId > 0 && !taxiFlight) {
        // The mount's own footfall events and its own footstep sounds.
        uint32_t animId = 0;
        float animTimeMs = 0.0f, animDurationMs = 0.0f;
        if (cameraController->isMoving() &&
            characterRenderer->getAnimationState(mountInstanceId, animId, animTimeMs, animDurationMs) &&
            mountFootsteps_.shouldTriggerFootstepEvent(animId, animTimeMs, animDurationMs,
                                                       characterRenderer->getFootstepEventTimes(mountInstanceId))) {
            playStep(renderer, mountInstanceId);
        }
        playerFootsteps_.reset();
    } else if (canPlayFootsteps && isFootstepState) {
        uint32_t animId = 0;
        float animTimeMs = 0.0f;
        float animDurationMs = 0.0f;
        if (characterRenderer->getAnimationState(characterInstanceId, animId, animTimeMs, animDurationMs) &&
            shouldTriggerFootstepEvent(animId, animTimeMs, animDurationMs,
                                       characterRenderer->getFootstepEventTimes(characterInstanceId))) {
            playStep(renderer, characterInstanceId);
        }
        mountFootsteps_.reset();
    } else {
        playerFootsteps_.reset();
        mountFootsteps_.reset();
    }
}

void FootstepDriver::playStep(Renderer* renderer, uint32_t instanceId) {
    auto* footstepManager = renderer->getAudioCoordinator()->getFootstepManager();
    auto* footprints = renderer->getFootprintRenderer();
    const glm::vec3& position = renderer->getCharacterPosition();
    const bool wading = isWading(renderer);
    if (wading) playWaterStepExtras(renderer);
    const auto terrainType = footprints ? footprints->terrainTypeAt(position) : std::nullopt;
    if (!terrainType) {
        LOG_DEBUG("Footstep: no TerrainType under the character at ", position.x, ",", position.y,
                  ",", position.z, "; no sound");
        return;
    }
    footstepManager->playFootstep(renderer->getCharacterRenderer()->getInstanceDisplayId(instanceId),
                                  *terrainType, wading);
}

bool FootstepDriver::isWading(Renderer* renderer) const {
    auto* waterRenderer = renderer->getWaterRenderer();
    if (!waterRenderer) return false;
    const glm::vec3& p = renderer->getCharacterPosition();
    const auto waterHeight = waterRenderer->getWaterHeightAt(p.x, p.y);
    return waterHeight && p.z < *waterHeight + 0.25f;
}

} // namespace rendering
} // namespace wowee
