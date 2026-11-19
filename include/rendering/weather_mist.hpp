#pragma once

#include <array>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include "rendering/vk_context.hpp"
#include "rendering/weather_texture.hpp"

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering {

class Camera;

/**
 * Weather mist: camera-facing puffs of one mist texture, in the fog colour,
 * streaming at the camera from the horizon ahead of it. Wow.exe has one class
 * for this (RTTI `Mists::Mist`) and names two textures for it, both the same
 * picture: `textures\Weather\SnowMist01.blp` with the rain and snow names and
 * `textures\Weather\WeatherMistGrainy01.blp` with the sandstorm's. Each weather
 * passes its own shape and count; the motion and slab come from the sandstorm
 * measurements (sand_storm.cpp), the rain counts from weather.cpp's.
 */
class WeatherMist {
public:
    /// Where the puffs live, relative to the camera's horizontal forward
    /// direction, and how many there are at full intensity.
    struct Shape {
        float speed;        // yards per second toward the camera
        float nearDepth;    // puffs die here, yards ahead
        float farDepth;     // and are born here
        float size;         // quad edge, yards
        float halfWidth;    // spawn slab half width
        float below;        // spawn slab below and above the camera, yards
        float above;
        float fade;         // seconds of fade in and out
        float puffsAtFull;  // live puffs at intensity 1
    };

    static constexpr size_t kMaxPuffs = 256;

    WeatherMist() = default;
    ~WeatherMist();
    WeatherMist(const WeatherMist&) = delete;
    WeatherMist& operator=(const WeatherMist&) = delete;

    [[nodiscard]] bool isInitialized() const { return vkCtx_ != nullptr; }
    [[nodiscard]] bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout,
                                  pipeline::AssetManager* assets, const std::string& texturePath);
    void shutdown();
    void recreatePipeline();

    /// `amount` is the weather's drawn intensity times the Weather Detail
    /// scale; the live puff count moves toward amount * shape.puffsAtFull.
    void update(const Camera& camera, float deltaTime, float amount, const Shape& shape);
    void clear();
    void render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet);
    [[nodiscard]] size_t puffCount() const { return puffs_.size(); }

private:
    struct Puff {
        glm::vec3 start;    // camera-relative centre, at birth
        float birth;
        glm::vec3 velocity;
        float death;
        float size;         // quad edge, yards
        float fade;
    };
    struct MistVertex {
        glm::vec3 position; // camera-relative
        float alpha;
        glm::vec2 uv;
    };

    bool createPipeline();

    VkContext* vkCtx_ = nullptr;
    VkDescriptorSetLayout perFrameLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    WeatherTexture texture_;

    struct FrameBuffer {
        ::VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
        void* mapped = nullptr;
    };
    std::array<FrameBuffer, MAX_FRAMES_IN_FLIGHT> frames_{};

    std::vector<Puff> puffs_;
    std::vector<MistVertex> vertices_;
    float time_ = 0.0f;
    float budget_ = 0.0f;  // fractional puffs owed to the spawn rate
};

} // namespace rendering
} // namespace wowee
