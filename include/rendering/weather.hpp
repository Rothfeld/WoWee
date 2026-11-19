#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include "rendering/vk_context.hpp"
#include "rendering/weather_mist.hpp"
#include "rendering/weather_texture.hpp"

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering {

class Camera;

/**
 * Rain and snow, drawn as 2.4.3 draws them (measurements and data sources in
 * weather.cpp):
 *
 *  - rain: thin triangles from Shaders\Vertex\rain.bls (2 yards long, 0.1 yard
 *    wide, RainDrop01.blp) falling from high above the camera, blended Mod2x
 *    so each lightens the scene behind it in the scene's own colour; a
 *    RainDropSplash01.blp splash (patter.bls) where each lands; mist puffs.
 *  - snow: SnowFlake01.blp points moved by Shaders\Vertex\snowpoint.bls;
 *    mist puffs.
 *
 * Sandstorms are SandStorm. Drops and flakes are world-placed around the
 * camera; mist travels with the camera.
 */
class Weather {
public:
    enum class Type {
        NONE,
        RAIN,
        SNOW,
        STORM
    };

    /// Highest walkable surface under (x, y) at or below z: terrain or a
    /// building's floor. Rain lands, and splashes, on it.
    using GroundQuery = std::function<std::optional<float>(float x, float y, float z)>;

    Weather();
    ~Weather();

    /// Needs the asset manager for the weather textures, so it is called once
    /// assets are available. Returns false when anything failed; the weather
    /// is then not drawn.
    [[nodiscard]] bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout,
                                  pipeline::AssetManager* assets);
    [[nodiscard]] bool isInitialized() const { return vkCtx_ != nullptr; }
    void recreatePipelines();
    void shutdown();

    void setGroundQuery(GroundQuery query) { groundQuery_ = std::move(query); }

    void update(const Camera& camera, float deltaTime);
    void render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet);

    void setWeatherType(Type type);
    [[nodiscard]] Type getWeatherType() const { return weatherType; }

    /// SMSG_WEATHER's grade (0..1).
    void setIntensity(float intensity);
    [[nodiscard]] float getIntensity() const { return intensity; }

    /// The game's Weather Detail setting (CVar weatherDensity, 0 to 3; 2 when
    /// unset): a quality knob, not a forecast. 2.4.3 draws (2 * level + 1) / 5
    /// of the level-2 amount: rain streaks counted over the same sky with the
    /// same rain were 105, 300, 500 and 700 a frame at levels 0 to 3, so 0
    /// still rains. Every count in weather.cpp is the level-2 amount.
    void setDensityLevel(int level);
    [[nodiscard]] float densityScale() const { return densityScale_; }

    /// Off in indoor WMO groups (MOGP 0x2000): drops, flakes and mist go
    /// at once and start over from the sky when the camera comes out.
    void setEnabled(bool enabled);
    [[nodiscard]] bool isEnabled() const { return enabled; }

    /// Live drops, splashes and flakes.
    [[nodiscard]] int getParticleCount() const;

    /**
     * @brief Zone weather configuration
     * Provides default weather per zone for single-player mode.
     * When connected to a server, SMSG_WEATHER overrides these.
     */
    struct ZoneWeather {
        Type type = Type::NONE;
        float minIntensity = 0.0f;     // Min intensity (varies over time)
        float maxIntensity = 0.0f;     // Max intensity
        float probability = 0.0f;      // Chance of weather being active (0-1)
    };
    void setZoneWeather(uint32_t zoneId, Type type, float minIntensity, float maxIntensity, float probability);
    void updateZoneWeather(uint32_t zoneId, float deltaTime);
    void initializeZoneWeatherDefaults();

private:
    struct Drop {
        glm::vec3 head;     // world
        float groundZ;      // where it stops
        float landedAge;    // < 0 while falling
    };
    struct Splash {
        glm::vec3 position; // world, where a drop landed
        float age;
    };
    struct Flake {
        glm::vec3 position; // world
        float groundZ;
        float age;
        float landedAge;    // < 0 while falling
    };
    struct SpriteVertex {   // weather_mod2x.vert.glsl
        glm::vec4 position; // camera-relative
        glm::vec2 uv;
    };
    struct FlakeVertex {    // weather_snow.vert.glsl
        glm::vec4 positionAlpha;  // camera-relative, alpha
    };

    bool createPipelines();
    void clearParticles();
    float groundAt(float x, float y);
    /// Born at the top of the box, or (`anyHeight`) anywhere on its way down.
    void spawnDrop(const glm::vec3& camera, bool anyHeight);
    void spawnFlake(const glm::vec3& camera, bool anyHeight);
    void updateRain(const Camera& camera, float deltaTime, float amount);
    void updateSnow(const Camera& camera, float deltaTime, float amount);

    VkContext* vkCtx_ = nullptr;
    VkDescriptorSetLayout perFrameLayout_ = VK_NULL_HANDLE;
    VkPipeline mod2xPipeline_ = VK_NULL_HANDLE;
    VkPipeline snowPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout mod2xLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout snowLayout_ = VK_NULL_HANDLE;
    WeatherTexture dropTexture_;
    WeatherTexture splashTexture_;
    WeatherTexture flakeTexture_;
    WeatherMist mist_;

    struct FrameBuffers {
        ::VkBuffer sprites = VK_NULL_HANDLE;
        VmaAllocation spritesAlloc = VK_NULL_HANDLE;
        void* spritesMapped = nullptr;
        ::VkBuffer flakes = VK_NULL_HANDLE;
        VmaAllocation flakesAlloc = VK_NULL_HANDLE;
        void* flakesMapped = nullptr;
    };
    std::array<FrameBuffers, MAX_FRAMES_IN_FLIGHT> frames_{};

    GroundQuery groundQuery_;
    std::unordered_map<int64_t, float> groundCache_;  // two-yard cells
    float groundCacheZ_ = 0.0f;              // the height lookups search down from
    std::vector<Drop> drops_;
    std::vector<Splash> splashes_;
    std::vector<Flake> flakes_;
    std::vector<SpriteVertex> dropVertices_;
    std::vector<SpriteVertex> splashVertices_;
    std::vector<FlakeVertex> flakeVertices_;
    float rainBudget_ = 0.0f;   // fractional drops owed to the spawn rate
    float snowBudget_ = 0.0f;   // fractional flakes
    float logTimer_ = 0.0f;

    bool enabled = false;
    Type weatherType = Type::NONE;
    float intensity = 0.0f;
    float densityScale_ = 1.0f;

    // Zone-based weather
    std::unordered_map<uint32_t, ZoneWeather> zoneWeatherTable_;
    uint32_t currentWeatherZone_ = 0;
    float zoneWeatherTimer_ = 0.0f;         // Time accumulator for weather cycling
    float zoneWeatherCycleDuration_ = 0.0f;  // Current cycle length
    bool zoneWeatherActive_ = false;         // Is zone weather currently active?
    float targetIntensity_ = 0.0f;           // Target intensity for smooth transitions
    bool zoneWeatherInitialized_ = false;
};

} // namespace rendering
} // namespace wowee
