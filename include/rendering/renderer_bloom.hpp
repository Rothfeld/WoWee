#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <cstdint>

namespace wowee::rendering {

class VkContext;

/**
 * TBC 2.4.3 full-screen scene glow (`ffxGlow`), modeled after shipped
 * `ffxbox4.bls`, `ffxgauss4.bls`, and `ffxglow.bls`.
 *
 * Runs on the finished 3D swapchain image after scene rendering, MSAA resolve,
 * and any FXAA / FSR 1.0 / FSR 2.2-3 upscaling, and composites into the
 * single-sampled overlay pass ahead of SunShafts and all 2D UI (Minimap,
 * FrameXML, Nameplates, ImGui):
 *   1. Two 2x2 linear blits downsample the swapchain image to 1/2 then 1/4
 *      resolution (exact 4x4 box filter, matching two passes of ffxbox4.bls).
 *   2. Separable horizontal + vertical 9-tap Gaussian compute passes blur the
 *      quarter-resolution buffer with screen-height-normalized sigma matching
 *      Native TBC 2.4.3 (5.75 px total sigma at 720p).
 *   3. Full-screen additive fragment pass adds `glow * (blur * blur)` onto the
 *      swapchain color buffer (`ffxglow.bls`).
 */
class RendererBloom {
public:
    RendererBloom() = default;
    ~RendererBloom();
    RendererBloom(const RendererBloom&) = delete;
    RendererBloom& operator=(const RendererBloom&) = delete;

    [[nodiscard]] bool initialize(VkContext* ctx);
    void shutdown();

    /// Downsample and blur `swapchainImage` outside any render pass. Leaves
    /// `swapchainImage` back in `VK_IMAGE_LAYOUT_PRESENT_SRC_KHR`.
    bool record(VkCommandBuffer cmd, uint32_t frame, VkImage swapchainImage,
                VkExtent2D extent, float strength);

    /// Add `strength * blur^2` inside the 1x overlay render pass before UI draws.
    void composite(VkCommandBuffer cmd, uint32_t frame);

private:
    static constexpr uint32_t MAX_FRAMES = 2;

    struct Target {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };

    bool createTarget(Target& t, VkExtent2D extent, VkFormat format, VkImageUsageFlags usage);
    void destroyTarget(Target& t);
    bool ensureTargets(VkExtent2D extent);
    void destroyTargets();
    bool ensureCompositePipeline();

    VkContext* ctx_ = nullptr;
    bool usable_ = false;

    Target halfCopy_[MAX_FRAMES];
    Target quarterCopy_[MAX_FRAMES];
    Target blurTemp_[MAX_FRAMES];
    Target bloom_[MAX_FRAMES];
    VkExtent2D halfExtent_{.width = 0, .height = 0};
    VkExtent2D quarterExtent_{.width = 0, .height = 0};
    VkExtent2D sourceExtent_{.width = 0, .height = 0};
    float strength_[MAX_FRAMES] = {};
    bool drawThisFrame_[MAX_FRAMES] = {};

    VkSampler sampler_ = VK_NULL_HANDLE;

    VkDescriptorSetLayout blurSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout blurLayout_ = VK_NULL_HANDLE;
    VkPipeline blurPipeline_ = VK_NULL_HANDLE;

    VkDescriptorSetLayout compositeSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout compositeLayout_ = VK_NULL_HANDLE;
    VkPipeline compositePipeline_ = VK_NULL_HANDLE;
    VkFormat compositeFormat_ = VK_FORMAT_UNDEFINED;

    VkDescriptorPool descPool_ = VK_NULL_HANDLE;
    VkDescriptorSet blurHSets_[MAX_FRAMES] = {};
    VkDescriptorSet blurVSets_[MAX_FRAMES] = {};
    VkDescriptorSet compositeSets_[MAX_FRAMES] = {};
};

}  // namespace wowee::rendering
