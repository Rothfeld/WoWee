#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <vector>

namespace wowee {
namespace rendering {

class Camera;
class CameraController;
class WaterRenderer;
class M2Renderer;
class VkContext;

class SwimEffects {
public:
    SwimEffects();
    ~SwimEffects();

    [[nodiscard]] bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout);
    void shutdown();
    void recreatePipelines();
    /// The three pipelines both initialize() and recreatePipelines() need.
    void buildRipplePipeline(
        VkDevice device, const VkPipelineShaderStageCreateInfo& vertStage,
        const VkPipelineShaderStageCreateInfo& fragStage,
        const VkVertexInputBindingDescription& binding,
        const std::vector<VkVertexInputAttributeDescription>& attrs);
    void buildBubblePipeline(
        VkDevice device, const VkPipelineShaderStageCreateInfo& vertStage,
        const VkPipelineShaderStageCreateInfo& fragStage,
        const VkVertexInputBindingDescription& binding,
        const std::vector<VkVertexInputAttributeDescription>& attrs);

    /// Select the render pass these particles will be recorded into. Water now
    /// draws after them in a pass of its own, so the spray has to move into that
    /// pass too or the water sheet is painted straight over it. Call before
    /// initialize() or recreatePipelines().
    void setTargetPass(VkRenderPass pass, VkSampleCountFlagBits samples) {
        targetPass_ = pass;
        targetSamples_ = samples;
    }
    void update(const Camera& camera, const CameraController& cc,
                const WaterRenderer& water, float deltaTime);
    void render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet);
    void spawnFootSplash(const glm::vec3& footPos, float waterH);

private:
    struct Particle {
        glm::vec3 position;
        glm::vec3 velocity;
        float lifetime;
        float maxLifetime;
        float size;
        float alpha;
    };

    static constexpr int MAX_RIPPLE_PARTICLES = 200;
    static constexpr int MAX_BUBBLE_PARTICLES = 150;

    std::vector<Particle> ripples;
    std::vector<Particle> bubbles;

    // Vulkan objects
    VkContext* vkCtx = nullptr;

    // Ripple pipeline + dynamic buffer
    VkPipeline ripplePipeline = VK_NULL_HANDLE;
    VkPipelineLayout ripplePipelineLayout = VK_NULL_HANDLE;
    ::VkBuffer rippleDynamicVB = VK_NULL_HANDLE;
    VmaAllocation rippleDynamicVBAlloc = VK_NULL_HANDLE;
    VmaAllocationInfo rippleDynamicVBAllocInfo{};
    VkDeviceSize rippleDynamicVBSize = 0;

    // Bubble pipeline + dynamic buffer
    VkPipeline bubblePipeline = VK_NULL_HANDLE;
    VkPipelineLayout bubblePipelineLayout = VK_NULL_HANDLE;
    ::VkBuffer bubbleDynamicVB = VK_NULL_HANDLE;
    VmaAllocation bubbleDynamicVBAlloc = VK_NULL_HANDLE;
    VmaAllocationInfo bubbleDynamicVBAllocInfo{};
    VkDeviceSize bubbleDynamicVBSize = 0;

    std::vector<float> rippleVertexData;
    std::vector<float> bubbleVertexData;

    VkRenderPass targetPass_ = VK_NULL_HANDLE;
    VkSampleCountFlagBits targetSamples_ = VK_SAMPLE_COUNT_1_BIT;

    float wadeSpawnAccum = 0.0f;
    float rippleSpawnAccum = 0.0f;
    float bubbleSpawnAccum = 0.0f;

    void spawnRipple(const glm::vec3& pos, const glm::vec3& moveDir, float waterH);
    void spawnBubble(const glm::vec3& pos, float waterH);
};

} // namespace rendering
} // namespace wowee
