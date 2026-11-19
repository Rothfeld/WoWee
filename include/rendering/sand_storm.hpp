#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include "rendering/vk_context.hpp"
#include "rendering/weather_mist.hpp"

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering {

class Camera;

/**
 * The sandstorm weather: SMSG_WEATHER states 22, 41 and 42, whose
 * Weather.dbc rows have EffectType 3 and no EffectTexture. The original draws
 * it as two layers, both in the fog colour:
 *
 *  - grains: single points moved by Shaders\Vertex\sand.bls (game data), a
 *    straight line from a start position at a constant velocity, faded in over
 *    its first 0.2 s and out over its last 0.2 s, sized by distance;
 *  - mist: Textures\Weather\WeatherMistGrainy01.blp puffs.
 *
 * Grains stream toward the camera from where it looks, horizontally, and
 * travel with it: running does not change how fast they come (measured on
 * 2.4.3, `_experiments/20261118T171000Z.aq20_sandstorm` in the workspace).
 * The puffs are given the same motion; theirs was not separable from their
 * fading in the captures. The constants in sand_storm.cpp say which
 * measurement each comes from and which are chosen.
 */
class SandStorm {
public:
    SandStorm() = default;
    ~SandStorm();

    [[nodiscard]] bool isInitialized() const { return vkCtx_ != nullptr; }
    [[nodiscard]] bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout,
                                  pipeline::AssetManager* assetManager);
    void shutdown();
    void recreatePipelines();

    /// `targetIntensity` is the server's grade (SMSG_WEATHER's float) while a
    /// sandstorm is reported and the camera is outdoors, else 0. The drawn
    /// intensity moves toward it at a fixed rate.
    void update(const Camera& camera, float deltaTime, float targetIntensity);
    void render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet);

    /// The game's Weather Detail setting, as for rain and snow.
    void setDensityScale(float scale) { densityScale_ = glm::clamp(scale, 0.0f, 1.0f); }
    [[nodiscard]] float intensity() const { return intensity_; }
    [[nodiscard]] size_t grainCount() const { return grains_.size(); }
    [[nodiscard]] size_t puffCount() const { return mist_.puffCount(); }

private:
    struct Grain {          // one vertex, as sand.bls reads it
        glm::vec3 start;    // camera-relative, at birth
        float birth;
        glm::vec3 velocity; // yards per second
        float death;
    };
    bool createPipelines();
    bool createBuffers();
    void spawnGrain(const glm::vec3& forward, const glm::vec3& right);

    VkContext* vkCtx_ = nullptr;
    VkDescriptorSetLayout perFrameLayout_ = VK_NULL_HANDLE;
    VkPipeline grainPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout grainLayout_ = VK_NULL_HANDLE;
    WeatherMist mist_;

    struct FrameBuffers {
        ::VkBuffer grains = VK_NULL_HANDLE;
        VmaAllocation grainsAlloc = VK_NULL_HANDLE;
        void* grainsMapped = nullptr;
    };
    std::array<FrameBuffers, MAX_FRAMES_IN_FLIGHT> frames_{};

    std::vector<Grain> grains_;
    float time_ = 0.0f;
    float intensity_ = 0.0f;
    float densityScale_ = 1.0f;
    float grainBudget_ = 0.0f;  // fractional grains owed to the spawn rate
};

} // namespace rendering
} // namespace wowee
