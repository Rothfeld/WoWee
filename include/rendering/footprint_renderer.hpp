#pragma once

#include <array>
#include <optional>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include "rendering/vk_texture.hpp"

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering {

class Camera;
class Renderer;
class VkContext;

/**
 * Short-lived ground decals spawned by authored M2 footfall events.
 * CreatureModelData supplies the original-client mask and physical dimensions.
 */
class FootprintRenderer {
public:
    FootprintRenderer() = default;
    ~FootprintRenderer();

    [[nodiscard]] bool initialize(Renderer* owner, VkContext* ctx,
                                  VkDescriptorSetLayout perFrameLayout,
                                  pipeline::AssetManager* assetManager);
    void shutdown();
    void recreatePipelines();

    void update(float deltaTime);
    void clear();
    /// One print of CreatureModelData row `creatureModelDataId`'s footprint
    /// mask, centred on `footPosition` and turned with the body's yaw.
    void spawn(uint32_t creatureModelDataId, const glm::vec3& footPosition,
               float yawRadians, bool leftFoot, float scale);
    [[nodiscard]] bool hasProfile(uint32_t creatureModelDataId) const;
    /// TerrainType id of the surface at a foot: the terrain texture's
    /// GroundEffectTexture row, or the WMO floor material. None on model
    /// (M2) surfaces and off any floor.
    [[nodiscard]] std::optional<uint32_t> terrainTypeAt(const glm::vec3& position) const;
    void render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const Camera& camera);

private:
    struct Profile {
        uint8_t textureIndex = 0;
        float length = 1.0f;
        float width = 0.8f;
    };

    struct Print {
        glm::vec3 position{0.0f};
        glm::vec2 slope{0.0f};
        float yaw = 0.0f;
        float length = 1.0f;
        float signedWidth = 0.8f;
        float age = 0.0f;
        uint8_t textureIndex = 0;
    };

    // FootprintTextures.dbc has six rows in 2.4.3 (Base, Cloven, Bare, Claw,
    // Hoof, Paw); loading asserts the file still has that many.
    static constexpr size_t kTextureCount = 6;

    Renderer* owner_ = nullptr;
    VkContext* vkCtx_ = nullptr;
    VkDescriptorSetLayout perFrameLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout materialSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, kTextureCount> textureSets_{};
    std::array<VkTexture, kTextureCount> textures_{};
    VkBuffer quadVB_ = VK_NULL_HANDLE;
    VmaAllocation quadVBAlloc_ = VK_NULL_HANDLE;

    std::unordered_map<uint32_t, Profile> profilesByModel_;  // CreatureModelData id
    std::vector<Print> prints_;
    std::unordered_map<uint32_t, uint32_t> terrainFlags_;
    std::unordered_map<uint32_t, uint32_t> groundEffectTerrain_;
    [[nodiscard]] bool surfaceAllowsFootprints(const glm::vec3& position) const;

    bool createPipeline();
    bool createDescriptorResources();
    bool createQuad();
    bool loadFootprintData(pipeline::AssetManager* assetManager);
    [[nodiscard]] float resolveFloorHeight(const glm::vec3& position) const;
};

} // namespace rendering
} // namespace wowee
