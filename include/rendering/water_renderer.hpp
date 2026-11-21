#pragma once

#include "rendering/vk_texture.hpp"
#include <vector>
#include <memory>
#include <optional>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>

namespace wowee {
namespace pipeline {
    class AssetManager;
    struct ADTTerrain;
    struct LiquidData;
    struct WMOLiquid;
}

namespace rendering {

class Camera;
class VkContext;

/**
 * Water surface for a single map chunk
 */
struct WaterSurface {
    glm::vec3 position;
    glm::vec3 origin;
    glm::vec3 stepX;
    glm::vec3 stepY;
    float minHeight;
    float maxHeight;
    uint16_t liquidType;
    std::optional<glm::vec4> indoorColor;

    int tileX = -1, tileY = -1;
    uint32_t wmoId = 0;
    // The WMO group (MOGP) the liquid belongs to; drawn with that group.
    uint16_t wmoGroup = 0;

    uint8_t xOffset = 0;
    uint8_t yOffset = 0;
    uint8_t width = 8;
    uint8_t height = 8;

    std::vector<float> heights;
    std::vector<uint8_t> depths;
    // Magma/slime: texture coordinate of each grid vertex, from the authored
    // s,t. Empty for water, whose coordinates follow the grid.
    std::vector<glm::vec2> texCoords;
    bool greenMagma = false;  // magma drawn with the lavagreen sequence
    std::vector<uint8_t> mask;

    // Vulkan render data
    ::VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VmaAllocation vertexAlloc = VK_NULL_HANDLE;
    ::VkBuffer indexBuffer = VK_NULL_HANDLE;
    VmaAllocation indexAlloc = VK_NULL_HANDLE;
    int indexCount = 0;

    // Per-surface material UBO
    ::VkBuffer materialUBO = VK_NULL_HANDLE;
    VmaAllocation materialAlloc = VK_NULL_HANDLE;

    // Material descriptor set (set 1)
    VkDescriptorSet materialSet = VK_NULL_HANDLE;

    [[nodiscard]] bool hasHeightData() const { return !heights.empty(); }
};

/**
 * Water renderer (Vulkan) with planar reflections, Gerstner waves,
 * GGX specular, shoreline foam, and subsurface scattering.
 */
// Matches set 2 binding 3 in water.frag.glsl. Keep kMaxWakePoints in step with
// the MAX_WAKE_POINTS constant declared there.
constexpr int kMaxWakePoints = 32;

constexpr int kMaxWaterContacts = 64;
struct WaterContactActor {
    uint64_t guid;
    glm::vec3 position;
    float radius;
};

struct WaterFrameUBOData {
    glm::mat4 reflViewProj{1.0f};
    glm::vec4 wakeBounds{0.0f};              // xy = centre, z = cull radius, w = count
    glm::vec4 liquidParams{0.5f, 1.0f, 0.0f, 0.0f};
    glm::vec4 contactMeta{};
    glm::vec4 contactPositions[kMaxWaterContacts]{}; // xyz, radius
    glm::vec4 contactStyles[kMaxWaterContacts]{}; // strength, direction xy, wake
    glm::vec4 wakePoints[kMaxWakePoints]{};  // xy = pos, z = age 0..1, w = strength
    glm::vec4 riverDeepColor{};
    glm::vec4 riverShallowColor{};
    glm::vec4 oceanShallowColor{}; // alpha in w
    glm::vec4 oceanDeepColor{};    // alpha in w
};

class WaterRenderer {
public:
    WaterRenderer();
    ~WaterRenderer();

    bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout, pipeline::AssetManager* assets);
    void shutdown();

    /// greenMagma: the tile's magma draws XTextures/lavagreen (Outland's
    /// terrain lava natively) rather than XTextures/lava.
    void loadFromTerrain(const pipeline::ADTTerrain& terrain, bool append = false,
                         int tileX = -1, int tileY = -1, bool greenMagma = false);

    void loadFromWMO(const pipeline::WMOLiquid& liquid, const glm::mat4& modelMatrix, uint32_t wmoId,
                     uint16_t groupIndex);
    /// Whether any of these groups of a WMO instance is drawn this frame
    /// (WMORenderer::anyGroupDrawn). A group's liquid is drawn with the
    /// group: 2.4.3 seen from above Orgrimmar draws neither the interior
    /// groups nor their pools.
    using WmoGroupQuery = std::function<bool(uint32_t wmoInstanceId, const std::vector<uint16_t>& groups)>;
    void setWmoGroupQuery(WmoGroupQuery query) { wmoGroupDrawn_ = std::move(query); }
    void removeWMO(uint32_t wmoId);
    void removeTile(int tileX, int tileY);
    void clear();

    void recreatePipelines();

    // Separate 1x pass for MSAA mode - water rendered after MSAA resolve
    bool createWater1xPass(VkFormat colorFormat, VkFormat depthFormat);
    void createWater1xFramebuffers(const std::vector<VkImageView>& swapViews,
                                    VkImageView depthView, VkExtent2D extent);
    void destroyWater1xResources();
    bool hasWater1xPass() const { return water1xRenderPass != VK_NULL_HANDLE; }
    VkRenderPass getWater1xRenderPass() const { return water1xRenderPass; }
    VkFramebuffer getWater1xFramebuffer(uint32_t index) const {
        return index < water1xFramebuffers.size() ? water1xFramebuffers[index] : VK_NULL_HANDLE;
    }

    void render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const Camera& camera, float time, bool use1x = false, uint32_t frameIndex = 0);
    void captureSceneHistory(VkCommandBuffer cmd,
                             VkImage srcColorImage,
                             VkImage srcDepthImage,
                             VkExtent2D srcExtent,
                             bool srcDepthIsMsaa,
                             uint32_t frameIndex = 0);
    [[nodiscard]] VkDescriptorSetLayout getSceneSetLayout() const { return sceneSetLayout; }
    [[nodiscard]] VkDescriptorSet getSceneSet(uint32_t frameIndex) const {
        return sceneHistory[frameIndex % SCENE_HISTORY_FRAMES].sceneSet;
    }

    // --- Planar reflection pass ---
    // Call sequence: beginReflectionPass → [render scene] → endReflectionPass
    bool beginReflectionPass(VkCommandBuffer cmd);
    void endReflectionPass(VkCommandBuffer cmd);

    // Get the dominant water height near a position (for reflection plane)
    std::optional<float> getDominantWaterHeight(const glm::vec3& cameraPos) const;

    // Compute reflected view matrix for a given water height
    static glm::mat4 computeReflectedView(const Camera& camera, float waterHeight);
    // Compute oblique clip projection to clip below-water geometry in reflection
    static glm::mat4 computeObliqueProjection(const glm::mat4& proj, const glm::mat4& view, float waterHeight);

    // Update the reflection UBO with reflected viewProj matrix
    void updateReflectionUBO(const glm::mat4& reflViewProj);

    /// Feed the surface disturbance left by something moving through the water.
    /// `intensity` is 0 when nothing is disturbing the surface. `wading` picks
    /// churned-up froth underfoot; swimming instead lays a V wake off the
    /// shoulders. Call once per frame.
    void setRiverAlpha(const glm::vec2& alpha) { frameUBO_.liquidParams.x = alpha.x; frameUBO_.liquidParams.y = alpha.y; }
    void setRiverDeepColor(const glm::vec3& color) { frameUBO_.riverDeepColor = glm::vec4(color, 1.0f); }
    void setRiverShallowColor(const glm::vec3& color) { frameUBO_.riverShallowColor = glm::vec4(color, 1.0f); }
    void setOceanColors(const glm::vec3& shallow, const glm::vec3& deep, const glm::vec2& alpha) {
        frameUBO_.oceanShallowColor = glm::vec4(shallow, alpha.x);
        frameUBO_.oceanDeepColor = glm::vec4(deep, alpha.y);
    }
    void updateContacts(float deltaTime, const std::vector<WaterContactActor>& actors);

    void updateWake(float deltaTime, const glm::vec2& pos, const glm::vec2& travelDir,
                    float intensity, bool wading);

    VkRenderPass getReflectionRenderPass() const { return reflectionRenderPass; }
    VkExtent2D getReflectionExtent() const { return {.width = REFLECTION_WIDTH, .height = REFLECTION_HEIGHT}; }
    bool hasReflectionPass() const { return reflectionRenderPass != VK_NULL_HANDLE; }
    bool hasSurfaces() const { return !surfaces.empty(); }

    void setEnabled(bool enabled) { renderingEnabled = enabled; }
    bool isEnabled() const { return renderingEnabled; }

    void setRefractionEnabled(bool enabled);
    bool isRefractionEnabled() const { return refractionEnabled; }
    // Display brightness (1.0 = neutral). The scene-history capture used for
    // refraction bakes this in, so the shader divides it back out.
    // Size of the target the water is drawn into. Screen-space lookups derive
    // their UVs from this rather than from the refraction texture's own size,
    // which is deliberately smaller than the frame.
    void setRenderExtent(VkExtent2D e) { renderExtent_ = e; }

    /// Where a point sits on a surface, if that surface has water there:
    /// the projection and the render mask, which every query asks first.
    std::optional<glm::vec2> wateredGridPosition(const WaterSurface& surface,
                                                 float glX, float glY) const;

    std::optional<float> getWaterHeightAt(float glX, float glY) const;
    /// Like getWaterHeightAt but only returns water surfaces whose height is
    /// close to the query Z (within maxAbove units above). Avoids false
    /// underwater detection from elevated WMO water far above the camera.
    std::optional<float> getNearestWaterHeightAt(float glX, float glY, float queryZ, float maxAbove = 15.0f) const;
    // A surface belongs to this volume only when no solid terrain/roof or floating hull
    // separates it from the query (`wmoId == 0` identifies outdoor terrain/ocean water).
    using WaterColumnBlockedQuery = std::function<bool(const glm::vec3& position, float surface, uint32_t wmoId)>;
    void setWaterColumnBlockedQuery(WaterColumnBlockedQuery query) {
        waterColumnBlockedQuery_ = std::move(query);
    }
    void setWaterColumnBlockedQuery(std::function<bool(const glm::vec3&, float)> query) {
        if (!query) {
            waterColumnBlockedQuery_ = nullptr;
            return;
        }
        waterColumnBlockedQuery_ = [q = std::move(query)](const glm::vec3& pos, float surface, uint32_t) {
            return q(pos, surface);
        };
    }
    std::optional<uint16_t> getWaterTypeAt(float glX, float glY) const;
    bool isWmoWaterAt(float glX, float glY) const;

    int getSurfaceCount() const { return static_cast<int>(surfaces.size()); }

private:
    WaterColumnBlockedQuery waterColumnBlockedQuery_;
    void createWaterMesh(WaterSurface& surface);
    void destroyWaterMesh(WaterSurface& surface);

    glm::vec4 getLiquidColor(uint16_t liquidType) const;
    float getLiquidAlpha(uint16_t liquidType) const;

    void updateMaterialUBO(WaterSurface& surface);
    VkDescriptorSet allocateMaterialSet();
    VkExtent2D refractionCaptureExtent() const;
    void createSceneHistoryResources(VkExtent2D extent, VkFormat colorFormat, VkFormat depthFormat);
    void destroySceneHistoryResources();

    // Reflection pass resources
    void createReflectionResources();
    void destroyReflectionResources();

    VkContext* vkCtx = nullptr;
    bool legacyWater = false;
    std::vector<VkTexture> liquidFrames;
    VkTexture splashTexture, wakeTexture;
    std::vector<VkDescriptorSet> liquidSets;
    VkDescriptorSetLayout lakeSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool lakePool = VK_NULL_HANDLE;

    // Pipeline
    VkPipeline waterPipeline = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout materialSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool materialDescPool = VK_NULL_HANDLE;
    VkDescriptorSetLayout sceneSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool sceneDescPool = VK_NULL_HANDLE;
    static constexpr uint32_t MAX_WATER_SETS = 16384;

    VkSampler sceneColorSampler = VK_NULL_HANDLE;
    VkSampler sceneDepthSampler = VK_NULL_HANDLE;
    // Per-frame scene history to avoid race between frames in flight
    static constexpr uint32_t SCENE_HISTORY_FRAMES = 2;
    // One reflection/wake UBO slot per frame in flight, at an offset every
    // device accepts for a uniform buffer (Vulkan caps the alignment at 256).
    static constexpr VkDeviceSize kFrameUBOStride =
        (sizeof(WaterFrameUBOData) + 255) / 256 * 256;
    struct PerFrameSceneHistory {
        VkImage colorImage = VK_NULL_HANDLE;
        VmaAllocation colorAlloc = VK_NULL_HANDLE;
        VkImageView colorView = VK_NULL_HANDLE;
        VkImage depthImage = VK_NULL_HANDLE;
        VmaAllocation depthAlloc = VK_NULL_HANDLE;
        VkImageView depthView = VK_NULL_HANDLE;
        VkDescriptorSet sceneSet = VK_NULL_HANDLE;
    };
    PerFrameSceneHistory sceneHistory[SCENE_HISTORY_FRAMES];
    VkExtent2D sceneHistoryExtent = {.width = 0, .height = 0};
    bool sceneHistoryReady = false;
    mutable uint32_t renderDiagCounter_ = 0;

    // Planar reflection resources
    static constexpr uint32_t REFLECTION_WIDTH = 512;
    static constexpr uint32_t REFLECTION_HEIGHT = 512;
    VkRenderPass reflectionRenderPass = VK_NULL_HANDLE;
    VkFramebuffer reflectionFramebuffer = VK_NULL_HANDLE;
    VkImage reflectionColorImage = VK_NULL_HANDLE;
    VmaAllocation reflectionColorAlloc = VK_NULL_HANDLE;
    VkImageView reflectionColorView = VK_NULL_HANDLE;
    VkImage reflectionDepthImage = VK_NULL_HANDLE;
    VmaAllocation reflectionDepthAlloc = VK_NULL_HANDLE;
    VkImageView reflectionDepthView = VK_NULL_HANDLE;
    VkSampler reflectionSampler = VK_NULL_HANDLE;
    VkImageLayout reflectionColorLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    // Reflection UBO (mat4 reflViewProj)
    // Surface disturbance trail. Points age out, spreading as they go; the
    // per-point drift is what turns a swimmer's pair of emissions into a V.
    struct WakePoint {
        glm::vec2 pos{0.0f};
        glm::vec2 drift{0.0f};
        float age = 0.0f;        // seconds
        float life = 1.0f;       // seconds
        float strength = 0.0f;
    };
    struct ContactEmitter {
        glm::vec3 previous{};
        float remaining = 0.0f;
        bool initialized = false;
        bool seen = false;
    };
    struct ContactPoint {
        glm::vec3 position;
        glm::vec2 direction;
        float radius, growth, strength, age;
        bool wake;
    };
    std::unordered_map<uint64_t, ContactEmitter> contactEmitters_;
    std::vector<ContactPoint> contactPoints_;
    std::vector<WakePoint> wakePoints_;
    WaterFrameUBOData frameUBO_{};
    glm::vec2 lastWakeEmitPos_{0.0f};
    bool hasWakeEmitPos_ = false;

    void uploadFrameUBO();

    ::VkBuffer reflectionUBO = VK_NULL_HANDLE;
    VmaAllocation reflectionUBOAlloc = VK_NULL_HANDLE;
    void* reflectionUBOMapped = nullptr;

    // Separate 1x water pass (used when MSAA is active)
    VkRenderPass water1xRenderPass = VK_NULL_HANDLE;
    VkPipeline water1xPipeline = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> water1xFramebuffers;

    std::vector<WaterSurface> surfaces;
    bool renderingEnabled = true;
    WmoGroupQuery wmoGroupDrawn_;
    bool refractionEnabled = false;
    VkExtent2D renderExtent_{.width = 0, .height = 0};
};

} // namespace rendering
} // namespace wowee
