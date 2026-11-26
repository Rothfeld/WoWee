#pragma once

#include <cmath>

#include "rendering/vk_shader.hpp"
#include "rendering/shadow_params.hpp"

#include "pipeline/terrain_mesh.hpp"
#include "pipeline/blp_loader.hpp"
#include "rendering/camera.hpp"
#include "rendering/descriptor_pool_chain.hpp"
#include "rendering/pinned_lru_cache.hpp"
#include "rendering/vk_texture.hpp"
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <algorithm>

namespace wowee {

// Forward declarations
namespace pipeline { class AssetManager; }

namespace rendering {

class VkContext;
class Frustum;
class RtScene;

/// A tileset texture as the terrain texture cache holds it.
struct TerrainTilesetTexture {
    std::unique_ptr<VkTexture> texture;
    bool hasSpecularAlpha = false;
};
/// Tileset textures by normalized path (tileset\elwynn\elwynngrassbase.blp),
/// pinned by the chunks that draw with them.
using TerrainTilesetCache = PinnedLruCache<std::string, TerrainTilesetTexture>;

/**
 * GPU-side terrain chunk data (Vulkan)
 */
struct TerrainChunkGPU {
    ::VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VmaAllocation vertexAlloc = VK_NULL_HANDLE;
    ::VkBuffer indexBuffer = VK_NULL_HANDLE;
    VmaAllocation indexAlloc = VK_NULL_HANDLE;
    uint32_t indexCount = 0;

    // Material descriptor set (set 1: 7 samplers + params UBO)
    VkDescriptorSet materialSet = VK_NULL_HANDLE;
    VkDescriptorPool materialPool = VK_NULL_HANDLE;  // the pool materialSet is freed to

    // Per-chunk params UBO (hasLayer1/2/3)
    ::VkBuffer paramsUBO = VK_NULL_HANDLE;
    VmaAllocation paramsAlloc = VK_NULL_HANDLE;

    // What the material descriptor set samples. Each points at a shared
    // tileset texture (pinned below), at one of this chunk's own maps (owned
    // below) or at one of the renderer's 1x1 fallbacks.
    VkTexture* baseTexture = nullptr;
    VkTexture* layerTextures[3] = {nullptr, nullptr, nullptr};
    VkTexture* alphaTextures[3] = {nullptr, nullptr, nullptr};
    VkTexture* shadowTexture = nullptr;
    // The cache entries behind baseTexture and layerTextures, each holding one
    // pin of this chunk's; null where the fallback is bound. Released in
    // TerrainRenderer::destroyChunkGPU.
    TerrainTilesetCache::Entry* tilesetPins[4] = {nullptr, nullptr, nullptr, nullptr};
    // This chunk's own 64x64 maps: the alpha maps of layers 1..3 and the baked
    // shadow map. Nothing else can use them, so they live and die with the
    // chunk (destroyChunkGPU) and are not in the texture cache.
    std::unique_ptr<VkTexture> ownedAlphaMaps[3];
    std::unique_ptr<VkTexture> ownedShadowMap;
    int layerCount = 0;
    bool bigAlpha = false;
    uint32_t layerFlags[4] = {0, 0, 0, 0};
    float layerSpecMask[4] = {0.0f, 0.0f, 0.0f, 0.0f};

    // World position for culling
    float worldX = 0.0f;
    float worldY = 0.0f;
    float worldZ = 0.0f;

    // Owning tile coordinates (for per-tile removal)
    int tileX = -1, tileY = -1;

    // Bounding sphere for frustum culling
    float boundingSphereRadius = 0.0f;
    glm::vec3 boundingSphereCenter = glm::vec3(0.0f);

    // Offsets into mega buffers for indirect drawing (-1 = not in mega buffer)
    int32_t megaBaseVertex = -1;
    uint32_t megaFirstIndex = 0;
    uint32_t vertexCount = 0;

    // The chunk as the ray traced lighting sees it (RtScene ids, or ~0u).
    uint32_t rtMesh = ~0u;
    uint32_t rtInstance = ~0u;

    [[nodiscard]] bool isValid() const { return vertexBuffer != VK_NULL_HANDLE && indexBuffer != VK_NULL_HANDLE; }
};

/**
 * Terrain renderer (Vulkan)
 */
class TerrainRenderer {
public:
    TerrainRenderer();
    ~TerrainRenderer();

    /**
     * Initialize terrain renderer
     * @param ctx Vulkan context
     * @param perFrameLayout Descriptor set layout for set 0 (per-frame UBO)
     * @param assetManager Asset manager for loading textures
     */
    bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout,
                    pipeline::AssetManager* assetManager);

    void shutdown();

    bool loadTerrain(const pipeline::TerrainMesh& mesh,
                     const std::vector<std::string>& texturePaths,
                     int tileX = -1, int tileY = -1);


    /// Upload a batch of terrain chunks incrementally. Returns true when all chunks done.
    /// chunkIndex is updated to the next chunk to process (0-255 row-major).
    bool loadTerrainIncremental(const pipeline::TerrainMesh& mesh,
                                const std::vector<std::string>& texturePaths,
                                int tileX, int tileY,
                                int& chunkIndex, int maxChunksPerCall = 16);

    void removeTile(int tileX, int tileY);

    void uploadPreloadedTextures(const std::unordered_map<std::string, pipeline::BLPImage>& textures);

    /**
     * Render terrain
     * @param cmd Command buffer to record into
     * @param perFrameSet Per-frame descriptor set (set 0)
     * @param camera Camera for frustum culling
     */
    void render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const Camera& camera);

    /**
     * Initialize terrain shadow pipeline (must be called after initialize()).
     * @param shadowRenderPass  Depth-only render pass used for the shadow map.
     */
    bool initializeShadow(VkRenderPass shadowRenderPass);

    /**
     * Render terrain into the shadow depth map.
     * @param cmd               Command buffer (inside shadow render pass).
     * @param lightSpaceMatrix  Orthographic light-space transform.
     * @param shadowCenter      World-space centre of shadow coverage.
     * @param shadowRadius      Cull radius around shadowCenter.
     */
    void renderShadow(VkCommandBuffer cmd, const glm::mat4& lightSpaceMatrix,
                      const glm::vec3& shadowCenter, float shadowRadius);

    [[nodiscard]] bool hasShadowPipeline() const { return shadowPipeline_ != VK_NULL_HANDLE; }

    void clear();

    void recreatePipelines();
    /// The fill pipeline and its wireframe derivative, which
    /// initialize() and recreatePipelines() both need.
    bool buildMainPassPipelines(VkDevice device,
                                wowee::rendering::VkShaderModule& vertShader,
                                wowee::rendering::VkShaderModule& fragShader);

    void setWireframe(bool enabled) { wireframe = enabled; }
    void setFrustumCulling(bool enabled) { frustumCullingEnabled = enabled; }
    void setFogEnabled(bool enabled) { fogEnabled = enabled; }
    [[nodiscard]] bool isFogEnabled() const { return fogEnabled; }
    void setViewDistance(float distance) { maxViewDistance_ = std::clamp(distance, 400.0f, 2400.0f); }

    void setShadowMap(VkDescriptorImageInfo /*depthInfo*/, const glm::mat4& /*lightSpaceMat*/) {}
    void clearShadowMap() {}

    [[nodiscard]] int getChunkCount() const { return static_cast<int>(chunks.size()); }
    [[nodiscard]] int getRenderedChunkCount() const { return renderedChunks; }
    /// How far the furthest chunk drawn this frame was, for the diagnostic that
    /// compares it against the doodads. See Renderer::logViewDistanceDiag.
    [[nodiscard]] float getFurthestDrawnDistance() const { return std::sqrt(furthestDrawnSq_); }
    [[nodiscard]] int getCulledChunkCount() const { return culledChunks; }
    [[nodiscard]] int getTriangleCount() const;
    [[nodiscard]] VkContext* getVkContext() const { return vkCtx; }

    /// What the texture cache holds, by its own accounting.
    ///
    /// For the WOWEE_MEMORY_TRACE line (Renderer::update), which pairs these
    /// with the allocator's figures so a run can tell cache growth from other
    /// growth.
    struct TextureCacheStats {
        size_t entries = 0;        ///< Tileset textures in the cache.
        size_t pinnedEntries = 0;  ///< Of those, held by at least one live chunk.
        size_t accountedBytes = 0; ///< What the budget is compared against.
        size_t budgetBytes = 0;
        size_t chunks = 0;         ///< Live terrain chunks.
        size_t chunkMaps = 0;      ///< Alpha and shadow maps those chunks own (not in the cache).
    };
    [[nodiscard]] TextureCacheStats getTextureCacheStats() const;

    /// Where chunks register their geometry for the ray traced lighting.
    void setRtScene(RtScene* scene) { rtScene_ = scene; }

private:
    TerrainChunkGPU uploadChunk(const pipeline::ChunkMesh& chunk);
    /// The cache entry for a tileset texture, loading it if need be. Null
    /// when the file cannot be loaded or uploaded, or the cache is full of
    /// pinned textures; the caller binds the white fallback then.
    TerrainTilesetCache::Entry* loadTexture(const std::string& path);
    /// A chunk's own alpha or shadow map. Null when the upload fails; the
    /// caller binds the fallback then.
    std::unique_ptr<VkTexture> createAlphaTexture(const std::vector<uint8_t>& alphaData);
    std::unique_ptr<VkTexture> createShadowTexture(const std::array<uint8_t, 512>& shadowMap);
    bool isChunkVisible(const TerrainChunkGPU& chunk, const Frustum& frustum);
    void calculateBoundingSphere(TerrainChunkGPU& chunk, const pipeline::ChunkMesh& meshChunk);
    /// Sets chunk.materialSet and chunk.materialPool. False when no pool
    /// could serve and a new one could not be created.
    bool allocateMaterialSet(TerrainChunkGPU& chunk);
    /// False when there is nothing sampleable to write, in which case the
    /// caller must drop the chunk: a descriptor set that was allocated and
    /// never written is as undefined to bind as one holding a null view.
    bool writeMaterialDescriptors(VkDescriptorSet set, const TerrainChunkGPU& chunk);
    void destroyChunkGPU(TerrainChunkGPU& chunk);

    /// Point a chunk's base, layer and alpha textures at the loaded ones.
    ///
    /// Both load paths do this identically; chunkX and chunkY only name the
    /// chunk in the warnings. A layer whose textureId is past the end of the
    /// tile's texture list falls back to white rather than going unbound.
    void bindChunkTextures(TerrainChunkGPU& gpuChunk,
                           const pipeline::ChunkMesh& chunk,
                           const std::vector<std::string>& texturePaths,
                           int tileX, int tileY, int chunkX, int chunkY);

    /// Allocate and fill a chunk's params UBO. False means the allocation
    /// failed, and the two callers answer that differently: the one-shot load
    /// skips the chunk, the incremental one hands it back to be retried.
    bool createChunkParamsUBO(TerrainChunkGPU& gpuChunk);

    VkContext* vkCtx = nullptr;
    RtScene* rtScene_ = nullptr;
    void registerRtChunk(TerrainChunkGPU& gpuChunk, const pipeline::ChunkMesh& chunk);
    pipeline::AssetManager* assetManager = nullptr;

    // Main pipelines
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipeline wireframePipeline = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout materialSetLayout = VK_NULL_HANDLE;

    // Shadow pipeline
    VkPipeline shadowPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout shadowPipelineLayout_ = VK_NULL_HANDLE;
    /// The set the shadow pass binds. Five separate members before,
    /// built and torn down here and in three other renderers.
    ShadowParamsSet shadowParams_;

    // Descriptor pools for material sets: one set per terrain chunk, 256
    // chunks to a tile, and tiles are held to the *unload* radius. A pool is
    // added when the existing ones are full (descriptor_pool_chain.hpp), so
    // the number of resident tiles has no ceiling here and a chunk is not left
    // without a set for want of pool space.
    DescriptorPoolChain<VkDescriptorPool, VkDescriptorSet> materialPools_;
    /// Sets per pool: 64 tiles. One pool carries the 49 tiles of the smallest
    /// load radius; 81 to 112 resident tiles (view distance 1900) take two.
    static constexpr uint32_t MATERIAL_SETS_PER_POOL = 16 * 1024;

    // Loaded terrain chunks
    std::vector<TerrainChunkGPU> chunks;

    // Shared tileset textures. Chunks pin what they draw with; the per-chunk
    // alpha and shadow maps are owned by their chunk and are not in here.
    TerrainTilesetCache tilesetCache_;
    std::unordered_set<std::string> loggedTextureLoadFails_;
    size_t liveChunkMaps_ = 0;

    /// Evict least-recently-used unpinned tileset textures until `needBytes`
    /// fits. Returns whether there is now room.
    ///
    /// Evicted textures go through the frame fences rather than being
    /// destroyed on the spot: the chunk that unpinned one may have been
    /// removed this very frame, with its material descriptor set still named
    /// by a command buffer in flight.
    bool evictTexturesFor(size_t needBytes);

    // Fallback textures
    std::unique_ptr<VkTexture> whiteTexture;
    std::unique_ptr<VkTexture> opaqueAlphaTexture;
    std::unique_ptr<VkTexture> unshadowedTexture;

    // Rendering state
    bool wireframe = false;
    bool frustumCullingEnabled = true;
    bool fogEnabled = true;
    float maxViewDistance_ = 1200.0f;
    int renderedChunks = 0;
    float furthestDrawnSq_ = 0.0f;
    int culledChunks = 0;

    // Mega vertex/index buffers for indirect drawing
    // All terrain chunks share a single VB + IB, eliminating per-chunk rebinds.
    // Indirect draw commands are built CPU-side each frame for visible chunks.
    VkBuffer megaVB_ = VK_NULL_HANDLE;
    VmaAllocation megaVBAlloc_ = VK_NULL_HANDLE;
    void* megaVBMapped_ = nullptr;
    VkBuffer megaIB_ = VK_NULL_HANDLE;
    VmaAllocation megaIBAlloc_ = VK_NULL_HANDLE;
    void* megaIBMapped_ = nullptr;
    uint32_t megaVBUsed_ = 0;  // vertices used
    uint32_t megaIBUsed_ = 0;  // indices used
    static constexpr uint32_t MEGA_VB_MAX_VERTS   = 1536 * 1024; // ~1.5M verts × 44B ≈ 64MB
    static constexpr uint32_t MEGA_IB_MAX_INDICES  = 6 * 1024 * 1024; // 6M indices × 4B = 24MB

    VkBuffer indirectBuffer_ = VK_NULL_HANDLE;
    VmaAllocation indirectAlloc_ = VK_NULL_HANDLE;
    static constexpr uint32_t MAX_INDIRECT_DRAWS = 8192;
};

} // namespace rendering
} // namespace wowee
