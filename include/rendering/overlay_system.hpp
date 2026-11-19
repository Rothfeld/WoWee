#pragma once

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <glm/mat4x4.hpp>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include "rendering/vk_texture.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/unit_shadow.hpp"
#include <array>
#include <functional>
#include <optional>
#include <vector>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering {

class VkContext;

/// Manages selection circle and fullscreen overlay Vulkan pipelines.
/// Extracted from Renderer to isolate overlay rendering resources.
class OverlaySystem {
public:
    /// Height query callable: returns floor height at (x, y) or (x, y, probeZ).
    using HeightQuery2D = std::function<std::optional<float>(float x, float y)>;
    using HeightQuery3D = std::function<std::optional<float>(float x, float y, float probeZ)>;
    /// Appends the drawn world triangles (three world-space vertices each)
    /// whose bounds meet the box [min, max].
    using SurfaceTriangles = std::function<void(const glm::vec3& min, const glm::vec3& max,
                                                std::vector<glm::vec3>& out)>;

    explicit OverlaySystem(VkContext* ctx);
    ~OverlaySystem();

    OverlaySystem(const OverlaySystem&) = delete;
    OverlaySystem& operator=(const OverlaySystem&) = delete;

    void loadSelectionTexture(pipeline::AssetManager& assets);

    // Selection circle
    /// pos is the unit's position (not its feet); radius its selection radius.
    void setSelectionCircle(const glm::vec3& pos, float radius, const glm::vec3& color);
    void setSelectionCirclePosition(const glm::vec3& pos);
    void clearSelectionCircle();
    void renderSelectionCircle(const glm::mat4& view, const glm::mat4& projection,
                               VkCommandBuffer cmd,
                               const HeightQuery2D& terrainHeight,
                               const HeightQuery3D& wmoHeight,
                               const HeightQuery3D& m2Height,
                               const SurfaceTriangles& surfaces);

    // Ground blob shadows under units (rendering/unit_shadow.hpp)
    void loadUnitShadowTexture(pipeline::AssetManager& assets);
    void setUnitShadows(std::vector<UnitShadowBlob> blobs);
    /// Multiplies ShadowBlob.blp onto the terrain and WMO triangles under
    /// each unit near enough to the floor below it. Recorded after the
    /// ground and before units, so it never darkens a model.
    void renderUnitShadows(const glm::mat4& view, const glm::mat4& projection,
                           const glm::vec3& cameraPos, float fogStart, float fogEnd,
                           VkCommandBuffer cmd,
                           const HeightQuery2D& terrainHeight,
                           const HeightQuery3D& wmoHeight,
                           const HeightQuery3D& m2Height,
                           const SurfaceTriangles& surfaces);

    // Fullscreen color overlay (underwater tint, etc.)
    void renderOverlay(const glm::vec4& color, VkCommandBuffer cmd);

    /// Underwater tint bounded by a waterline that sweeps across the view as the
    /// camera crosses the surface. lineNdc runs from +1 (line off the bottom, so
    /// nothing is tinted) to -1 (off the top, so everything is), softness spans
    /// the meniscus and rippleAmp bends the edge.
    void renderWaterline(const glm::vec4& color, const glm::mat4& invViewProj,
                         float waterZ, float softness, float rippleAmp,
                         float time, bool hasSeam, VkCommandBuffer cmd);

    // Fullscreen multiplicative brightness scale (scene.rgb *= scale). Uses a
    // dst-color blend so brightness > 1 truly scales luminance instead of
    // lerping toward white (which washed everything out). scale > 1 only.
    void renderBrightnessScale(float scale, VkCommandBuffer cmd);

    /// Destroy all Vulkan resources (called before VkContext teardown).
    void cleanup();

    /// Recreate pipelines after swapchain resize / MSAA change.
    void recreatePipelines();

private:
    void initSelectionCircle();
    void initOverlayPipeline();
    void initBrightnessPipeline();

    VkContext* vkCtx_ = nullptr;

    // Selection circle resources
    VkPipeline selCirclePipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout selCirclePipelineLayout_ = VK_NULL_HANDLE;
    std::array<AllocatedBuffer, MAX_FRAMES_IN_FLIGHT> selCircleVertexBuffers_{};
    VkTexture selCircleTexture_;
    VkDescriptorSetLayout selCircleSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool selCirclePool_ = VK_NULL_HANDLE;
    VkDescriptorSet selCircleSet_ = VK_NULL_HANDLE;
    std::array<size_t, MAX_FRAMES_IN_FLIGHT> selCircleVertexCapacity_{};
    glm::vec3 selCirclePos_{0.0f};
    glm::vec3 selCircleColor_{1.0f, 0.0f, 0.0f};
    float selCircleRadius_ = 1.5f;
    bool selCircleVisible_ = false;
    // The world triangles the ring is drawn on this frame.
    std::vector<glm::vec3> selCircleTriangles_;

    // Unit blob shadow resources
    void initUnitShadows();
    VkPipeline shadowPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout shadowPipelineLayout_ = VK_NULL_HANDLE;
    std::array<AllocatedBuffer, MAX_FRAMES_IN_FLIGHT> shadowVertexBuffers_{};
    std::array<size_t, MAX_FRAMES_IN_FLIGHT> shadowVertexCapacity_{};
    VkTexture shadowTexture_;
    VkDescriptorSetLayout shadowSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool shadowPool_ = VK_NULL_HANDLE;
    VkDescriptorSet shadowSet_ = VK_NULL_HANDLE;
    std::vector<UnitShadowBlob> shadowBlobs_;
    std::vector<glm::vec3> shadowTriangles_;

    // Fullscreen overlay resources
    VkPipeline overlayPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout overlayPipelineLayout_ = VK_NULL_HANDLE;
    // Multiplicative brightness pipeline (shares overlayPipelineLayout_).
    VkPipeline brightnessPipeline_ = VK_NULL_HANDLE;
};

} // namespace rendering
} // namespace wowee
