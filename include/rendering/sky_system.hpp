#pragma once

#include <algorithm>
#include <memory>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>
#include "rendering/night_sky_math.hpp"

namespace wowee {
namespace rendering {

class Camera;
class VkContext;
class Skybox;
class Celestial;
class StarField;
class Clouds;
class LensFlare;
class LightingManager;

/**
 * Sky rendering parameters (extracted from LightingManager)
 */
struct SkyParams {
    // Sun/moon positioning
    glm::vec3 directionalDir{0.0f, -1.0f, 0.3f};
    glm::vec3 sunColor{1.0f, 1.0f, 0.9f};

    // Sky colors (for skybox tinting/blending)
    glm::vec3 skyTopColor{0.5f, 0.7f, 1.0f};
    glm::vec3 skyMiddleColor{0.7f, 0.85f, 1.0f};
    glm::vec3 skyBand1Color{0.9f, 0.95f, 1.0f};
    glm::vec3 skyBand2Color{1.0f, 0.98f, 0.9f};
    glm::vec3 skySmogColor{0.85f, 0.88f, 0.92f};
    glm::vec3 fogColor{0.5f, 0.6f, 0.7f};
    glm::vec3 cloudColor{0.9f, 0.95f, 1.0f};
    glm::vec3 cloudEmissiveColor{0.2f, 0.3f, 0.45f};
    glm::vec3 cloudLayer2Color{0.0f, 0.0f, 0.0f};
    float cloudLayer2Weight = 0.0f;

    // Atmospheric effects
    float celestialGlowThrough = 1.0f; // 0-1 (LightFloatBand 2)
    float cloudDensity = 0.0f;         // 0-1 (LightFloatBand 3: sky smog / cloud weight)
    float fogDensity = 0.0f;        // 0-1
    float horizonGlow = 0.3f;       // 0-1
    float weatherIntensity = 0.0f;  // 0-1 (rain/snow intensity, attenuates lens flare)
    // How much of the line from the eye to the sun is blocked, 0 clear to 1
    // solid. The lens flare had no such input: it asked only whether the sun
    // was in front of the camera and on screen, so a hillside, a building or a
    // ceiling between the two changed nothing and the flare hung over the
    // terrain that was covering it.
    float sunOcclusion = 0.0f;

    // Time
    float timeOfDay = 12.0f;    // 0-24 hours
    float gameTime = -1.0f;     // Server game time, hours since midnight (-1 = use fallback)

    // Skybox selection (future: from LightSkybox.dbc)
    uint32_t skyboxModelId = 0;
    bool skyboxHasStars = false;  // Does loaded skybox include baked stars?
    bool useOriginalSkybox = false; // Original camera-centered client M2 is active
};

/// Effective procedural cloud density in [0, 1] for Azeroth/Eversong skies.
///
/// Native TBC 2.4.3 CSkyScene renders only the untextured vertex-band sky dome
/// and Stars.m2 on clear night skies (no procedural FBM cloud dome), while
/// preserving daytime and dusk DBC clouds (until SkyTop actually darkens into
/// night, e.g. 21:00..22:00 in Orgrimmar/Durotar/Elwynn) and storm overcast
/// clouds at any hour.
inline float effectiveSkyCloudDensity(bool useOriginalSkybox,
                                      float dbcCloudDensity,
                                      float timeOfDayHours,
                                      float weatherIntensity,
                                      const glm::vec3& skyTopColor = glm::vec3(0.0f)) {
    if (useOriginalSkybox) return 0.0f;
    const float clampedDensity = std::clamp(dbcCloudDensity, 0.0f, 1.0f);
    const float darkSky = 1.0f - skySmoothstep(0.14f, 0.30f, skyColorLuminance(skyTopColor));
    // Clear-night Azeroth zones author LightFloatBand[3] <= 0.50 and show only
    // stars at night; permanent-overcast zones (Karazhan 386 / Deadwind Pass 19
    // / Duskwood 57 at 0.70, Plaguelands 0.68, Blasted Lands 0.85, Storm 0.95)
    // keep their authored cloud deck at all hours.
    const float overcastPersistence = skySmoothstep(0.55f, 0.65f, clampedDensity);
    const float clearNightSuppression =
        starFieldTimeVisibility(timeOfDayHours) * darkSky * (1.0f - overcastPersistence);
    const float daylightFactor = 1.0f - clearNightSuppression;
    const float clearSkyDensity = clampedDensity * daylightFactor;
    const float weatherBoost = (weatherIntensity > 0.05f) ? (weatherIntensity * 0.4f) : 0.0f;
    return std::min(1.0f, clearSkyDensity + weatherBoost);
}

/**
 * Unified sky rendering system
 *
 * Coordinates skybox (authoritative), celestial bodies (sun + 2 moons),
 * and fallback procedural stars. Driven by lighting system data.
 *
 * Architecture:
 * - Skybox is PRIMARY (includes baked stars from M2 models)
 * - Celestial renders sun + White Lady + Blue Child
 * - StarField is DEBUG/FALLBACK only (disabled when skybox has stars)
 */
class SkySystem {
public:
    SkySystem();
    ~SkySystem();

    /**
     * Initialize sky system components.
     * @param ctx            Vulkan context (required for Vulkan renderers)
     * @param perFrameLayout Descriptor set layout for set 0 (camera UBO)
     */
    bool initialize(VkContext* ctx = nullptr, VkDescriptorSetLayout perFrameLayout = VK_NULL_HANDLE);
    void shutdown();

    /**
     * Update sky system (time, moon phases, etc.)
     */
    void update(float deltaTime);

    /**
     * Render complete sky.
     * @param cmd         Active Vulkan command buffer
     * @param perFrameSet Per-frame descriptor set (set 0, camera UBO)
     * @param camera      Camera for legacy sub-renderers (lens flare, etc.)
     * @param params      Sky parameters from lighting system
     */
    void render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet,
                const Camera& camera, const SkyParams& params);

    /**
     * Enable/disable procedural stars (DEBUG/FALLBACK)
     * Default: OFF (stars come from skybox)
     */
    void setProceduralStarsEnabled(bool enabled) { proceduralStarsEnabled_ = enabled; }
    [[nodiscard]] bool isProceduralStarsEnabled() const { return proceduralStarsEnabled_; }

    /**
     * Enable/disable debug sky mode (forces procedural stars even with skybox)
     */
    void setDebugSkyMode(bool enabled) { debugSkyMode_ = enabled; }
    [[nodiscard]] bool isDebugSkyMode() const { return debugSkyMode_; }

    /**
     * Get sun position in world space (for lens flare, shadows, etc.)
     */
    [[nodiscard]] glm::vec3 getSunPosition(const SkyParams& params) const;

    /**
     * Enable/disable moon phase cycling
     */
    void setMoonPhaseCycling(bool enabled);

    void setBlueChildPhase(float phase);

    [[nodiscard]] float getBlueChildPhase() const;

    // Component accessors (for direct control if needed)
    [[nodiscard]] Skybox*    getSkybox()    const { return skybox_.get(); }
    [[nodiscard]] Celestial* getCelestial() const { return celestial_.get(); }
    [[nodiscard]] StarField* getStarField() const { return starField_.get(); }
    [[nodiscard]] Clouds*    getClouds()    const { return clouds_.get(); }
    [[nodiscard]] LensFlare* getLensFlare() const { return lensFlare_.get(); }

private:
    std::unique_ptr<Skybox>    skybox_;      // Authoritative sky
    std::unique_ptr<Celestial> celestial_;   // Sun + 2 moons
    std::unique_ptr<StarField> starField_;   // Fallback procedural stars
    std::unique_ptr<Clouds>    clouds_;      // Cloud layer
    std::unique_ptr<LensFlare> lensFlare_;   // Sun lens flare

    bool proceduralStarsEnabled_ = false;
    bool debugSkyMode_ = false;
    bool initialized_ = false;
};

} // namespace rendering
} // namespace wowee
