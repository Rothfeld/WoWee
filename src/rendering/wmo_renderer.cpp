#include "rendering/collision_geometry.hpp"
#include "rendering/body_collision.hpp"
#include "rendering/movement_limits.hpp"
#include "rendering/pom_quality.hpp"
#include "rendering/placement_transform.hpp"
#include "rendering/spatial_grid.hpp"
#include "rendering/wmo_vertex.hpp"
#include "rendering/shadow_params.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/wmo_portal_visibility.hpp"
#include "rendering/visual_mesh_pick.hpp"
#include "rendering/rt_bvh.hpp"
#include "rendering/rt_scene.hpp"
#include "rendering/wmo_material_class.hpp"
#include "rendering/normal_map.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_texture.hpp"
#include "rendering/vk_buffer.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_utils.hpp"
#include "rendering/vk_frame_data.hpp"
#include "rendering/camera.hpp"
#include "rendering/frustum.hpp"
#include "pipeline/wmo_loader.hpp"
#include "pipeline/asset_manager.hpp"
#include "core/logger.hpp"
#include "game/game_utils.hpp"
#include <cassert>
#include <mutex>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <cassert>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <thread>
#include <unordered_set>

namespace wowee {
namespace rendering {

namespace {

/// MOGP flag 0x80: unreachable group (see createGroupResources).
constexpr uint32_t kWMOGroupUnreachable = 0x80;
/// Where a WMO surface stops being a floor and becomes a wall, as the absolute
/// z of its normal: cos 49.46 degrees.
///
/// The static pass sorts triangles into floors and walls with it, and the
/// runtime wall check skips anything at or above it because grounding handles
/// those. The two had to agree and said so only in a comment; one definition
/// is what actually holds them together. A drifted pair here means a surface
/// the player can neither walk on nor be stopped by, which reads as falling
/// through a floor or as an invisible wall.
///
/// Not the slope-slide cutoff, which is cos 50 degrees (0.6428) and lives with
/// the sliding code, and not M2's: that classifies floors at 0.35 so steep
/// stairs stay walkable while still blocking, so its two cutoffs deliberately
/// overlap where these do not.
constexpr float kWallMaxAbsNormalZ = 0.65f;
} // namespace

// Thread-local scratch buffers for collision queries (allows concurrent getFloorHeight/checkWallCollision calls)
static thread_local std::vector<size_t> tl_candidateScratch;
static thread_local std::vector<uint32_t> tl_triScratch;
static thread_local std::unordered_set<uint32_t> tl_candidateIdScratch;

static void transformAABB(const glm::mat4& modelMatrix,
                          const glm::vec3& localMin,
                          const glm::vec3& localMax,
                          glm::vec3& outMin,
                          glm::vec3& outMax);

WMORenderer::WMORenderer() {
}

WMORenderer::~WMORenderer() {
    shutdown();
}

/// Builds the four main-pass pipelines from an already-loaded shader pair.
///
/// initialize() and recreatePipelines() both need exactly these four, in
/// exactly these states, and each described all four for itself. They only
/// differ in what happens when one fails to build: the first cannot continue,
/// the second is a rebuild after a settings change and leaves the renderer
/// with whatever it managed.
bool WMORenderer::buildMainPassPipelines(VkDevice device,
                                         wowee::rendering::VkShaderModule& vertShader,
                                         wowee::rendering::VkShaderModule& fragShader) {
    // --- Vertex input ---
    const VkVertexInputBindingDescription vertexBinding =
        perVertexBinding(sizeof(WMOVertex));
    const std::vector<VkVertexInputAttributeDescription> vertexAttribs =
        toVkAttributes(kWmoVertexAttributes);

    // Authored outward faces are counter-clockwise with our Vulkan projection.
    // Even without culling, this controls gl_FrontFacing and normal reversal.
    // --- Build opaque pipeline (base for derivatives - shared state optimization) ---
    VkRenderPass mainPass = vkCtx_->getImGuiRenderPass();

    opaquePipeline_ = PipelineBuilder()
        .setShaders(vertShader.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                    fragShader.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
        .setVertexInput({ vertexBinding }, vertexAttribs)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE)
        .setDepthTest(true, true, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setColorBlendAttachment(PipelineBuilder::blendDisabled())
        .setMultisample(vkCtx_->getMsaaSamples())
        .setLayout(pipelineLayout_)
        .setRenderPass(mainPass)
        .setDynamicStates(viewportAndScissorDynamic())
        .setFlags(VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT)
        .build(device, vkCtx_->getPipelineCache());

    if (!opaquePipeline_) {
        core::Logger::getInstance().error("WMORenderer: failed to create opaque pipeline");
        return false;
    }

    // --- Build transparent pipeline (derivative of opaque) ---
    transparentPipeline_ = PipelineBuilder()
        .setShaders(vertShader.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                    fragShader.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
        .setVertexInput({ vertexBinding }, vertexAttribs)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE)
        .setDepthTest(true, false, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setColorBlendAttachment(PipelineBuilder::blendAlpha())
        .setMultisample(vkCtx_->getMsaaSamples())
        .setLayout(pipelineLayout_)
        .setRenderPass(mainPass)
        .setDynamicStates(viewportAndScissorDynamic())
        .setFlags(VK_PIPELINE_CREATE_DERIVATIVE_BIT)
        .setBasePipeline(opaquePipeline_)
        .build(device, vkCtx_->getPipelineCache());

    if (!transparentPipeline_) {
        core::Logger::getInstance().warning("WMORenderer: transparent pipeline not available");
    }

    // --- Build wireframe pipeline (derivative of opaque) ---
    wireframePipeline_ = PipelineBuilder()
        .setShaders(vertShader.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                    fragShader.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
        .setVertexInput({ vertexBinding }, vertexAttribs)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_LINE, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE)
        .setDepthTest(true, true, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setColorBlendAttachment(PipelineBuilder::blendDisabled())
        .setMultisample(vkCtx_->getMsaaSamples())
        .setLayout(pipelineLayout_)
        .setRenderPass(mainPass)
        .setDynamicStates(viewportAndScissorDynamic())
        .setFlags(VK_PIPELINE_CREATE_DERIVATIVE_BIT)
        .setBasePipeline(opaquePipeline_)
        .build(device, vkCtx_->getPipelineCache());

    if (!wireframePipeline_) {
        core::Logger::getInstance().warning("WMORenderer: wireframe pipeline not available");
    }

    // One-sided materials. MOMT flag 0x04 (F_UNCULLED) marks the two-sided
    // ones; every other WMO surface is drawn from its front only. GM Island's
    // guildhousea.wmo shows why: its exterior group has walls facing into the
    // courtyard behind the lodge, and the original sees through their backs
    // from that courtyard to the trees and terrain beyond.
    opaqueCulledPipeline_ = PipelineBuilder()
        .setShaders(vertShader.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                    fragShader.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
        .setVertexInput({ vertexBinding }, vertexAttribs)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE)
        .setDepthTest(true, true, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setColorBlendAttachment(PipelineBuilder::blendDisabled())
        .setMultisample(vkCtx_->getMsaaSamples())
        .setLayout(pipelineLayout_)
        .setRenderPass(mainPass)
        .setDynamicStates(viewportAndScissorDynamic())
        .setFlags(VK_PIPELINE_CREATE_DERIVATIVE_BIT)
        .setBasePipeline(opaquePipeline_)
        .build(device, vkCtx_->getPipelineCache());
    transparentCulledPipeline_ = PipelineBuilder()
        .setShaders(vertShader.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                    fragShader.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
        .setVertexInput({ vertexBinding }, vertexAttribs)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE)
        .setDepthTest(true, false, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setColorBlendAttachment(PipelineBuilder::blendAlpha())
        .setMultisample(vkCtx_->getMsaaSamples())
        .setLayout(pipelineLayout_)
        .setRenderPass(mainPass)
        .setDynamicStates(viewportAndScissorDynamic())
        .setFlags(VK_PIPELINE_CREATE_DERIVATIVE_BIT)
        .setBasePipeline(opaquePipeline_)
        .build(device, vkCtx_->getPipelineCache());
    if (!opaqueCulledPipeline_ || !transparentCulledPipeline_) {
        core::Logger::getInstance().error("WMORenderer: one-sided material pipelines not available");
        return false;
    }

    return opaquePipeline_ != VK_NULL_HANDLE;
}

bool WMORenderer::initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout,
                              pipeline::AssetManager* assets) {
    if (initialized_) { assetManager = assets; return true; }
    core::Logger::getInstance().info("Initializing WMO renderer (Vulkan)...");

    vkCtx_ = ctx;
    assetManager = assets;

    if (!vkCtx_) {
        core::Logger::getInstance().error("WMORenderer: null VkContext");
        return false;
    }

    const unsigned hc = std::thread::hardware_concurrency();
    const size_t availableCores = (hc > 1u) ? static_cast<size_t>(hc - 1u) : 1ull;
    // WMO culling is lighter than animation; keep defaults conservative to reduce spikes.
    const size_t defaultCullThreads = std::max<size_t>(1, availableCores / 4);
    numCullThreads_ = static_cast<uint32_t>(std::max<size_t>(
        1, envSizeOrDefault("WOWEE_WMO_CULL_THREADS", defaultCullThreads)));
    core::Logger::getInstance().info("WMO cull threads: ", numCullThreads_);

    // Off unless asked for: a building that is not drawn is a worse fault than
    // a building drawn when it did not need to be.
    if (const char* v = std::getenv("WOWEE_WMO_CULL"); v && *v && *v != '0') {
        cullingEnabled_ = true;
        core::Logger::getInstance().info("WMO culling enabled by WOWEE_WMO_CULL");
    }

    VkDevice device = vkCtx_->getDevice();

    // --- Create material descriptor set layout (set 1) ---
    // binding 0: sampler2D (diffuse texture)
    // binding 1: uniform buffer (WMOMaterial)
    // binding 2: sampler2D (normal+height map)
    std::vector<VkDescriptorSetLayoutBinding> materialBindings(3);
    materialBindings[0] = {};
    materialBindings[0].binding = 0;
    materialBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    materialBindings[0].descriptorCount = 1;
    materialBindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    materialBindings[1] = {};
    materialBindings[1].binding = 1;
    materialBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    materialBindings[1].descriptorCount = 1;
    materialBindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    materialBindings[2] = {};
    materialBindings[2].binding = 2;
    materialBindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    materialBindings[2].descriptorCount = 1;
    materialBindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    materialSetLayout_ = createDescriptorSetLayout(device, materialBindings);
    if (!materialSetLayout_) {
        core::Logger::getInstance().error("WMORenderer: failed to create material set layout");
        return false;
    }

    // --- Create descriptor pool ---
    VkDescriptorPoolSize poolSizes[] = {
        { .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = MAX_MATERIAL_SETS * 2 },  // diffuse + normal/height
        { .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = MAX_MATERIAL_SETS },
    };

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = MAX_MATERIAL_SETS;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;

    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &materialDescPool_) != VK_SUCCESS) {
        core::Logger::getInstance().error("WMORenderer: failed to create descriptor pool");
        return false;
    }

    // --- Create pipeline layout ---
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(WMOPushConstants);

    std::vector<VkDescriptorSetLayout> setLayouts = { perFrameLayout, materialSetLayout_ };
    pipelineLayout_ = createPipelineLayout(device, setLayouts, { pushRange });
    if (!pipelineLayout_) {
        core::Logger::getInstance().error("WMORenderer: failed to create pipeline layout");
        return false;
    }

    // --- Load shaders ---
    VkShaderModule vertShader, fragShader;
    if (!vertShader.loadFromFile(device, "assets/shaders/wmo.vert.spv")) {
        core::Logger::getInstance().error("WMORenderer: failed to load vertex shader");
        return false;
    }
    if (!fragShader.loadFromFile(device, "assets/shaders/wmo.frag.spv")) {
        core::Logger::getInstance().error("WMORenderer: failed to load fragment shader");
        return false;
    }

    if (!buildMainPassPipelines(device, vertShader, fragShader)) {
        vertShader.destroy();
        fragShader.destroy();
        return false;
    }

    vertShader.destroy();
    fragShader.destroy();

    // --- Create fallback white texture ---
    whiteTexture_ = std::make_unique<VkTexture>();
    uint8_t whitePixel[4] = {255, 255, 255, 255};
    whiteTexture_->upload(*vkCtx_, whitePixel, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, false);
    whiteTexture_->createSampler(device, VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                                  VK_SAMPLER_ADDRESS_MODE_REPEAT);

    // --- Create flat normal placeholder texture ---
    // (128,128,255,128) = flat normal pointing up (0,0,1), mid-height
    flatNormalTexture_ = std::make_unique<VkTexture>();
    uint8_t flatNormalPixel[4] = {128, 128, 255, 128};
    flatNormalTexture_->upload(*vkCtx_, flatNormalPixel, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, false);
    flatNormalTexture_->createSampler(device, VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                                       VK_SAMPLER_ADDRESS_MODE_REPEAT);
    textureCacheBudgetBytes_ =
        envSizeMBOrDefault("WOWEE_WMO_TEX_CACHE_MB", 8192) * 1024ull * 1024ull;
    modelCacheLimit_ = envSizeMBOrDefault("WOWEE_WMO_MODEL_LIMIT", 4000);
    core::Logger::getInstance().info("WMO texture cache budget: ",
                                     textureCacheBudgetBytes_ / (1024 * 1024), " MB");
    core::Logger::getInstance().info("WMO model cache limit: ", modelCacheLimit_);

    core::Logger::getInstance().info("WMO renderer initialized (Vulkan)");
    initialized_ = true;
    return true;
}

void WMORenderer::shutdown() {
    core::Logger::getInstance().info("Shutting down WMO renderer...");

    // Without a context there is nothing to free on the GPU and nothing to drain
    // - and nothing to call it on either, which is what this used to try.
    if (!vkCtx_) {
        loadedModels.clear();
        instances.clear();
        spatialGrid.clear();
        instanceIndexById.clear();
        initialized_ = false;
        return;
    }

    VkDevice device = vkCtx_->getDevice();
    VmaAllocator allocator = vkCtx_->getAllocator();

    vkDeviceWaitIdle(device);

    // Free all GPU resources for loaded models
    for (auto& [id, model] : loadedModels) {
        releaseRtModel(id);
        for (auto& group : model.groups) {
            destroyGroupGPU(group);
        }
    }

    // Free cached textures
    for (auto& [path, entry] : textureCache) {
        if (entry.texture) entry.texture->destroy(device, allocator);
        if (entry.normalHeightMap) entry.normalHeightMap->destroy(device, allocator);
    }
    textureCache.clear();
    textureCacheBytes_ = 0;
    textureCacheCounter_ = 0;
    failedTextureCache_.clear();
    failedTextureRetryAt_.clear();
    loggedTextureLoadFails_.clear();
    textureLookupSerial_ = 0;
    textureBudgetRejectWarnings_ = 0;

    // Free white texture and flat normal texture
    if (whiteTexture_) { whiteTexture_->destroy(device, allocator); whiteTexture_.reset(); }
    if (flatNormalTexture_) { flatNormalTexture_->destroy(device, allocator); flatNormalTexture_.reset(); }

    loadedModels.clear();
    instances.clear();
    spatialGrid.clear();
    instanceIndexById.clear();

    // destroyGroupGPU defers its frees, and those lambdas release descriptor
    // sets from the pool destroyed just below. Drain them here, while the pool
    // is still valid and before the pipelines and pools go: no further frames
    // will run to drain the queue, so anything left in it either leaks or is
    // run later against a dead pool.
    vkCtx_->flushDeferredCleanup();

    // Destroy pipelines
    destroy(device, opaquePipeline_);
    destroy(device, transparentPipeline_);
    destroy(device, wireframePipeline_);
    destroy(device, opaqueCulledPipeline_);
    destroy(device, transparentCulledPipeline_);
    destroy(device, pipelineLayout_);
    destroy(device, materialDescPool_);
    destroy(device, materialSetLayout_);

    // Destroy shadow resources
    destroy(device, shadowPipeline_);
    destroy(device, shadowPipelineLayout_);
    destroyShadowParamsSet(device, allocator, shadowParams_);

    vkCtx_ = nullptr;
    initialized_ = false;
}

bool WMORenderer::loadModel(const pipeline::WMOModel& model, uint32_t id) {
    // No budget: run to completion, which is what callers outside terrain
    // streaming expect.
    for (;;) {
        const ModelLoadResult r = loadModelIncremental(model, id, 0.0f);
        if (r == ModelLoadResult::InProgress) continue;
        return r == ModelLoadResult::Complete;
    }
}

WMORenderer::ModelLoadResult WMORenderer::loadModelIncremental(
        const pipeline::WMOModel& model, uint32_t id, float budgetMs) {
    if (!model.isValid()) {
        core::Logger::getInstance().error("Cannot load invalid WMO model");
        return ModelLoadResult::Failed;
    }

    // Check if already loaded
    auto existingIt = loadedModels.find(id);
    if (existingIt != loadedModels.end()) {
        // If a model was first loaded while texture resolution failed (or before
        // assets were fully available), it can remain permanently white because
        // merged batches cache texture pointers at load time. Do a one-time reload for
        // models that have texture paths but no resolved non-white textures.
        if (assetManager && !model.textures.empty()) {
            bool hasResolvedTexture = false;
            for (VkTexture* tex : existingIt->second.textures) {
                if (tex != nullptr && tex != whiteTexture_.get()) {
                    hasResolvedTexture = true;
                    break;
                }
            }
            // Track which WMO models have been force-reloaded after resolving only to
            // fallback textures. Cap the set to avoid unbounded memory growth in worlds
            // with many unique WMO groups (e.g. Dalaran has 2000+).
            static constexpr size_t kMaxRetryTracked = 8192;
            static std::unordered_set<uint32_t> retryReloadedModels;
            static bool retryReloadedModelsCapped = false;
            if (retryReloadedModels.size() > kMaxRetryTracked) {
                retryReloadedModels.clear();
                if (!retryReloadedModelsCapped) {
                    core::Logger::getInstance().warning("WMO fallback-retry set exceeded ", kMaxRetryTracked, " entries; reset");
                    retryReloadedModelsCapped = true;
                }
            }
            if (!hasResolvedTexture && retryReloadedModels.insert(id).second) {
                core::Logger::getInstance().warning(
                    "WMO model ", id,
                    " has only fallback textures; forcing one-time reload");
                unloadModel(id);
            } else {
                return ModelLoadResult::Complete;
            }
        } else {
            return ModelLoadResult::Complete;
        }
    }
    if (loadedModels.size() >= modelCacheLimit_) {
        if (modelLimitRejectWarnings_ < 3) {
            core::Logger::getInstance().warning("WMO model cache full (",
                                                loadedModels.size(), "/", modelCacheLimit_,
                                                "), skipping model load: id=", id);
        }
        ++modelLimitRejectWarnings_;
        return ModelLoadResult::Failed;
    }

    core::Logger::getInstance().debug("Loading WMO model ", id, " with ", model.groups.size(), " groups, ",
                                      model.textures.size(), " textures...");

    // The accumulating model lives across calls, so a resumed load picks up its
    // textures, materials and the groups already uploaded.
    ModelData& modelData = loadingModels_[id];
    modelData.id = id;
    modelData.rootId = model.rootId;
    modelData.sourcePath = model.sourcePath;
    modelData.boundingBoxMin = model.boundingBoxMin;
    modelData.boundingBoxMax = model.boundingBoxMax;
    modelData.wmoAmbientColor = model.ambientColor;
    modelData.fogs = model.fogs;
    modelData.lights = model.lights;
    {
        glm::vec3 ext = model.boundingBoxMax - model.boundingBoxMin;
        float horiz = std::max(ext.x, ext.y);
        float vert = ext.z;
        modelData.isLowPlatform = (vert < 6.0f && horiz > 20.0f);
    }

    core::Logger::getInstance().debug("  WMO bounds: min=(", model.boundingBoxMin.x, ", ", model.boundingBoxMin.y, ", ", model.boundingBoxMin.z,
                                      ") max=(", model.boundingBoxMax.x, ", ", model.boundingBoxMax.y, ", ", model.boundingBoxMax.z, ")");

    // Batch all GPU uploads (textures, VBs, IBs) into a single command buffer
    // submission with one fence wait, instead of one per upload.
    if (vkCtx_) vkCtx_->beginUploadBatch();

    // Textures and materials are model-level and done once; a resumed call has
    // them already and goes straight to the remaining groups.
    if (!modelData.setupDone) {

    // Load textures for this model
    core::Logger::getInstance().debug("  WMO has ", model.textures.size(), " texture paths, ", model.materials.size(), " materials");
    if (assetManager && !model.textures.empty()) {
        const auto texStart = std::chrono::steady_clock::now();
        for (size_t i = modelData.nextTextureIndex; i < model.textures.size(); i++) {
            if (budgetMs > 0.0f && i > modelData.nextTextureIndex) {
                const float spent = std::chrono::duration<float, std::milli>(
                    std::chrono::steady_clock::now() - texStart).count();
                if (spent >= budgetMs) {
                    modelData.nextTextureIndex = i;
                    if (vkCtx_) vkCtx_->endUploadBatch();
                    return ModelLoadResult::InProgress;  // resume at this texture
                }
            }
            const auto& texPath = model.textures[i];
            core::Logger::getInstance().debug("    Loading texture ", i, ": ", texPath);
            VkTexture* tex = loadTexture(texPath);
            modelData.textures.push_back(tex);
            // Store lowercase texture name for material detection
            std::string lowerPath = texPath;
            std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            modelData.textureNames.push_back(lowerPath);
        }
        core::Logger::getInstance().debug("  Loaded ", modelData.textures.size(), " textures for WMO");
    }

    // Store material -> texture index mapping
    // IMPORTANT: mat.texture1 is a byte offset into MOTX, not an array index!
    // We need to convert it using the textureOffsetToIndex map
    core::Logger::getInstance().debug("  textureOffsetToIndex map has ", model.textureOffsetToIndex.size(), " entries");
    static int matLogCount = 0;
    auto resolveTextureIndex = [&](uint32_t textureField) -> uint32_t {
        auto it = model.textureOffsetToIndex.find(textureField);
        if (it != model.textureOffsetToIndex.end()) {
            return it->second;
        }
        // Some files may store direct index instead of MOTX byte offset.
        if (textureField < model.textures.size()) {
            return textureField;
        }
        return std::numeric_limits<uint32_t>::max();
    };

    for (size_t i = 0; i < model.materials.size(); i++) {
        const auto& mat = model.materials[i];
        uint32_t texIndex = 0;  // Default to first texture
        const uint32_t t1 = resolveTextureIndex(mat.texture1);
        const uint32_t t2 = resolveTextureIndex(mat.texture2);
        const uint32_t t3 = resolveTextureIndex(mat.texture3);

        // Prefer first valid non-empty texture among texture1/2/3.
        auto pickValid = [&](uint32_t idx) -> bool {
            if (idx == std::numeric_limits<uint32_t>::max()) return false;
            if (idx >= model.textures.size()) return false;
            if (model.textures[idx].empty()) return false;
            texIndex = idx;
            return true;
        };
        if (!pickValid(t1)) {
            if (!pickValid(t2)) {
                pickValid(t3);
            }
        }

        if (matLogCount < 20) {
            core::Logger::getInstance().debug("  Material ", i,
                ": tex1=", mat.texture1, "->", t1,
                " tex2=", mat.texture2, "->", t2,
                " tex3=", mat.texture3, "->", t3,
                " chosen=", texIndex);
            matLogCount++;
        }

        modelData.materialTextureIndices.push_back(texIndex);
        modelData.materialBlendModes.push_back(mat.blendMode);
        modelData.materialShaders.push_back(mat.shader);
        modelData.materialFlags.push_back(mat.flags);
        modelData.materialSidnColors.push_back(mat.color1);
        modelData.materialTerrainTypes.push_back(mat.terrainType);

    }


    modelData.nextTextureIndex = model.textures.size();
    modelData.setupDone = true;
    }  // end one-time setup

    // Create GPU resources for each group, a bounded number per call. A model
    // with hundreds of groups would otherwise upload them all in one step: the
    // worst measured was 286 groups at 131ms, against an 8ms budget.
    const auto groupStart = std::chrono::steady_clock::now();
    for (size_t gi = modelData.nextGroupIndex; gi < model.groups.size(); gi++) {
        if (budgetMs > 0.0f) {
            const float spent = std::chrono::duration<float, std::milli>(
                std::chrono::steady_clock::now() - groupStart).count();
            if (spent >= budgetMs && gi + 1 < model.groups.size()) {
                // Resume AT this group, not after it. Marking it done before
                // deciding whether to stop dropped one group on the floor at
                // every budget break, and a dropped group is a missing piece of
                // the building - the interior floors past a doorway in
                // Stormwind, on a model big enough to break several times.
                modelData.nextGroupIndex = gi;
                if (vkCtx_) vkCtx_->endUploadBatch();
                return ModelLoadResult::InProgress;
            }
        }
        modelData.nextGroupIndex = gi + 1;
        const auto& wmoGroup = model.groups[gi];
        // Skip empty groups
        if (wmoGroup.vertices.empty() || wmoGroup.indices.empty()) {
            continue;
        }

        GroupResources resources;
        resources.authoredIndex = static_cast<uint32_t>(gi);
        // Every group is drawn at every distance: 2.4.3 has no distance-only
        // groups. What a camera sees of a building is chosen by portals alone.
        if (createGroupResources(wmoGroup, resources, wmoGroup.flags, modelData.wmoAmbientColor)) {
            modelData.groups.push_back(resources);
            modelData.loadedGroups++;
        }
    }

    if (modelData.loadedGroups == 0) {
        core::Logger::getInstance().warning("No valid groups loaded for WMO ", id);
        if (vkCtx_) vkCtx_->endUploadBatch();
        loadingModels_.erase(id);
        return ModelLoadResult::Failed;
    }

    // Build pre-merged batches for each group (texture-sorted for efficient rendering)
    for (auto& groupRes : modelData.groups) {
        // Use pointer value as key for batching
        struct BatchKey {
            uintptr_t texPtr;
            bool alphaTest;
            bool unlit;
            bool specular;
            bool specularAlphaMask;
            uint8_t emissiveLevel;
            uint32_t sidnColor;
            bool twoSided;
            bool operator==(const BatchKey& o) const {
                return texPtr == o.texPtr && alphaTest == o.alphaTest && twoSided == o.twoSided &&
                       unlit == o.unlit && sidnColor == o.sidnColor &&
                       specular == o.specular &&
                       specularAlphaMask == o.specularAlphaMask &&
                       emissiveLevel == o.emissiveLevel;
            }
        };
        struct BatchKeyHash {
            size_t operator()(const BatchKey& k) const {
                return std::hash<uintptr_t>()(k.texPtr) ^
                       (std::hash<bool>()(k.alphaTest) << 1) ^
                       (std::hash<bool>()(k.unlit) << 2) ^
                       (std::hash<uint32_t>()(k.sidnColor) << 3) ^
                       (std::hash<uint8_t>()(k.emissiveLevel) << 4) ^
                       (std::hash<bool>()(k.specular) << 5) ^
                       (std::hash<bool>()(k.specularAlphaMask) << 6) ^
                       (std::hash<bool>()(k.twoSided) << 7);
            }
        };
        std::unordered_map<BatchKey, GroupResources::MergedBatch, BatchKeyHash> batchMap;

        for (const auto& batch : groupRes.batches) {
            VkTexture* tex = whiteTexture_.get();
            bool hasTexture = false;

            if (batch.materialId < modelData.materialTextureIndices.size()) {
                uint32_t texIndex = modelData.materialTextureIndices[batch.materialId];
                if (texIndex < modelData.textures.size()) {
                    tex = modelData.textures[texIndex];
                    hasTexture = (tex != nullptr && tex != whiteTexture_.get());
                    if (!tex) tex = whiteTexture_.get();
                } else {
                    LOG_WARNING("WMO ", id, " batch materialId=", batch.materialId,
                                " texIndex=", texIndex, " >= textures size ",
                                modelData.textures.size(), " - white fallback");
                }
            } else {
                LOG_WARNING("WMO ", id, " batch materialId=", batch.materialId,
                            " >= materialTextureIndices size ",
                            modelData.materialTextureIndices.size(), " - white fallback");
            }

            bool alphaTest = false;
            uint32_t blendMode = 0;
            if (batch.materialId < modelData.materialBlendModes.size()) {
                blendMode = modelData.materialBlendModes[batch.materialId];
                alphaTest = (blendMode == 1);
            }

            bool unlit = false;
            uint32_t matFlags = 0;
            if (batch.materialId < modelData.materialFlags.size()) {
                matFlags = modelData.materialFlags[batch.materialId];
                unlit = (matFlags & 0x01) != 0;
            }

            // TBC uses the authored MOMT blend mode. Non-TBC profiles add the
            // sidnColor emission of F_SIDN materials to their lighting.
            const bool nightGlow = !game::isActiveExpansion("tbc") && wmoMaterialIsNightGlow(matFlags);
            const uint8_t emissiveLevel = nightGlow ? 1 : 0;
            const uint32_t sidnColor = nightGlow ? modelData.materialSidnColors.at(batch.materialId) : 0u;


            BatchKey key{ .texPtr = reinterpret_cast<uintptr_t>(tex), .alphaTest = alphaTest, .unlit = unlit,
                          .specular = !game::isActiveExpansion("tbc") ||
                              wmoShaderUsesSpecular(modelData.materialShaders.at(batch.materialId)),
                          .specularAlphaMask = game::isActiveExpansion("tbc") &&
                              modelData.materialShaders.at(batch.materialId) == 1,
                          .emissiveLevel = emissiveLevel, .sidnColor = sidnColor,
                          .twoSided = (matFlags & 0x04) != 0 };
            auto& mb = batchMap[key];
            if (mb.draws.empty()) {
                mb.texture = tex;
                mb.hasTexture = hasTexture;
                mb.alphaTest = alphaTest;
                mb.unlit = unlit;
                mb.isTransparent = (blendMode >= 2);
                mb.twoSided = key.twoSided;
                mb.emissiveLevel = emissiveLevel;
                mb.sidnColor = sidnColor;
                // Look up normal/height map from texture cache
                if (hasTexture && tex != whiteTexture_.get()) {
                    for (const auto& [cacheKey, cacheEntry] : textureCache) {
                        if (cacheEntry.texture.get() == tex) {
                            mb.normalHeightMap = cacheEntry.normalHeightMap.get();
                            mb.heightMapVariance = cacheEntry.heightMapVariance;
                            break;
                        }
                    }
                }
            }
            GroupResources::MergedBatch::DrawRange dr;
            dr.firstIndex = batch.startIndex;
            dr.indexCount = batch.indexCount;
            mb.draws.push_back(dr);
        }

        // Allocate descriptor sets and UBOs for each merged batch
        groupRes.mergedBatches.reserve(batchMap.size());
        bool anyTextured = false;
        bool isInterior = pipeline::wmoUsesLocalLighting(groupRes.groupFlags);
        for (auto& [key, mb] : batchMap) {
            if (mb.hasTexture) anyTextured = true;
            if (!vkCtx_) {
                groupRes.mergedBatches.push_back(std::move(mb));
                continue;
            }

            // Create material UBO
            VmaAllocator allocator = vkCtx_->getAllocator();
            AllocatedBuffer matBuf = createBuffer(allocator, sizeof(WMOMaterialUBO),
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VMA_MEMORY_USAGE_CPU_TO_GPU);
            mb.materialUBO = matBuf.buffer;
            mb.materialUBOAlloc = matBuf.allocation;

            // Write material params
            WMOMaterialUBO matData{};
            matData.hasTexture = mb.hasTexture ? 1 : 0;
            matData.alphaTest = mb.alphaTest ? 1 : 0;
            matData.unlit = mb.unlit ? 1 : 0;
            matData.isInterior = isInterior ? 1 : 0;
            matData.hasVertexColors = (groupRes.groupFlags & 0x4) != 0 ? 1 : 0;
            matData.specularIntensity = key.specular ? 0.5f : 0.0f;
            matData.specularAlphaMask = key.specularAlphaMask ? 1 : 0;
            matData.enableNormalMap = normalMappingEnabled_ ? 1 : 0;
            matData.enablePOM = pomEnabled_ ? 1 : 0;
            matData.pomScale = 0.012f;
            matData.pomMaxSamples = pomSamplesFor(pomQuality_);
            matData.heightMapVariance = mb.heightMapVariance;
            matData.normalMapStrength = normalMapStrength_;
            matData.wmoAmbientR = modelData.wmoAmbientColor.r;
            matData.wmoAmbientG = modelData.wmoAmbientColor.g;
            matData.wmoAmbientB = modelData.wmoAmbientColor.b;
            matData.emissive = static_cast<int32_t>(mb.emissiveLevel);
            matData.sidnColor = static_cast<int32_t>(mb.sidnColor);
            if (matBuf.info.pMappedData) {
                memcpy(matBuf.info.pMappedData, &matData, sizeof(matData));
            }

            // Allocate and write descriptor set
            mb.materialSet = allocateMaterialSet();
            // Valid, not merely non-null. descriptorInfo() returns the
            // texture's handles as they are, so one whose upload or view
            // creation failed writes VK_NULL_HANDLE into a live descriptor and
            // declares SHADER_READ_ONLY_OPTIMAL over it - undefined behaviour
            // that reaches an NVIDIA driver as a lost device. See #123.
            //
            // The set is dropped rather than written when even the fallback is
            // unsampleable, and the render pass already skips a batch with no
            // set: an unlit wall costs a wall, a null view costs the device.
            const auto pick = [](VkTexture* wanted, VkTexture* fallback) -> VkTexture* {
                if (wanted && wanted->isValid()) return wanted;
                return (fallback && fallback->isValid()) ? fallback : nullptr;
            };
            VkTexture* texToUse = pick(mb.texture, whiteTexture_.get());
            VkTexture* nhMap = pick(mb.normalHeightMap, flatNormalTexture_.get());
            if ((!texToUse || !nhMap) && mb.materialSet) {
                vkFreeDescriptorSets(vkCtx_->getDevice(), materialDescPool_, 1,
                                     &mb.materialSet);
                mb.materialSet = VK_NULL_HANDLE;
            }
            if (mb.materialSet) {
                VkDescriptorImageInfo imgInfo = texToUse->descriptorInfo();

                VkDescriptorBufferInfo bufInfo{};
                bufInfo.buffer = mb.materialUBO;
                bufInfo.offset = 0;
                bufInfo.range = sizeof(WMOMaterialUBO);

                VkDescriptorImageInfo nhImgInfo = nhMap->descriptorInfo();

                VkWriteDescriptorSet writes[3] = {};
                writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[0].dstSet = mb.materialSet;
                writes[0].dstBinding = 0;
                writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[0].descriptorCount = 1;
                writes[0].pImageInfo = &imgInfo;

                writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[1].dstSet = mb.materialSet;
                writes[1].dstBinding = 1;
                writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                writes[1].descriptorCount = 1;
                writes[1].pBufferInfo = &bufInfo;

                writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[2].dstSet = mb.materialSet;
                writes[2].dstBinding = 2;
                writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[2].descriptorCount = 1;
                writes[2].pImageInfo = &nhImgInfo;

                vkUpdateDescriptorSets(vkCtx_->getDevice(), 3, writes, 0, nullptr);
            }

            groupRes.mergedBatches.push_back(std::move(mb));
        }
        groupRes.allUntextured = !anyTextured && !groupRes.mergedBatches.empty();
    }

    if (vkCtx_) vkCtx_->endUploadBatch();

    // Copy portal data for visibility culling
    modelData.portalVertices = model.portalVertices;
    for (const auto& portal : model.portals) {
        PortalData pd;
        pd.startVertex = portal.startVertex;
        pd.vertexCount = portal.vertexCount;
        pd.normal = portal.normal;
        pd.distance = portal.distance;
        modelData.portals.push_back(pd);
    }
    for (const auto& ref : model.portalRefs) {
        PortalRef pr;
        pr.portalIndex = ref.portalIndex;
        pr.groupIndex = ref.groupIndex;
        pr.side = ref.side;
        modelData.portalRefs.push_back(pr);
    }
    // Build per-group portal ref ranges from WMOGroup data
    modelData.groupPortalRefs.resize(model.groups.size(), {0, 0});
    for (size_t gi = 0; gi < model.groups.size(); gi++) {
        modelData.groupPortalRefs[gi] = {model.groups[gi].portalStart, model.groups[gi].portalCount};
    }

    if (!modelData.portals.empty()) {
        core::Logger::getInstance().debug("WMO portals: ", modelData.portals.size(),
                                          " refs: ", modelData.portalRefs.size());
    }

    // Store doodad templates (M2 models placed in WMO) for instancing later.
    // Populate doodadTemplates with set 0 (Set_$DefaultGlobal) via
    // selectWmoDoodadIndices so inactive sets are never included by default.
    if (!model.doodadSets.empty() && !model.doodads.empty()) {
        const auto activeIndices = pipeline::selectWmoDoodadIndices(
            model.doodadSets, model.doodads.size(), /*activeSetIndex=*/0);
        modelData.doodadTemplates.reserve(activeIndices.size());
        for (uint32_t doodadIdx : activeIndices) {
            const auto& doodad = model.doodads[doodadIdx];
            auto nameIt = model.doodadNames.find(doodad.nameIndex);
            if (nameIt == model.doodadNames.end()) continue;

            std::string m2Path = nameIt->second;
            if (m2Path.empty()) continue;

            m2Path = pipeline::modelPathToM2(m2Path);

            // Build doodad's local transform (WoW coordinates)
            // WMO doodads use quaternion rotation
            glm::quat fixedRotation(doodad.rotation.w, doodad.rotation.x, doodad.rotation.y, doodad.rotation.z);

            glm::mat4 localTransform(1.0f);
            localTransform = glm::translate(localTransform, doodad.position);
            localTransform *= glm::mat4_cast(fixedRotation);
            localTransform = glm::scale(localTransform, glm::vec3(doodad.scale));

            DoodadTemplate doodadTemplate;
            doodadTemplate.m2Path = std::move(m2Path);
            doodadTemplate.localTransform = localTransform;
            modelData.doodadTemplates.push_back(std::move(doodadTemplate));
        }

        if (!modelData.doodadTemplates.empty()) {
            core::Logger::getInstance().debug("WMO has ", modelData.doodadTemplates.size(), " doodad templates");
        }
    }

    modelData.setupDone = true;

    // Every non-empty source group must have arrived. Uploading them across
    // several calls under a time budget makes it possible to lose one at a
    // resume point without anything else noticing: the model still loads, still
    // renders, and is simply missing pieces of itself.
    size_t expectedGroups = 0;
    for (const auto& g : model.groups) {
        if (!g.vertices.empty() && !g.indices.empty()) expectedGroups++;
    }
    if (modelData.groups.size() != expectedGroups) {
        core::Logger::getInstance().error(
            "WMO ", id, " uploaded ", modelData.groups.size(), " of ", expectedGroups,
            " groups - geometry is missing from this model");
    }

    // Read before the move: the log line below reports what was stored, and
    // modelData no longer owns it afterwards.
    const uint32_t loadedGroupCount = modelData.loadedGroups;
    registerRtModel(id, modelData);
    loadedModels[id] = std::move(modelData);
    loadingModels_.erase(id);
    core::Logger::getInstance().debug("WMO model ", id, " loaded successfully (", loadedGroupCount, " groups)");
    return ModelLoadResult::Complete;
}

bool WMORenderer::isModelLoaded(uint32_t id) const {
    return loadedModels.find(id) != loadedModels.end();
}

bool WMORenderer::instanceHasCollisionGeometry(uint32_t instanceId) const {
    auto it = std::find_if(instances.begin(), instances.end(),
                           [instanceId](const WMOInstance& inst) { return inst.id == instanceId; });
    if (it == instances.end()) return false;
    auto model = loadedModels.find(it->modelId);
    return model != loadedModels.end() && model->second.setupDone &&
           !model->second.groups.empty();
}

void WMORenderer::unloadModel(uint32_t id) {
    auto it = loadedModels.find(id);
    if (it == loadedModels.end()) {
        return;
    }
    releaseRtModel(id);

    // Free GPU resources - defer because in-flight command buffers may
    // still reference this model's vertex/index buffers and descriptors.
    for (auto& group : it->second.groups) {
        destroyGroupGPU(group, /*defer=*/true);
    }

    loadedModels.erase(it);
    core::Logger::getInstance().info("WMO model ", id, " unloaded");
}

void WMORenderer::cleanupUnusedModels() {
    // Build set of model IDs that are still referenced by instances
    std::unordered_set<uint32_t> usedModelIds;
    for (const auto& instance : instances) {
        usedModelIds.insert(instance.modelId);
    }

    // Find and remove models with no instances
    std::vector<uint32_t> toRemove;
    for (const auto& [id, model] : loadedModels) {
        if (usedModelIds.find(id) == usedModelIds.end()) {
            toRemove.push_back(id);
        }
    }

    // unloadModel() routes every group buffer, material UBO, and descriptor
    // through deferAfterAllFrameFences(). Do not stall the entire device here;
    // periodic cleanup can otherwise introduce a visible hitch every time a
    // streamed WMO leaves the active set.
    for (uint32_t id : toRemove) {
        unloadModel(id);
    }

    if (!toRemove.empty()) {
        core::Logger::getInstance().info("WMO cleanup: removed ", toRemove.size(), " unused models, ", loadedModels.size(), " remaining");
    }
}

uint32_t WMORenderer::createInstance(uint32_t modelId, const glm::vec3& position,
                                     const glm::vec3& rotation, float scale, uint32_t nameSet) {
    // Check if model is loaded
    if (loadedModels.find(modelId) == loadedModels.end()) {
        core::Logger::getInstance().error("Cannot create instance of unloaded WMO model ", modelId);
        return 0;
    }

    WMOInstance instance;
    instance.id = nextInstanceId++;
    instance.nameSet = nameSet;
    instance.modelId = modelId;
    instance.position = position;
    instance.rotation = rotation;
    instance.scale = scale;
    instance.updateModelMatrix();
    const ModelData& model = loadedModels[modelId];
    transformAABB(instance.modelMatrix, model.boundingBoxMin, model.boundingBoxMax,
                  instance.worldBoundsMin, instance.worldBoundsMax);

    // Pre-compute world-space group bounds to avoid per-frame transformAABB
    instance.worldGroupBounds.reserve(model.groups.size());
    for (const auto& group : model.groups) {
        glm::vec3 gMin, gMax;
        transformAABB(instance.modelMatrix, group.boundingBoxMin, group.boundingBoxMax, gMin, gMax);
        gMin -= glm::vec3(0.5f);
        gMax += glm::vec3(0.5f);
        instance.worldGroupBounds.emplace_back(gMin, gMax);
    }

    instances.push_back(instance);
    size_t idx = instances.size() - 1;
    instanceIndexById[instance.id] = idx;
    insertBounds(spatialGrid, instance.worldBoundsMin, instance.worldBoundsMax, instance.id);
    core::Logger::getInstance().debug("Created WMO instance ", instance.id, " (model ", modelId, ")");
    return instance.id;
}

uint32_t WMORenderer::createInstanceWithMatrix(uint32_t modelId, const glm::mat4& modelMatrix,
                                               uint32_t nameSet) {
    if (loadedModels.find(modelId) == loadedModels.end()) {
        core::Logger::getInstance().error("Cannot create instance of unloaded WMO model ", modelId);
        return 0;
    }

    WMOInstance instance;
    instance.id = nextInstanceId++;
    instance.nameSet = nameSet;
    instance.modelId = modelId;
    instance.position = glm::vec3(modelMatrix[3]);
    instance.rotation = glm::vec3(0.0f);
    instance.scale = glm::length(glm::vec3(modelMatrix[0]));
    instance.modelMatrix = modelMatrix;
    instance.invModelMatrix = glm::inverse(modelMatrix);
    refreshInstanceBounds(instance);

    instances.push_back(instance);
    size_t idx = instances.size() - 1;
    instanceIndexById[instance.id] = idx;
    insertBounds(spatialGrid, instance.worldBoundsMin, instance.worldBoundsMax, instance.id);
    core::Logger::getInstance().debug("Created WMO instance (matrix) ", instance.id, " (model ", modelId, ")");
    return instance.id;
}

/// Recomputes an instance's world bounds from its model matrix.
///
/// Called whenever the matrix changes, which happens two ways: a plain move,
/// and a transport being handed a whole transform by the server. Both used to
/// do this themselves.
///
/// The half-unit of padding on each group's box is deliberate. The bounds are
/// what a collision query tests before it looks at triangles, and a box fitted
/// exactly to its geometry rejects a query that starts on the surface, so a
/// character standing on a floor can fail to find the floor it is standing on.
void WMORenderer::refreshInstanceBounds(WMOInstance& inst) {
    auto modelIt = loadedModels.find(inst.modelId);
    if (modelIt == loadedModels.end()) return;

    const ModelData& model = modelIt->second;
    transformAABB(inst.modelMatrix, model.boundingBoxMin, model.boundingBoxMax,
                  inst.worldBoundsMin, inst.worldBoundsMax);
    inst.worldGroupBounds.clear();
    inst.worldGroupBounds.reserve(model.groups.size());
    for (const auto& group : model.groups) {
        glm::vec3 gMin, gMax;
        transformAABB(inst.modelMatrix, group.boundingBoxMin, group.boundingBoxMax, gMin, gMax);
        gMin -= glm::vec3(0.5f);
        gMax += glm::vec3(0.5f);
        inst.worldGroupBounds.emplace_back(gMin, gMax);
    }
}

void WMORenderer::setInstancePosition(uint32_t instanceId, const glm::vec3& position) {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return;
    auto& inst = instances[idxIt->second];
    inst.position = position;
    inst.updateModelMatrix();
    refreshInstanceBounds(inst);
    rebuildSpatialIndex();
}

bool WMORenderer::getInstanceWorldBounds(uint32_t instanceId, glm::vec3& outMin,
                                         glm::vec3& outMax) const {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end() || idxIt->second >= instances.size()) return false;
    const auto& inst = instances[idxIt->second];
    outMin = inst.worldBoundsMin;
    outMax = inst.worldBoundsMax;
    return true;
}

void WMORenderer::setInstanceIsTransport(uint32_t instanceId, bool isTransport) {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return;
    instances[idxIt->second].isTransport = isTransport;
}

void WMORenderer::setInstanceHidden(uint32_t instanceId, bool hidden) {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return;
    instances[idxIt->second].hidden = hidden;
}

void WMORenderer::setInstanceTransform(uint32_t instanceId, const glm::mat4& transform) {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return;
    auto& inst = instances[idxIt->second];

    // Decompose transform to position/rotation/scale
    inst.position = glm::vec3(transform[3]);

    // Extract rotation (assuming uniform scale)
    glm::mat3 rotationMatrix(transform);
    float scaleX = glm::length(glm::vec3(transform[0]));
    float scaleY = glm::length(glm::vec3(transform[1]));
    float scaleZ = glm::length(glm::vec3(transform[2]));
    inst.scale = scaleX;  // Assume uniform scale

    if (scaleX > 0.0001f) rotationMatrix[0] /= scaleX;
    if (scaleY > 0.0001f) rotationMatrix[1] /= scaleY;
    if (scaleZ > 0.0001f) rotationMatrix[2] /= scaleZ;

    inst.rotation = glm::vec3(0.0f);  // Euler angles not directly used, so zero them

    // Update model matrix and bounds
    inst.modelMatrix = transform;
    inst.invModelMatrix = glm::inverse(transform);

    refreshInstanceBounds(inst);

    // Propagate transform to child M2 doodads (chairs, furniture on transports)
    if (m2Renderer_ && !inst.doodads.empty()) {
        for (const auto& doodad : inst.doodads) {
            glm::mat4 worldTransform = inst.modelMatrix * doodad.localTransform;
            m2Renderer_->setInstanceTransform(doodad.m2InstanceId, worldTransform);
        }
    }

    rebuildSpatialIndex();
}

void WMORenderer::addDoodadToInstance(uint32_t instanceId, uint32_t m2InstanceId, const glm::mat4& localTransform) {
    auto it = std::find_if(instances.begin(), instances.end(),
                          [instanceId](const WMOInstance& inst) { return inst.id == instanceId; });
    if (it == instances.end()) {
        // Nothing to parent to. The M2 was already created, so silently dropping
        // it here leaves it stranded at the origin, drawn nowhere and owned by
        // no one - invisible in a way that looks exactly like a load failure.
        core::Logger::getInstance().warning(
            "WMO doodad has no parent instance ", instanceId,
            " - M2 instance ", m2InstanceId, " left at the origin");
        return;
    }
    WMOInstance::DoodadInfo doodad;
    doodad.m2InstanceId = m2InstanceId;
    doodad.localTransform = localTransform;
    it->doodads.push_back(doodad);

    // Place it immediately rather than waiting for the parent's next transform
    // push, so it is never drawn at the origin for a frame and never sits there
    // indefinitely if the parent is static.
    if (m2Renderer_) {
        m2Renderer_->setInstanceTransform(m2InstanceId, it->modelMatrix * localTransform);
    }
}

size_t WMORenderer::setInstanceDoodadAnimation(uint32_t instanceId, uint32_t animationId,
                                               bool loop) {
    if (!m2Renderer_) return 0;
    auto it = std::find_if(instances.begin(), instances.end(),
                           [instanceId](const WMOInstance& inst) { return inst.id == instanceId; });
    if (it == instances.end()) return 0;
    for (const auto& doodad : it->doodads) {
        // A no-op for a doodad without that sequence, so the barrels and
        // lanterns sharing the deck are left alone.
        m2Renderer_->setInstanceAnimation(doodad.m2InstanceId, animationId, loop);
    }
    return it->doodads.size();
}

const std::vector<WMORenderer::DoodadTemplate>* WMORenderer::getDoodadTemplates(uint32_t modelId) const {
    auto it = loadedModels.find(modelId);
    if (it != loadedModels.end() && !it->second.doodadTemplates.empty()) {
        return &it->second.doodadTemplates;
    }
    return nullptr;
}

bool WMORenderer::hasInstance(uint32_t instanceId) const {
    return std::find_if(instances.begin(), instances.end(),
                        [instanceId](const WMOInstance& inst) { return inst.id == instanceId; })
           != instances.end();
}

void WMORenderer::removeInstance(uint32_t instanceId) {
    auto it = std::find_if(instances.begin(), instances.end(),
                          [instanceId](const WMOInstance& inst) { return inst.id == instanceId; });
    if (it != instances.end()) {
        if (m2Renderer_) {
            for (const auto& doodad : it->doodads) {
                m2Renderer_->removeInstance(doodad.m2InstanceId);
            }
        }
        instances.erase(it);
        rebuildSpatialIndex();
        core::Logger::getInstance().debug("Removed WMO instance ", instanceId);
    }
}

void WMORenderer::removeInstances(const std::vector<uint32_t>& instanceIds) {
    if (instanceIds.empty() || instances.empty()) {
        return;
    }

    std::unordered_set<uint32_t> toRemove(instanceIds.begin(), instanceIds.end());
    if (m2Renderer_) {
        for (const auto& inst : instances) {
            if (toRemove.find(inst.id) == toRemove.end()) {
                continue;
            }
            for (const auto& doodad : inst.doodads) {
                m2Renderer_->removeInstance(doodad.m2InstanceId);
            }
        }
    }

    const size_t oldSize = instances.size();
    instances.erase(std::remove_if(instances.begin(), instances.end(),
                   [&toRemove](const WMOInstance& inst) {
                       return toRemove.find(inst.id) != toRemove.end();
                   }),
                   instances.end());

    if (instances.size() != oldSize) {
        rebuildSpatialIndex();
        core::Logger::getInstance().debug("Removed ", (oldSize - instances.size()),
                                          " WMO instances (batched)");
    }
}

void WMORenderer::clearInstances() {
    if (m2Renderer_) {
        for (const auto& inst : instances) {
            for (const auto& doodad : inst.doodads) {
                m2Renderer_->removeInstance(doodad.m2InstanceId);
            }
        }
    }
    instances.clear();
    spatialGrid.clear();
    instanceIndexById.clear();
    precomputedFloorGrid.clear();  // Invalidate floor cache when instances change
    core::Logger::getInstance().info("Cleared all WMO instances");
}

void WMORenderer::clearAll() {
    clearInstances();

    if (vkCtx_) {
        VkDevice device = vkCtx_->getDevice();
        VmaAllocator allocator = vkCtx_->getAllocator();
        vkDeviceWaitIdle(device);

        // Free GPU resources for loaded models
        for (auto& [id, model] : loadedModels) {
            releaseRtModel(id);
            for (auto& group : model.groups) {
                destroyGroupGPU(group);
            }
        }

        // Free cached textures
        for (auto& [path, entry] : textureCache) {
            if (entry.texture) entry.texture->destroy(device, allocator);
            if (entry.normalHeightMap) entry.normalHeightMap->destroy(device, allocator);
        }

        // Reset descriptor pool so new allocations succeed after reload
        if (materialDescPool_) {
            vkResetDescriptorPool(device, materialDescPool_, 0);
        }
    }

    loadedModels.clear();
    textureCache.clear();
    textureCacheBytes_ = 0;
    textureCacheCounter_ = 0;
    failedTextureCache_.clear();
    failedTextureRetryAt_.clear();
    loggedTextureLoadFails_.clear();
    textureLookupSerial_ = 0;
    textureBudgetRejectWarnings_ = 0;
    precomputedFloorGrid.clear();

    LOG_INFO("Cleared all WMO models, instances, and texture cache");
}

void WMORenderer::setCollisionFocus(const glm::vec3& worldPos, float radius) {
    collisionFocus.set(worldPos, radius);
}
// setLighting is now a no-op (lighting is in the per-frame UBO)

void WMORenderer::resetQueryStats() {
    queryTimeMs = 0.0;
    queryCallCount = 0;
    currentFrameId++;
    // Note: precomputedFloorGrid is persistent and not cleared per-frame
}

bool WMORenderer::saveFloorCache() const {
    if (mapName_.empty()) {
        core::Logger::getInstance().warning("Cannot save floor cache: no map name set");
        return false;
    }

    std::string filepath = "cache/wmo_floor_" + mapName_ + ".bin";

    // Create directory if needed
    std::filesystem::path path(filepath);
    std::filesystem::path absPath = std::filesystem::absolute(path);
    core::Logger::getInstance().info("Saving floor cache to: ", absPath.string());

    if (path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            core::Logger::getInstance().error("Failed to create cache directory: ", ec.message());
        }
    }

    std::ofstream file(filepath, std::ios::binary);
    if (!file) {
        core::Logger::getInstance().error("Failed to open floor cache file for writing: ", filepath);
        return false;
    }

    // Write header: magic + version + count
    const uint32_t magic = 0x574D4F46;  // "WMOF"
    const uint32_t version = 1;
    const uint64_t count = precomputedFloorGrid.size();

    file.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    file.write(reinterpret_cast<const char*>(&version), sizeof(version));
    file.write(reinterpret_cast<const char*>(&count), sizeof(count));

    // Write each entry: key (uint64) + height (float)
    for (const auto& [key, height] : precomputedFloorGrid) {
        file.write(reinterpret_cast<const char*>(&key), sizeof(key));
        file.write(reinterpret_cast<const char*>(&height), sizeof(height));
    }

    core::Logger::getInstance().info("Saved WMO floor cache (", mapName_, "): ", count, " entries");
    return true;
}

bool WMORenderer::loadFloorCache() {
    if (mapName_.empty()) {
        core::Logger::getInstance().warning("Cannot load floor cache: no map name set");
        return false;
    }

    std::string filepath = "cache/wmo_floor_" + mapName_ + ".bin";

    std::ifstream file(filepath, std::ios::binary);
    if (!file) {
        core::Logger::getInstance().info("No existing floor cache for map: ", mapName_);
        return false;
    }

    // Read and validate header
    uint32_t magic = 0, version = 0;
    uint64_t count = 0;

    file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    file.read(reinterpret_cast<char*>(&version), sizeof(version));
    file.read(reinterpret_cast<char*>(&count), sizeof(count));

    if (magic != 0x574D4F46 || version != 1) {
        core::Logger::getInstance().warning("Invalid floor cache file format: ", filepath);
        return false;
    }

    // Read entries
    precomputedFloorGrid.clear();
    precomputedFloorGrid.reserve(count);

    for (uint64_t i = 0; i < count; i++) {
        uint64_t key;
        float height;
        file.read(reinterpret_cast<char*>(&key), sizeof(key));
        file.read(reinterpret_cast<char*>(&height), sizeof(height));
        precomputedFloorGrid[key] = height;
    }

    core::Logger::getInstance().info("Loaded WMO floor cache (", mapName_, "): ", precomputedFloorGrid.size(), " entries");
    return true;
}

void WMORenderer::precomputeFloorCache() {
    if (instances.empty()) {
        core::Logger::getInstance().info("precomputeFloorCache: no instances to precompute");
        return;
    }

    size_t startSize = precomputedFloorGrid.size();
    size_t samplesChecked = 0;

    core::Logger::getInstance().info("Pre-computing floor cache for ", instances.size(), " WMO instances...");

    for (const auto& instance : instances) {
        // Get world bounds for this instance
        const glm::vec3& boundsMin = instance.worldBoundsMin;
        const glm::vec3& boundsMax = instance.worldBoundsMax;

        // Sample reference Z is above the structure
        float refZ = boundsMax.z + 10.0f;

        // Iterate over grid points within the bounds
        float startX = std::floor(boundsMin.x / FLOOR_GRID_CELL_SIZE) * FLOOR_GRID_CELL_SIZE;
        float startY = std::floor(boundsMin.y / FLOOR_GRID_CELL_SIZE) * FLOOR_GRID_CELL_SIZE;

        int stepsX = static_cast<int>((boundsMax.x - startX) / FLOOR_GRID_CELL_SIZE) + 1;
        int stepsY = static_cast<int>((boundsMax.y - startY) / FLOOR_GRID_CELL_SIZE) + 1;
        for (int ix = 0; ix < stepsX; ++ix) {
            float x = startX + ix * FLOOR_GRID_CELL_SIZE;
            for (int iy = 0; iy < stepsY; ++iy) {
                float y = startY + iy * FLOOR_GRID_CELL_SIZE;
                // Sample at grid cell center
                float sampleX = x + FLOOR_GRID_CELL_SIZE * 0.5f;
                float sampleY = y + FLOOR_GRID_CELL_SIZE * 0.5f;

                // Check if already cached
                uint64_t key = floorGridKey(sampleX, sampleY);
                if (precomputedFloorGrid.find(key) != precomputedFloorGrid.end()) {
                    continue;  // Already computed
                }

                samplesChecked++;

                // Query floor height and store result in the precomputed grid
                auto h = getFloorHeight(sampleX, sampleY, refZ);
                if (h) {
                    precomputedFloorGrid[key] = *h;
                }
            }
        }
    }

    size_t newEntries = precomputedFloorGrid.size() - startSize;
    core::Logger::getInstance().info("Floor cache precompute complete: ", samplesChecked, " samples checked, ",
                                     newEntries, " new entries, total ", precomputedFloorGrid.size());
}

void WMORenderer::rebuildSpatialIndex() {
    spatialGrid.clear();
    instanceIndexById.clear();
    instanceIndexById.reserve(instances.size());

    for (size_t i = 0; i < instances.size(); i++) {
        const auto& inst = instances[i];
        instanceIndexById[inst.id] = i;

        insertBounds(spatialGrid, inst.worldBoundsMin, inst.worldBoundsMax, inst.id);
    }
}

void WMORenderer::gatherCandidates(const glm::vec3& queryMin, const glm::vec3& queryMax,
                                   std::vector<size_t>& outIndices) const {
    outIndices.clear();

    // Hidden instances are filtered here rather than in each caller: every
    // collision, floor and containment query in this file reaches the world
    // through this one function, and a transport that is on another continent
    // must not be standable on this one.
    gatherIds(spatialGrid, queryMin, queryMax, tl_candidateIdScratch,
              [&](uint32_t id) {
                  auto idxIt = instanceIndexById.find(id);
                  if (idxIt != instanceIndexById.end() && !instances[idxIt->second].hidden) {
                      outIndices.push_back(idxIt->second);
                  }
              });

    // Safety fallback: if the grid misses due streaming/index drift, avoid
    // tunneling by scanning overlapping instances instead of returning none.
    if (outIndices.empty() && !instances.empty()) {
        for (size_t i = 0; i < instances.size(); i++) {
            const auto& inst = instances[i];
            if (!inst.hidden &&
                queryMax.x >= inst.worldBoundsMin.x && queryMin.x <= inst.worldBoundsMax.x &&
                queryMax.y >= inst.worldBoundsMin.y && queryMin.y <= inst.worldBoundsMax.y &&
                queryMax.z >= inst.worldBoundsMin.z && queryMin.z <= inst.worldBoundsMax.z) {
                outIndices.push_back(i);
            }
        }
    }
}

void WMORenderer::prepareRender() {
    ++currentFrameId;

    // Update material UBOs if settings changed (mapped memory writes - main thread only)
    if (materialSettingsDirty_) {
        materialSettingsDirty_ = false;
        int maxSamples = pomSamplesFor(pomQuality_);
        for (auto& [modelId, model] : loadedModels) {
            for (auto& group : model.groups) {
                for (auto& mb : group.mergedBatches) {
                    if (!mb.materialUBO) continue;
                    VmaAllocationInfo allocInfo{};
                    vmaGetAllocationInfo(vkCtx_->getAllocator(), mb.materialUBOAlloc, &allocInfo);
                    if (allocInfo.pMappedData) {
                        auto* ubo = reinterpret_cast<WMOMaterialUBO*>(allocInfo.pMappedData);
                        ubo->enableNormalMap = normalMappingEnabled_ ? 1 : 0;
                        ubo->enablePOM = pomEnabled_ ? 1 : 0;
                        ubo->pomScale = 0.012f;
                        ubo->pomMaxSamples = maxSamples;
                        ubo->heightMapVariance = mb.heightMapVariance;
                        ubo->normalMapStrength = normalMapStrength_;
                    }
                }
            }
        }
    }
}

void WMORenderer::render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const Camera& camera,
                         const glm::vec3* viewerPos, bool mirroredView) {
    if (!opaquePipeline_ || instances.empty()) {
        lastDrawCalls = 0;
        return;
    }

    lastDrawCalls = 0;

    // Extract frustum planes for proper culling
    glm::mat4 viewProj = camera.getProjectionMatrix() * camera.getViewMatrix();
    Frustum frustum;
    frustum.extractFromMatrix(viewProj);

    lastPortalCulledGroups = 0;
    lastDistanceCulledGroups = 0;

    // ── Phase 1: Visibility culling ──────────────────────────
    // Was loadedModels.count(modelId) per instance - but cullInstance below
    // already does loadedModels.find() and bails on miss, so this pre-filter
    // was a redundant hashmap lookup per instance every frame. Just include
    // every instance; the cull step prunes unloaded ones.
    visibleInstances_.clear();
    visibleInstances_.reserve(instances.size());
    for (size_t i = 0; i < instances.size(); ++i) {
        visibleInstances_.push_back(i);
    }

    glm::vec3 camPos = camera.getPosition();
    // Portal culling seeds from both the camera and the character. Either one
    // alone has a way to be wrong - a third-person camera can end up inside a
    // wall or a broom cupboard, and a character can sit in a loose interior AABB
    // while visually outside - and being wrong here does not mean drawing a
    // little too much, it means the building vanishes. Seeding from both is a
    // superset of either, so it can only ever add groups.
    const glm::vec3 portalViewerPos = viewerPos ? *viewerPos : camPos;
    // Indoors the portal walk is an optimisation and stays behind WOWEE_WMO_CULL.
    // From outside it decides what exists: rooms are drawn only through their
    // doorways, so it runs whether or not culling was asked for.
    bool doPortalCull = portalCulling && cullingEnabled_;
    bool doDistanceCull = distanceCulling && cullingEnabled_;

    auto cullInstance = [&](size_t instIdx, InstanceDrawList& result) {
        // Retire the slot before anything can return. drawLists_ is reused
        // across frames, so a slot that bails out below would otherwise keep
        // last frame's model pointer - and unloading a map clears
        // loadedModels, which frees what that pointer names. The draw loop
        // only tests it against null, so a stale one is followed.
        result.model = nullptr;
        result.visibleGroups.clear();
        result.portalCulled = 0;
        result.distanceCulled = 0;
        result.instanceIndex = instIdx;

        if (instIdx >= instances.size()) return;
        const auto& instance = instances[instIdx];
        // Somewhere else entirely - see setInstanceHidden.
        if (instance.hidden) return;
        auto mdlIt = loadedModels.find(instance.modelId);
        if (mdlIt == loadedModels.end()) return;
        const ModelData& model = mdlIt->second;

        result.model = &model;   // cache so the draw-list loop doesn't redo the hash lookup

        if (sealedInterior_) {
            if (instance.id != sealedInterior_->instanceId) {
                result.model = nullptr;
                return;
            }
            result.visibleGroups = sealedInterior_->groups;
            return;
        }
        if (exteriorHiddenWmo_ && instance.id != *exteriorHiddenWmo_) {
            result.model = nullptr;
            return;
        }
        const bool skipExteriorGroups = exteriorHiddenWmo_.has_value() && instance.id == *exteriorHiddenWmo_;

        // Portal-based visibility - reuse member scratch buffer (avoid per-frame alloc)
        portalVisibleGroupSet_.clear();
        const bool usePortalCulling = portalVisibleGroups(instance, model, camPos, portalViewerPos, frustum, viewProj,
                                                          doPortalCull, portalVisibleGroupSet_);

        const std::vector<uint8_t> hiddenUnreachable =
            hiddenUnreachableGroups(instance, model, camPos, portalViewerPos, frustum);

        for (size_t gi = 0; gi < model.groups.size(); ++gi) {
            if (skipExteriorGroups && wmoGroupSeesExterior(model.groups[gi].groupFlags)) continue;
            if (!hiddenUnreachable.empty() && hiddenUnreachable[gi]) continue;
            if (usePortalCulling &&
                portalVisibleGroupSet_.find(static_cast<uint32_t>(gi)) == portalVisibleGroupSet_.end()) {
                result.portalCulled++;
                continue;
            }

            // The distance test used to run whether distanceCulling was set or
            // not - the flag only chose between two distances - so turning that
            // flag off never stopped anything past viewDistance_ disappearing.
            // Now nothing is dropped at all unless culling is asked for.
            if (cullingEnabled_ && gi < instance.worldGroupBounds.size()) {
                const auto& [gMin, gMax] = instance.worldGroupBounds[gi];

                glm::vec3 closestPoint = glm::clamp(camPos, gMin, gMax);
                float distSq = glm::dot(closestPoint - camPos, closestPoint - camPos);
                const float groupViewDistance = doDistanceCull
                    ? std::min(viewDistance_, maxGroupDistance)
                    : viewDistance_;
                if (distSq > groupViewDistance * groupViewDistance) {
                    result.distanceCulled++;
                    continue;
                }
            }

            result.visibleGroups.push_back(static_cast<uint32_t>(gi));
        }
    };

    // Resize drawLists to match (reuses previous capacity)
    drawLists_.resize(visibleInstances_.size());

    // Sequential culling (parallel dispatch overhead > savings for typical instance counts)
    for (size_t j = 0; j < visibleInstances_.size(); ++j) {
        cullInstance(visibleInstances_[j], drawLists_[j]);
    }

    // ── Phase 2: Vulkan draw ────────────────────────────────
    // Select pipeline based on wireframe mode
    VkPipeline activePipeline = (wireframeMode && wireframePipeline_) ? wireframePipeline_ : opaquePipeline_;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, activePipeline);

    // Bind per-frame descriptor set (set 0)
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_,
                             0, 1, &perFrameSet, 0, nullptr);

    // Track which pipeline is currently bound: bit 0 transparent, bit 1 one-sided.
    int currentPipelineKind = 0;

    for (const auto& dl : drawLists_) {
        if (dl.instanceIndex >= instances.size() || dl.model == nullptr) continue;
        const auto& instance = instances[dl.instanceIndex];
        const ModelData& model = *dl.model;


        // Push model matrix. WMO geometry is static: the cloth term stays zero.
        WMOPushConstants push{};
        push.model = instance.modelMatrix;
        push.cloth = glm::vec4(0.0f);
        vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT,
                            0, sizeof(WMOPushConstants), &push);

        // Render visible groups
        for (uint32_t gi : dl.visibleGroups) {
            const auto& group = model.groups[gi];

            // Only skip antiportal geometry
            if (group.antiportal) continue;

            // Skip groups with invalid GPU resources
            if (group.vertexBuffer == VK_NULL_HANDLE || group.indexBuffer == VK_NULL_HANDLE) continue;

            // Bind vertex + index buffers
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &group.vertexBuffer, &offset);
            vkCmdBindIndexBuffer(cmd, group.indexBuffer, 0, VK_INDEX_TYPE_UINT16);

            // Render each merged batch
            for (const auto& mb : group.mergedBatches) {
                if (!mb.materialSet) continue;

                // Determine which pipeline this batch needs
                const int neededPipeline = (mb.isTransparent && transparentPipeline_ ? 1 : 0) |
                                           (mb.twoSided || wireframeMode || mirroredView ? 0 : 2);

                // Switch pipeline if needed (descriptor sets and push constants
                // are preserved across compatible pipeline layout switches)
                if (neededPipeline != currentPipelineKind) {
                    VkPipeline targetPipeline = activePipeline;
                    if (neededPipeline == 1) targetPipeline = transparentPipeline_;
                    if (neededPipeline == 2) targetPipeline = opaqueCulledPipeline_;
                    if (neededPipeline == 3) targetPipeline = transparentCulledPipeline_;
                    if (targetPipeline == VK_NULL_HANDLE) continue;

                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, targetPipeline);
                    currentPipelineKind = neededPipeline;
                }

                // Bind material descriptor set (set 1)
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_,
                                         1, 1, &mb.materialSet, 0, nullptr);


                // Issue draw calls for each range in this merged batch
                for (const auto& dr : mb.draws) {
                    if (dr.indexCount == 0) continue;
                    vkCmdDrawIndexed(cmd, dr.indexCount, 1, dr.firstIndex, 0, 0);
                    lastDrawCalls++;
                }
            }
        }

        lastPortalCulledGroups += dl.portalCulled;
        lastDistanceCulledGroups += dl.distanceCulled;
    }
}

bool WMORenderer::initializeShadow(VkRenderPass shadowRenderPass) {
    if (!vkCtx_ || shadowRenderPass == VK_NULL_HANDLE) return false;
    VkDevice device = vkCtx_->getDevice();

    // The set the shadow pass binds, built the same way for all four renderers.
    if (!createShadowParamsSet(device, vkCtx_->getAllocator(), sizeof(ShadowParamsUBO),
                               whiteTexture_->getImageView(),
                              whiteTexture_->getSampler(), "WMORenderer", shadowParams_)) {
        return false;
    }

    // Create shadow pipeline layout: set 1 = shadowParams_.layout, push constants = 128 bytes
    VkPushConstantRange pc{};
    // The fragment stage reads the alpha-test flags out of the same block.
    pc.stageFlags = kSharedShadowPushStages;
    pc.offset = 0;
    pc.size = sizeof(ShadowPush);  // one combined matrix and the pass flags
    shadowPipelineLayout_ = createPipelineLayout(device, {shadowParams_.layout}, {pc});
    if (!shadowPipelineLayout_) {
        core::Logger::getInstance().error("WMORenderer: failed to create shadow pipeline layout");
        return false;
    }

    // Load shadow shaders
    VkShaderModule vertShader, fragShader;
    if (!vertShader.loadFromFile(device, "assets/shaders/shadow.vert.spv")) {
        core::Logger::getInstance().error("WMORenderer: failed to load shadow vertex shader");
        return false;
    }
    if (!fragShader.loadFromFile(device, "assets/shaders/shadow.frag.spv")) {
        core::Logger::getInstance().error("WMORenderer: failed to load shadow fragment shader");
        return false;
    }

    // The shadow shader is shared with the skinned renderers, so it declares
    // bone inputs this geometry has none of; kWmoShadowVertexAttributes says
    // where they point and why.
    const VkVertexInputBindingDescription vertBind = perVertexBinding(sizeof(WMOVertex));
    const std::vector<VkVertexInputAttributeDescription> vertAttrs =
        toVkAttributes(kWmoShadowVertexAttributes);

    shadowPipeline_ = buildShadowPipeline(
        device, vkCtx_->getPipelineCache(),
        vertShader.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
        fragShader.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT),
        vertBind, vertAttrs, shadowPipelineLayout_, shadowRenderPass,
        vkCtx_->useDynamicRendering());

    vertShader.destroy();
    fragShader.destroy();

    if (!shadowPipeline_) {
        core::Logger::getInstance().error("WMORenderer: failed to create shadow pipeline");
        return false;
    }
    core::Logger::getInstance().info("WMORenderer shadow pipeline initialized");
    return true;
}

void WMORenderer::renderShadow(VkCommandBuffer cmd, const glm::mat4& lightSpaceMatrix,
                               const glm::vec3& shadowCenter, float shadowRadius) {
    if (!shadowPipeline_ || !shadowParams_.set) return;
    if (instances.empty() || loadedModels.empty()) return;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipelineLayout_,
        0, 1, &shadowParams_.set, 0, nullptr);

    // WMO shadow cull uses the ortho half-extent (shadow map coverage) rather than
    // the proximity radius so that distant buildings whose shadows reach the player
    // are still rendered into the shadow map.
    const float wmoCullRadius = std::max(shadowRadius, 180.0f);
    const float wmoCullRadiusSq = wmoCullRadius * wmoCullRadius;

    for (const auto& instance : instances) {
        // Distance cull using world bounding box - WMO origins can be far from
        // their geometry, so point-based culling misses large buildings.
        glm::vec3 closest = glm::clamp(shadowCenter, instance.worldBoundsMin, instance.worldBoundsMax);
        glm::vec3 diff = closest - shadowCenter;
        if (glm::dot(diff, diff) > wmoCullRadiusSq) continue;
        auto modelIt = loadedModels.find(instance.modelId);
        if (modelIt == loadedModels.end()) continue;
        const ModelData& model = modelIt->second;

        ShadowPush push{.lightSpaceModel = lightSpaceMatrix * instance.modelMatrix};
        vkCmdPushConstants(cmd, shadowPipelineLayout_, kSharedShadowPushStages,
                           0, sizeof(ShadowPush), &push);

        for (size_t gi = 0; gi < model.groups.size(); ++gi) {
            const auto& group = model.groups[gi];
            if (group.vertexBuffer == VK_NULL_HANDLE || group.indexBuffer == VK_NULL_HANDLE) continue;

            // Skip antiportal geometry
            if (group.antiportal) continue;

            // Per-group AABB cull against shadow frustum
            if (gi < instance.worldGroupBounds.size()) {
                const auto& [gMin, gMax] = instance.worldGroupBounds[gi];
                glm::vec3 gClosest = glm::clamp(shadowCenter, gMin, gMax);
                glm::vec3 gDiff = gClosest - shadowCenter;
                if (glm::dot(gDiff, gDiff) > wmoCullRadiusSq) continue;
            }

            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &group.vertexBuffer, &offset);
            vkCmdBindIndexBuffer(cmd, group.indexBuffer, 0, VK_INDEX_TYPE_UINT16);

            for (const auto& mb : group.mergedBatches) {
                for (const auto& dr : mb.draws) {
                    vkCmdDrawIndexed(cmd, dr.indexCount, 1, dr.firstIndex, 0, 0);
                }
            }
        }
    }
}

uint32_t WMORenderer::getTotalTriangleCount() const {
    uint32_t total = 0;
    for (const auto& instance : instances) {
        auto modelIt = loadedModels.find(instance.modelId);
        if (modelIt != loadedModels.end()) {
            total += modelIt->second.getTotalTriangles();
        }
    }
    return total;
}

bool WMORenderer::createGroupResources(const pipeline::WMOGroup& group, GroupResources& resources,
                                       uint32_t groupFlags, const glm::vec3& wmoAmbientColor) {
    if (group.vertices.empty() || group.indices.empty()) {
        return false;
    }

    resources.groupId = group.groupId;
    resources.authoredMin = group.boundingBoxMin;
    resources.authoredMax = group.boundingBoxMax;
    resources.groupFlags = groupFlags;
    std::copy_n(group.fogIndices, 4, resources.fogIndices.begin());

    resources.vertexCount = group.vertices.size();
    resources.indexCount = group.indices.size();
    resources.boundingBoxMin = group.boundingBoxMin;
    resources.boundingBoxMax = group.boundingBoxMax;
    {
        glm::vec3 geomMin(std::numeric_limits<float>::max());
        glm::vec3 geomMax(std::numeric_limits<float>::lowest());
        for (const auto& v : group.vertices) {
            geomMin = glm::min(geomMin, v.position);
            geomMax = glm::max(geomMax, v.position);
        }
        resources.geometryMin = geomMin;
        resources.geometryMax = geomMax;
    }

    std::vector<WMOVertex> vertices;
    vertices.reserve(group.vertices.size());

    for (const auto& v : group.vertices) {
        WMOVertex vd;
        vd.position = v.position;
        vd.normal = v.normal;
        vd.texCoord = v.texCoord;
        vd.color = v.color;
        vd.tangent = glm::vec4(0.0f);
        vertices.push_back(vd);
    }

    // Compute tangents using Lengyel's method
    {
        std::vector<glm::vec3> tan1(vertices.size(), glm::vec3(0.0f));
        std::vector<glm::vec3> tan2(vertices.size(), glm::vec3(0.0f));

        const auto& indices = group.indices;
        for (size_t i = 0; i + 2 < indices.size(); i += 3) {
            uint16_t i0 = indices[i], i1 = indices[i + 1], i2 = indices[i + 2];
            if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) continue;

            const glm::vec3& p0 = vertices[i0].position;
            const glm::vec3& p1 = vertices[i1].position;
            const glm::vec3& p2 = vertices[i2].position;
            const glm::vec2& uv0 = vertices[i0].texCoord;
            const glm::vec2& uv1 = vertices[i1].texCoord;
            const glm::vec2& uv2 = vertices[i2].texCoord;

            glm::vec3 dp1 = p1 - p0;
            glm::vec3 dp2 = p2 - p0;
            glm::vec2 duv1 = uv1 - uv0;
            glm::vec2 duv2 = uv2 - uv0;

            float det = duv1.x * duv2.y - duv1.y * duv2.x;
            if (std::abs(det) < 1e-8f) continue;  // degenerate UVs
            float r = 1.0f / det;

            glm::vec3 sdir = (dp1 * duv2.y - dp2 * duv1.y) * r;
            glm::vec3 tdir = (dp2 * duv1.x - dp1 * duv2.x) * r;

            tan1[i0] += sdir; tan1[i1] += sdir; tan1[i2] += sdir;
            tan2[i0] += tdir; tan2[i1] += tdir; tan2[i2] += tdir;
        }

        for (size_t i = 0; i < vertices.size(); i++) {
            // Vertex normals from corrupt WMO data could be zero-length or
            // NaN. glm::normalize on either returns NaN that contaminates
            // the entire Gram-Schmidt tangent below; fall back to up-axis.
            glm::vec3 n;
            float normLen = glm::length(vertices[i].normal);
            if (std::isfinite(normLen) && normLen > 1e-6f) {
                n = vertices[i].normal / normLen;
            } else {
                n = glm::vec3(0, 0, 1);
            }
            glm::vec3 t = tan1[i];

            if (glm::dot(t, t) < 1e-8f) {
                // Fallback: generate tangent perpendicular to normal
                glm::vec3 up = (std::abs(n.y) < 0.999f) ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
                t = glm::normalize(glm::cross(n, up));
            }

            // Gram-Schmidt orthogonalize
            t = glm::normalize(t - n * glm::dot(n, t));
            float w = (glm::dot(glm::cross(n, t), tan2[i]) < 0.0f) ? -1.0f : 1.0f;
            vertices[i].tangent = glm::vec4(t, w);
        }
    }

    if (vkCtx_) {
        // Upload vertex buffer to GPU
        AllocatedBuffer vertBuf = uploadBuffer(*vkCtx_, vertices.data(),
            vertices.size() * sizeof(WMOVertex),
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        resources.vertexBuffer = vertBuf.buffer;
        resources.vertexAlloc = vertBuf.allocation;

        // Upload index buffer to GPU
        AllocatedBuffer idxBuf = uploadBuffer(*vkCtx_, group.indices.data(),
            group.indices.size() * sizeof(uint16_t),
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        resources.indexBuffer = idxBuf.buffer;
        resources.indexAlloc = idxBuf.allocation;
    }

    // Store collision geometry for floor raycasting.
    // Use MOPY per-triangle flags to exclude detail/decorative geometry (flag 0x04)
    // from collision - these are things like gears, railings, etc.
    resources.collisionVertices.reserve(group.vertices.size());
    for (const auto& v : group.vertices) {
        resources.collisionVertices.push_back(v.position);
    }
    resources.lightRefs = group.lightRefs;
    // All locally-lit interior groups (including MOGP 0x200 groups with MOLR
    // lights such as Goldshire Inn's kitchen, Jasperlode Mine, and Tempest Keep)
    // store floor MOCV RGB plus FixColorVertexAlpha's exterior-blend alpha:
    // transBatch vertices keep their authored MOCV.a (0..1 transition weight),
    // intBatch vertices use 0 (their MOCV.a is wall overbright, not exterior
    // blend), and extBatch vertices use 1.
    if (pipeline::wmoUsesLocalLighting(group.flags)) {
        resources.collisionColors.reserve(group.vertices.size());
        if ((group.flags & 0x4u) != 0) {
            size_t transVertexEnd = 0;
            if (group.transBatchCount > 0) {
                transVertexEnd = (group.transBatchCount < group.batches.size())
                    ? static_cast<size_t>(group.batches[group.transBatchCount].startVertex)
                    : group.vertices.size();
            }
            const size_t extBatchStart = static_cast<size_t>(group.transBatchCount) + group.intBatchCount;
            size_t extVertexStart = group.vertices.size();
            if (group.extBatchCount > 0 && extBatchStart < group.batches.size()) {
                extVertexStart = static_cast<size_t>(group.batches[extBatchStart].startVertex);
            }
            for (size_t vi = 0; vi < group.vertices.size(); ++vi) {
                const auto& v = group.vertices[vi];
                float extBlend = 0.0f;
                if (vi < transVertexEnd) extBlend = v.color.a;
                else if (vi >= extVertexStart) extBlend = 1.0f;
                resources.collisionColors.emplace_back(v.color.r, v.color.g, v.color.b, extBlend);
            }
        } else {
            resources.collisionColors.assign(group.vertices.size(), glm::vec4(wmoAmbientColor, 0.0f));
        }
    }
    resources.triMaterials = group.triMaterials;
    // MOGP 0x80 (unreachable): a group no one stands in - Black Temple's
    // Den of Mortal Delights, haremstair, sewersection01 and Chamber of
    // Command copies (groups 39, 40, 46, 48), drawn from outside. 8606 has
    // no collision with them: teleported onto group 46's floor at the Den
    // stairs (810.6, 466, 195.2) native fell to hall11's floor at 192.8,
    // where WoWee stood on it. The CMaNGOS vmap extractor drops them too
    // (WMOGroup::ShouldSkip). They keep their render geometry and box; no
    // triangle of theirs is offered to any query, and no containment test
    // places a point in them.
    resources.unreachable = (groupFlags & kWMOGroupUnreachable) != 0;
    resources.antiportal = wmoGroupIsAntiportal(groupFlags, group.name);
    if (resources.unreachable || resources.antiportal) {
        // Vertices stay for the box; without indices nothing can be hit.
    } else if (!group.triFlags.empty()) {
        // Store all triangles but tag each with MOPY flags for collision filtering
        resources.collisionIndices = group.indices;
        size_t numTris = group.indices.size() / 3;
        resources.triMopyFlags.resize(numTris, 0);
        for (size_t t = 0; t < numTris; t++) {
            resources.triMopyFlags[t] = (t < group.triFlags.size()) ? group.triFlags[t] : 0;
        }
    } else {
        resources.collisionIndices = group.indices;
    }

    // Compute actual bounding box from vertices (WMO header bboxes can be unreliable)
    if (!resources.collisionVertices.empty()) {
        resources.boundingBoxMin = resources.collisionVertices[0];
        resources.boundingBoxMax = resources.collisionVertices[0];
        for (const auto& v : resources.collisionVertices) {
            resources.boundingBoxMin = glm::min(resources.boundingBoxMin, v);
            resources.boundingBoxMax = glm::max(resources.boundingBoxMax, v);
        }
    }

    // Build 2D spatial grid for fast collision triangle lookup
    resources.buildCollisionGrid();

    if (!resources.unreachable && !resources.antiportal &&
        resources.collisionIndices.empty()) {
        LOG_WARNING("WMO group offers no triangles: verts=",
                    resources.collisionVertices.size(),
                    " name='", group.name, "'");
    }

    // Create batches
    if (!group.batches.empty()) {
        for (const auto& batch : group.batches) {
            GroupResources::Batch resBatch;
            resBatch.startIndex = batch.startIndex;
            resBatch.indexCount = batch.indexCount;
            resBatch.materialId = batch.materialId;
            resources.batches.push_back(resBatch);
        }
    } else {
        // No batches defined - render entire group as one batch
        GroupResources::Batch batch;
        batch.startIndex = 0;
        batch.indexCount = resources.indexCount;
        batch.materialId = 0;
        resources.batches.push_back(batch);
    }

    return true;
}

// renderGroup removed - draw calls are inlined in render()

void WMORenderer::destroyGroupGPU(GroupResources& group, bool defer) {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();
    VmaAllocator allocator = vkCtx_->getAllocator();

    if (!defer) {
        // Immediate destruction (safe after vkDeviceWaitIdle)
        destroy(allocator, group.vertexBuffer, group.vertexAlloc);
        destroy(allocator, group.indexBuffer, group.indexAlloc);
        for (auto& mb : group.mergedBatches) {
            if (mb.materialSet) {
                vkFreeDescriptorSets(device, materialDescPool_, 1, &mb.materialSet);
                mb.materialSet = VK_NULL_HANDLE;
            }
            destroy(allocator, mb.materialUBO, mb.materialUBOAlloc);
        }
    } else {
        // Deferred destruction - previous frame's command buffer may still
        // reference these buffers and descriptor sets.
        ::VkBuffer vb = group.vertexBuffer;
        VmaAllocation vbAlloc = group.vertexAlloc;
        ::VkBuffer ib = group.indexBuffer;
        VmaAllocation ibAlloc = group.indexAlloc;
        group.vertexBuffer = VK_NULL_HANDLE;
        group.indexBuffer = VK_NULL_HANDLE;

        // Snapshot material handles (::VkBuffer = raw Vulkan handle, not RAII wrapper)
        struct MatSnapshot { VkDescriptorSet set; ::VkBuffer ubo; VmaAllocation uboAlloc; };
        std::vector<MatSnapshot> mats;
        mats.reserve(group.mergedBatches.size());
        for (auto& mb : group.mergedBatches) {
            mats.push_back({.set = mb.materialSet, .ubo = mb.materialUBO, .uboAlloc = mb.materialUBOAlloc});
            mb.materialSet = VK_NULL_HANDLE;
            mb.materialUBO = VK_NULL_HANDLE;
        }

        VkDescriptorPool pool = materialDescPool_;
        vkCtx_->deferAfterAllFrameFences([device, allocator, pool, vb, vbAlloc, ib, ibAlloc,
                                      mats = std::move(mats)]() {
            if (vb) vmaDestroyBuffer(allocator, vb, vbAlloc);
            if (ib) vmaDestroyBuffer(allocator, ib, ibAlloc);
            for (auto& m : mats) {
                if (m.set) {
                    VkDescriptorSet s = m.set;
                    vkFreeDescriptorSets(device, pool, 1, &s);
                }
                if (m.ubo) vmaDestroyBuffer(allocator, m.ubo, m.uboAlloc);
            }
        });
    }
}

VkDescriptorSet WMORenderer::allocateMaterialSet() {
    if (!materialDescPool_ || !materialSetLayout_) return VK_NULL_HANDLE;

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = materialDescPool_;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &materialSetLayout_;

    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(vkCtx_->getDevice(), &allocInfo, &set) != VK_SUCCESS) {
        core::Logger::getInstance().warning("WMORenderer: failed to allocate material descriptor set");
        return VK_NULL_HANDLE;
    }
    return set;
}

bool WMORenderer::isGroupVisible(const GroupResources& group, const glm::mat4& modelMatrix,
                                 const Camera& camera) const {
    // Proper frustum-AABB intersection test for accurate visibility culling
    // Transform bounding box min/max to world space
    glm::vec3 localMin = group.boundingBoxMin;
    glm::vec3 localMax = group.boundingBoxMax;

    // Transform min and max to world space
    glm::vec4 worldMinH = modelMatrix * glm::vec4(localMin, 1.0f);
    glm::vec4 worldMaxH = modelMatrix * glm::vec4(localMax, 1.0f);
    glm::vec3 worldMin = glm::vec3(worldMinH);
    glm::vec3 worldMax = glm::vec3(worldMaxH);

    // Ensure min/max are correct after transformation (handles non-uniform scaling)
    glm::vec3 boundsMin = glm::min(worldMin, worldMax);
    glm::vec3 boundsMax = glm::max(worldMin, worldMax);

    // Extract frustum planes from view-projection matrix
    Frustum frustum;
    frustum.extractFromMatrix(camera.getViewProjectionMatrix());

    // Test if AABB intersects view frustum
    return frustum.intersectsAABB(boundsMin, boundsMax);
}

int WMORenderer::findContainingGroup(const ModelData& model, const glm::vec3& localPos) const {
    // Find which group's bounding box contains the position
    // Prefer interior groups (smaller volume) when multiple match
    int bestGroup = -1;
    float bestVolume = std::numeric_limits<float>::max();

    for (size_t gi = 0; gi < model.groups.size(); gi++) {
        const auto& group = model.groups[gi];
        if (group.unreachable || group.antiportal) continue;
        if (localPos.x >= group.geometryMin.x && localPos.x <= group.geometryMax.x &&
            localPos.y >= group.geometryMin.y && localPos.y <= group.geometryMax.y &&
            localPos.z >= group.geometryMin.z && localPos.z <= group.geometryMax.z) {
            glm::vec3 size = group.geometryMax - group.geometryMin;
            float volume = size.x * size.y * size.z;
            if (volume < bestVolume) {
                bestVolume = volume;
                bestGroup = static_cast<int>(gi);
            }
        }
    }
    return bestGroup;
}

bool WMORenderer::isPortalVisible(const ModelData& model, uint16_t portalIndex,
                                   [[maybe_unused]] const glm::vec3& cameraLocalPos,
                                   const Frustum& frustum,
                                   const glm::mat4& modelMatrix) const {
    if (portalIndex >= model.portals.size()) return false;

    const auto& portal = model.portals[portalIndex];
    if (portal.vertexCount < 3) return false;
    if (portal.startVertex + portal.vertexCount > model.portalVertices.size()) return false;

    // Get portal polygon center and bounds for frustum test
    glm::vec3 center(0.0f);
    glm::vec3 pMin = model.portalVertices[portal.startVertex];
    glm::vec3 pMax = pMin;
    for (uint16_t i = 0; i < portal.vertexCount; i++) {
        const auto& v = model.portalVertices[portal.startVertex + i];
        center += v;
        pMin = glm::min(pMin, v);
        pMax = glm::max(pMax, v);
    }
    center /= static_cast<float>(portal.vertexCount);

    // Transform all 8 corners to world space to build the correct world AABB.
    // Direct transform of pMin/pMax is wrong for rotated WMOs - the matrix can
    // swap or negate components, inverting min/max and causing frustum test failures.
    const glm::vec3 corners[8] = {
        {pMin.x, pMin.y, pMin.z}, {pMax.x, pMin.y, pMin.z},
        {pMin.x, pMax.y, pMin.z}, {pMax.x, pMax.y, pMin.z},
        {pMin.x, pMin.y, pMax.z}, {pMax.x, pMin.y, pMax.z},
        {pMin.x, pMax.y, pMax.z}, {pMax.x, pMax.y, pMax.z},
    };
    glm::vec3 worldMin( std::numeric_limits<float>::max());
    glm::vec3 worldMax(-std::numeric_limits<float>::max());
    for (const auto& c : corners) {
        glm::vec3 wc = glm::vec3(modelMatrix * glm::vec4(c, 1.0f));
        worldMin = glm::min(worldMin, wc);
        worldMax = glm::max(worldMax, wc);
    }

    // Check if portal AABB intersects frustum (more robust than point test)
    return frustum.intersectsAABB(worldMin, worldMax);
}

bool WMORenderer::portalVisibleGroups(const WMOInstance& instance, const ModelData& model,
                                      const glm::vec3& camPos, const glm::vec3& viewerPos,
                                      const Frustum& frustum, const glm::mat4& viewProj, bool cullIndoors,
                                      std::unordered_set<uint32_t>& out) const {
    if (!portalCulling || model.portals.empty() || model.portalRefs.empty()) return false;
    const glm::vec3 localRealCam = glm::vec3(instance.invModelMatrix * glm::vec4(camPos, 1.0f));
    const glm::vec3 localViewer = glm::vec3(instance.invModelMatrix * glm::vec4(viewerPos, 1.0f));
    // Indoors only when the camera's best-fit group is interior-only and
    // no exterior group's box holds it too: interior boxes spill past
    // their doorways, so a camera in both is standing outside.
    bool cameraIndoors = false;
    const int camGroup = findContainingGroup(model, localRealCam);
    if (camGroup >= 0) {
        constexpr uint32_t WMO_GROUP_FLAG_OUTDOOR = 0x8;
        constexpr uint32_t WMO_GROUP_FLAG_INDOOR = 0x2000;
        const uint32_t gFlags = model.groups[camGroup].groupFlags;
        cameraIndoors = (gFlags & WMO_GROUP_FLAG_INDOOR) != 0 &&
                        (gFlags & WMO_GROUP_FLAG_OUTDOOR) == 0;
        for (size_t gi = 0; cameraIndoors && gi < model.groups.size(); ++gi) {
            if (static_cast<int>(gi) == camGroup) continue;
            const auto& g = model.groups[gi];
            if (!(g.groupFlags & WMO_GROUP_FLAG_OUTDOOR) || g.unreachable || g.antiportal) continue;
            if (glm::all(glm::greaterThanEqual(localRealCam, g.boundingBoxMin)) &&
                glm::all(glm::lessThanEqual(localRealCam, g.boundingBoxMax))) {
                cameraIndoors = false;
            }
        }
    }
    if (cameraIndoors) {
        if (!cullIndoors) return false;
        // Both viewpoints seed the walk. The frustum belongs to the camera,
        // so its group is the principled start - but at a doorway or in a
        // hallway the camera and the character stand in different groups,
        // and seeding from one while testing doors against the other's view
        // is how the interior emptied out. Neither is reliably right on its
        // own, so take both.
        //
        // Ironforge shows this worst because of what rescues everywhere
        // else: exterior groups are seeded unconditionally below, which in a
        // city WMO means the streets and facades stay drawn however the walk
        // goes. Ironforge is interior throughout, that seed contributes
        // almost nothing, and the entire result rests on the traversal.
        getVisibleGroupsViaPortals(model, localRealCam, localViewer, frustum, instance.modelMatrix, out);
    } else {
        getVisibleGroupsFromOutside(model, localRealCam, localViewer, frustum, viewProj, instance.modelMatrix, out);
    }
    return true;
}

std::vector<uint8_t> WMORenderer::hiddenUnreachableGroups(const WMOInstance& instance, const ModelData& model,
                                                          const glm::vec3& camPos, const glm::vec3& viewerPos,
                                                          const Frustum& frustum) const {
    // Unreachable groups (MOGP 0x80) have no portals of their own; other
    // groups' portals look into them. With the camera inside one of the
    // building's reachable group boxes, one is drawn only through an in-view
    // portal of the camera's or the character's group that leads to it, and
    // the doodads it lists only with it. Black Temple: group 2 (Illidan's
    // summit) looks into 48, the courtyard group 8 into 39, hall11 (4) and
    // others into 40; nothing leads into 46. Drawing them from everywhere put
    // 48's smooth copy of the Grand Promenade steps over the real stairs
    // (782, 174) and 46's floor and crystal over the Den entrance (810.6,
    // 466); drawing none lost the spires seen from the summit (745, 330).
    if (std::none_of(model.groups.begin(), model.groups.end(),
                     [](const auto& group) { return group.unreachable; })) {
        return {};
    }
    const glm::vec3 localCam(instance.invModelMatrix * glm::vec4(camPos, 1.0f));
    const bool cameraInBuilding = std::any_of(model.groups.begin(), model.groups.end(),
        [&](const auto& group) {
            return !group.unreachable && !group.antiportal &&
                   glm::all(glm::greaterThanEqual(localCam, group.boundingBoxMin)) &&
                   glm::all(glm::lessThanEqual(localCam, group.boundingBoxMax));
        });
    if (!cameraInBuilding) return {};
    std::vector<uint8_t> hidden(model.groups.size(), 0);
    for (size_t gi = 0; gi < model.groups.size(); ++gi) hidden[gi] = model.groups[gi].unreachable ? 1 : 0;
    const glm::vec3 localViewer(instance.invModelMatrix * glm::vec4(viewerPos, 1.0f));
    for (const int seedGroup : {findContainingGroup(model, localCam), findContainingGroup(model, localViewer)}) {
        if (seedGroup < 0 || static_cast<size_t>(seedGroup) >= model.groupPortalRefs.size()) continue;
        const auto [start, count] = model.groupPortalRefs[seedGroup];
        for (uint32_t r = start; r < static_cast<uint32_t>(start) + count && r < model.portalRefs.size(); ++r) {
            const auto& ref = model.portalRefs[r];
            if (ref.groupIndex < hidden.size() && hidden[ref.groupIndex] &&
                isPortalVisible(model, ref.portalIndex, localCam, frustum, instance.modelMatrix)) {
                hidden[ref.groupIndex] = 0;
            }
        }
    }
    return hidden;
}

void WMORenderer::updateRoomVisibility(const Camera& camera, const glm::vec3* viewerPos) {
    Frustum frustum;
    const glm::mat4 viewProj = camera.getProjectionMatrix() * camera.getViewMatrix();
    frustum.extractFromMatrix(viewProj);
    const glm::vec3 camPos = camera.getPosition();
    const glm::vec3 viewer = viewerPos ? *viewerPos : camPos;
    const bool cullIndoors = portalCulling && cullingEnabled_;
    std::unordered_set<uint32_t> drawn;
    for (auto& instance : instances) {
        instance.drawnGroups.clear();
        const auto mdlIt = loadedModels.find(instance.modelId);
        if (mdlIt == loadedModels.end()) continue;
        if (exteriorHiddenWmo_ && instance.id != *exteriorHiddenWmo_) {
            instance.drawnGroups.assign(mdlIt->second.groups.size(), 0);
            continue;
        }
        const bool skipExteriorGroups = exteriorHiddenWmo_.has_value() && instance.id == *exteriorHiddenWmo_;
        drawn.clear();
        const auto hidden = hiddenUnreachableGroups(instance, mdlIt->second, camPos, viewer, frustum);
        if (!portalVisibleGroups(instance, mdlIt->second, camPos, viewer, frustum, viewProj, cullIndoors, drawn)) {
            if (hidden.empty() && !skipExteriorGroups) continue;
            for (size_t g = 0; g < mdlIt->second.groups.size(); ++g) drawn.insert(static_cast<uint32_t>(g));
        }
        instance.drawnGroups.assign(mdlIt->second.groups.size(), 0);
        for (uint32_t g : drawn) {
            if (skipExteriorGroups && wmoGroupSeesExterior(mdlIt->second.groups[g].groupFlags)) continue;
            instance.drawnGroups[g] = hidden.empty() || !hidden[g];
        }
    }
    // The units' walk always runs indoors too: The Eye's entrance hall shows
    // no Bloodwarder pack through its walls on native, 60 yards on in the
    // next hall, while WoWee drew them over the wall.
    for (auto& instance : instances) {
        instance.unitVisibleGroups.clear();
        if (!instance.drawnGroups.empty()) {  // the same walk already ran
            instance.unitVisibleGroups = instance.drawnGroups;
            continue;
        }
        const auto mdlIt = loadedModels.find(instance.modelId);
        if (mdlIt == loadedModels.end()) continue;
        drawn.clear();
        if (!portalVisibleGroups(instance, mdlIt->second, camPos, viewer, frustum, viewProj, true, drawn)) continue;
        instance.unitVisibleGroups.assign(mdlIt->second.groups.size(), 0);
        for (uint32_t g : drawn) instance.unitVisibleGroups[g] = 1;
    }
}

bool WMORenderer::anyGroupDrawn(uint32_t instanceId, const std::vector<uint16_t>& groups) const {
    const auto it = instanceIndexById.find(instanceId);
    if (it == instanceIndexById.end()) {
        // A building and its doodads can belong to different tiles (doodads go
        // with the tile that prepared them first), so one can unload first.
        // There are no rooms left to ask; the doodad shows as before.
        static std::mutex saidMutex;
        static std::unordered_set<uint32_t> said;
        std::lock_guard<std::mutex> lock(saidMutex);
        if (said.insert(instanceId).second)
            LOG_WARNING("WMO doodad rooms: building instance ", instanceId, " is gone; doodads drawn unconditionally");
        return true;
    }
    const auto& drawnGroups = instances[it->second].drawnGroups;
    if (drawnGroups.empty()) return true;
    for (uint16_t g : groups) {
        assert(g < drawnGroups.size());
        if (drawnGroups[g]) return true;
    }
    return false;
}

std::vector<uint16_t> WMORenderer::interiorRoomsOf(const ModelData& model, const glm::mat4& invModelMatrix,
                                                   const glm::vec3& point) const {
    constexpr uint32_t WMO_GROUP_FLAG_OUTDOOR = 0x8;
    constexpr uint32_t WMO_GROUP_FLAG_INDOOR = 0x2000;
    const auto interiorOnly = [](const GroupResources& group) {
        return (group.groupFlags & WMO_GROUP_FLAG_INDOOR) && !(group.groupFlags & WMO_GROUP_FLAG_OUTDOOR);
    };
    const auto holds = [](const GroupResources& group, const glm::vec3& local) {
        return glm::all(glm::greaterThanEqual(local, group.geometryMin)) &&
               glm::all(glm::lessThanEqual(local, group.geometryMax));
    };
    const glm::vec3 local(invModelMatrix * glm::vec4(point, 1.0f));
    const glm::vec3 localDown = glm::normalize(glm::vec3(invModelMatrix * glm::vec4(0, 0, -1, 0)));
    const int room = groupBeneath(model, local, localDown);
    if (room >= 0) {
        const auto& group = model.groups[static_cast<size_t>(room)];
        if (!interiorOnly(group) || !holds(group, local)) return {};
        return {static_cast<uint16_t>(room)};
    }
    // No WMO surface below: it stands on the terrain. Orgrimmar's valleys are
    // ADT ground inside the boxes of its interior groups; the Valley of
    // Wisdom bonfire (177025, in group 134's box only) and the Godan's
    // Runeworks sign (173022, group 12's box only) are not drawn by 2.4.3 from
    // above the city, where none of those groups is. Such a point belongs to
    // every interior-only group whose box holds it; one exterior group's box
    // holding it leaves it outdoors.
    std::vector<uint16_t> rooms;
    for (size_t gi = 0; gi < model.groups.size(); ++gi) {
        const auto& group = model.groups[gi];
        if (!holds(group, local)) continue;
        if (!interiorOnly(group)) return {};
        rooms.push_back(static_cast<uint16_t>(gi));
    }
    return rooms;
}

std::optional<std::pair<uint32_t, std::vector<uint16_t>>> WMORenderer::interiorRoomAt(const glm::vec3& position) const {
    // From a little above the feet, as interiorObjectLight: the floor stood
    // on is found and a ceiling just overhead is not.
    const glm::vec3 point = position + glm::vec3(0.0f, 0.0f, 0.5f);
    gatherCandidates(point - glm::vec3(0.5f), point + glm::vec3(0.5f), tl_candidateScratch);
    for (size_t index : tl_candidateScratch) {
        const auto& instance = instances[index];
        const auto modelIt = loadedModels.find(instance.modelId);
        if (modelIt == loadedModels.end()) continue;
        auto rooms = interiorRoomsOf(modelIt->second, instance.invModelMatrix, point);
        if (!rooms.empty()) return std::make_pair(instance.id, std::move(rooms));
    }
    return std::nullopt;
}

bool WMORenderer::roomHides(const glm::vec3& point) {
    gatherCandidates(point - glm::vec3(0.5f), point + glm::vec3(0.5f), tl_candidateScratch);
    for (size_t index : tl_candidateScratch) {
        const auto& instance = instances[index];
        if (instance.unitVisibleGroups.empty()) continue;  // no portal walk this frame
        const auto modelIt = loadedModels.find(instance.modelId);
        if (modelIt == loadedModels.end()) continue;
        const auto rooms = interiorRoomsOf(modelIt->second, instance.invModelMatrix, point);
        if (rooms.empty()) continue;
        bool seen = false;
        for (uint16_t room : rooms) {
            assert(room < instance.unitVisibleGroups.size());
            seen = seen || instance.unitVisibleGroups[room];
        }
        if (!seen) return true;
    }
    return false;
}

void WMORenderer::getVisibleGroupsFromOutside(const ModelData& model,
                                               const glm::vec3& cameraLocalPos,
                                               const glm::vec3& viewerLocalPos,
                                               const Frustum& frustum,
                                               const glm::mat4& viewProj,
                                               const glm::mat4& modelMatrix,
                                               std::unordered_set<uint32_t>& outVisibleGroups) const {
    constexpr uint32_t WMO_GROUP_FLAG_OUTDOOR = 0x8;
    constexpr uint32_t WMO_GROUP_FLAG_INDOOR = 0x2000;
    // Screen rectangle (NDC x0,y0,x1,y1) each reached group is seen through.
    using Rect = glm::vec4;
    const Rect fullScreen(-1.0f, -1.0f, 1.0f, 1.0f);
    std::vector<bool> visited(model.groups.size(), false);
    std::vector<Rect> seenThrough(model.groups.size(), fullScreen);
    std::vector<uint32_t> queue;
    queue.reserve(model.groups.size());
    auto seed = [&](size_t gi, const Rect& rect) {
        if (visited[gi]) return;
        visited[gi] = true;
        seenThrough[gi] = rect;
        queue.push_back(static_cast<uint32_t>(gi));
        outVisibleGroups.insert(static_cast<uint32_t>(gi));
    };
    // The portal polygon's screen rectangle, or the full screen when any of
    // its corners lies behind the camera (no rectangle to clip by).
    const glm::mat4 localToClip = viewProj * modelMatrix;
    auto portalRect = [&](const PortalData& portal) {
        Rect r(1.0f, 1.0f, -1.0f, -1.0f);
        for (uint16_t i = 0; i < portal.vertexCount; ++i) {
            const glm::vec4 clip = localToClip * glm::vec4(model.portalVertices[portal.startVertex + i], 1.0f);
            if (clip.w <= 1e-3f) return fullScreen;
            const glm::vec2 ndc = glm::vec2(clip) / clip.w;
            r.x = std::min(r.x, ndc.x); r.y = std::min(r.y, ndc.y);
            r.z = std::max(r.z, ndc.x); r.w = std::max(r.w, ndc.y);
        }
        return r;
    };
    auto contains = [](const auto& g, const glm::vec3& p) {
        return glm::all(glm::greaterThanEqual(p, g.boundingBoxMin)) &&
               glm::all(glm::lessThanEqual(p, g.boundingBoxMax));
    };
    // The room a point stands in is the one whose surface is first below it,
    // among rooms whose box holds it. A box alone is not enough: Sunwell
    // Plateau's Witch's Sanctum (sunwell.wmo group 4) reaches 4 yards above
    // the M'uru shrine floor (group 44) laid over its ceiling, so standing at
    // M'uru seeded the sanctum and drew its chandelier (MODD 936,
    // BE_Chandelier01 at scale 5.09) up through the shrine floor as a gold
    // dish; native shows the bare glowing floor there.
    const glm::vec3 localDown = glm::normalize(glm::vec3(glm::inverse(modelMatrix) * glm::vec4(0, 0, -1, 0)));
    const int cameraRoom = groupBeneath(model, cameraLocalPos, localDown);
    const int viewerRoom = groupBeneath(model, viewerLocalPos, localDown);
    for (size_t gi = 0; gi < model.groups.size(); ++gi) {
        const auto& g = model.groups[gi];
        const bool interiorOnly = (g.groupFlags & WMO_GROUP_FLAG_INDOOR) && !(g.groupFlags & WMO_GROUP_FLAG_OUTDOOR);
        const bool occupied = (contains(g, cameraLocalPos) && cameraRoom == static_cast<int>(gi)) ||
                              (contains(g, viewerLocalPos) && viewerRoom == static_cast<int>(gi));
        if (!interiorOnly || occupied) seed(gi, fullScreen);
    }
    // Rooms are entered only through a portal that is in view and whose
    // front - the owning group's MOPR side - faces the camera. Without this an
    // outside camera drew every room of a building, including GM Island's
    // guildhousea.wmo ground-floor rooms behind the lodge, which the original
    // shows as open courtyard.
    for (size_t qi = 0; qi < queue.size(); ++qi) {
        const uint32_t current = queue[qi];
        if (current >= model.groupPortalRefs.size()) continue;
        const auto [portalStart, portalCount] = model.groupPortalRefs[current];
        for (uint16_t pi = 0; pi < portalCount; ++pi) {
            const uint32_t refIdx = portalStart + pi;
            assert(refIdx < model.portalRefs.size());
            const auto& ref = model.portalRefs[refIdx];
            if (ref.groupIndex >= model.groups.size() || visited[ref.groupIndex]) continue;
            assert(ref.portalIndex < model.portals.size());
            const auto& portal = model.portals[ref.portalIndex];
            const float side = glm::dot(portal.normal, cameraLocalPos) + portal.distance;
            if (side * static_cast<float>(ref.side) <= 0.0f) continue;
            if (!isPortalVisible(model, ref.portalIndex, cameraLocalPos, frustum, modelMatrix)) continue;
            // Seen only through every portal on the way: a room two doors in
            // must show through the first door too. Without this, rooms deep
            // inside Karazhan's tower, whose doors face the Master's Terrace
            // through the tower walls, hung lit in the sky above it.
            const Rect through = portalRect(portal);
            const Rect& parent = seenThrough[current];
            const Rect clipped(std::max(through.x, parent.x), std::max(through.y, parent.y),
                               std::min(through.z, parent.z), std::min(through.w, parent.w));
            if (clipped.x >= clipped.z || clipped.y >= clipped.w) continue;
            seed(ref.groupIndex, clipped);
        }
    }
}

void WMORenderer::getVisibleGroupsViaPortals(const ModelData& model,
                                              const glm::vec3& cameraLocalPos,
                                              const glm::vec3& viewerLocalPos,
                                              const Frustum& frustum,
                                              const glm::mat4& modelMatrix,
                                              std::unordered_set<uint32_t>& outVisibleGroups) const {
    constexpr uint32_t WMO_GROUP_FLAG_OUTDOOR = 0x8;
    constexpr uint32_t WMO_GROUP_FLAG_INDOOR = 0x2000;

    // Find camera's containing group
    int cameraGroup = findContainingGroup(model, cameraLocalPos);

    // If camera is outside all groups, fall back to frustum culling only
    if (cameraGroup < 0) {
        // Camera outside WMO - mark all groups as potentially visible
        // (will still be frustum culled in render)
        for (size_t gi = 0; gi < model.groups.size(); gi++) {
            outVisibleGroups.insert(static_cast<uint32_t>(gi));
        }
        return;
    }

    // Outdoor city WMOs (e.g. Stormwind) often have portal graphs that are valid for
    // indoor visibility but too aggressive outdoors, causing direction-dependent popout.
    // Only trust portal traversal when the camera is in an interior-only group.
    if (cameraGroup < static_cast<int>(model.groups.size())) {
        const uint32_t gFlags = model.groups[cameraGroup].groupFlags;
        const bool isIndoor = (gFlags & WMO_GROUP_FLAG_INDOOR) != 0;
        const bool isOutdoor = (gFlags & WMO_GROUP_FLAG_OUTDOOR) != 0;
        if (!isIndoor || isOutdoor) {
            for (size_t gi = 0; gi < model.groups.size(); gi++) {
                outVisibleGroups.insert(static_cast<uint32_t>(gi));
            }
            return;
        }
        // Best-fit group is indoor-only, but the position might also be inside an
        // outdoor group's AABB (e.g., standing on a street near a building whose
        // indoor AABB extends outward).  If any outdoor group also contains the
        // position, treat this as an outdoor location and show all groups.
        for (size_t gi = 0; gi < model.groups.size(); gi++) {
            if (static_cast<int>(gi) == cameraGroup) continue;
            const auto& g = model.groups[gi];
            if (!(g.groupFlags & WMO_GROUP_FLAG_OUTDOOR) || g.unreachable || g.antiportal) continue;
            if (cameraLocalPos.x >= g.boundingBoxMin.x && cameraLocalPos.x <= g.boundingBoxMax.x &&
                cameraLocalPos.y >= g.boundingBoxMin.y && cameraLocalPos.y <= g.boundingBoxMax.y &&
                cameraLocalPos.z >= g.boundingBoxMin.z && cameraLocalPos.z <= g.boundingBoxMax.z) {
                for (size_t gj = 0; gj < model.groups.size(); gj++) {
                    outVisibleGroups.insert(static_cast<uint32_t>(gj));
                }
                return;
            }
        }
    }

    // If the camera group has no portal refs, it's a dead-end group (utility/transition group).
    // Fall back to showing all groups to avoid the rest of the WMO going invisible.
    if (cameraGroup < static_cast<int>(model.groupPortalRefs.size())) {
        auto [portalStart, portalCount] = model.groupPortalRefs[cameraGroup];
        if (portalCount == 0) {
            for (size_t gi = 0; gi < model.groups.size(); gi++) {
                outVisibleGroups.insert(static_cast<uint32_t>(gi));
            }
            return;
        }
    }

    // Exterior groups are never portal-culled. AABB containment misclassifies
    // doorway/awning thresholds as indoors (interior boxes spill past the
    // door and outdoor boxes don't always overlap them), and a wrong BFS
    // start then hides the whole city. Portal traversal below only decides
    // interior-only groups; streets and facades always draw (distance culling
    // still bounds them).
    for (size_t gi = 0; gi < model.groups.size(); ++gi) {
        const uint32_t f = model.groups[gi].groupFlags;
        if (!(f & WMO_GROUP_FLAG_INDOOR) || (f & WMO_GROUP_FLAG_OUTDOOR))
            outVisibleGroups.insert(static_cast<uint32_t>(gi));
    }

    // BFS through portals from both viewpoints' groups plus every always-visible
    // exterior group, so interiors seen through open doors still draw even when
    // one viewpoint's containing group was misclassified.
    //
    // The character's group is seeded alongside the camera's because in a
    // doorway or a hallway the two are in different rooms. Walking from only one
    // of them, while judging every door against the camera's frustum, is what
    // emptied Ironforge. A second seed can only add groups, never remove any,
    // and drawing a room too many is the failure this should have.
    std::vector<bool> visited(model.groups.size(), false);
    std::vector<uint32_t> queue;
    queue.reserve(model.groups.size());

    auto seed = [&](int gi) {
        if (gi < 0 || gi >= static_cast<int>(model.groups.size())) return;
        if (visited[gi]) return;
        visited[gi] = true;
        queue.push_back(static_cast<uint32_t>(gi));
        outVisibleGroups.insert(static_cast<uint32_t>(gi));
    };
    seed(cameraGroup);
    seed(findContainingGroup(model, viewerLocalPos));
    // Each viewpoint also stands in the room a unit there is given
    // (roomHides): the group whose box holds it and whose surface is first
    // below it. The best-fit box need not be that room. In Dire Maul West's
    // Court of the Highborne the walkway's best-fit box is group 80, but its
    // floor at z -3.57 belongs to group 72, as do the Eldreth units standing
    // on it. Walking from 80 alone reached 72 only through a portal in view;
    // pitching the camera down took that portal off screen and the units
    // under the camera vanished, where build 8606 keeps drawing them.
    const glm::vec3 localDown = glm::normalize(glm::vec3(glm::inverse(modelMatrix) * glm::vec4(0, 0, -1, 0)));
    for (const glm::vec3& point : {cameraLocalPos, viewerLocalPos}) {
        const int room = groupBeneath(model, point, localDown);
        if (room < 0) continue;
        const auto& group = model.groups[static_cast<size_t>(room)];
        if (glm::all(glm::greaterThanEqual(point, group.boundingBoxMin)) &&
            glm::all(glm::lessThanEqual(point, group.boundingBoxMax))) {
            seed(room);
        }
    }

    // Exterior groups were inserted above without being queued; they are portals
    // into the interior too.
    for (uint32_t gi : outVisibleGroups) {
        if (!visited[gi]) {
            visited[gi] = true;
            queue.push_back(gi);
        }
    }

    size_t queueIdx = 0;
    while (queueIdx < queue.size()) {
        uint32_t currentGroup = queue[queueIdx++];

        // Get portal refs for this group
        if (currentGroup >= model.groupPortalRefs.size()) continue;
        auto [portalStart, portalCount] = model.groupPortalRefs[currentGroup];

        for (uint16_t pi = 0; pi < portalCount; pi++) {
            uint16_t refIdx = portalStart + pi;
            if (refIdx >= model.portalRefs.size()) continue;

            const auto& ref = model.portalRefs[refIdx];
            uint32_t targetGroup = ref.groupIndex;

            if (targetGroup >= model.groups.size()) continue;
            if (visited[targetGroup]) continue;

            // Check if portal is visible from camera
            if (isPortalVisible(model, ref.portalIndex, cameraLocalPos, frustum, modelMatrix)) {
                visited[targetGroup] = true;
                outVisibleGroups.insert(targetGroup);
                queue.push_back(targetGroup);
            }
        }
    }
}

void WMORenderer::WMOInstance::updateModelMatrix() {
    // The placement rotation the caller stored as (C, A, B + 180) in radians.
    // Buildings and doodads compose this identically, in placement_transform.hpp,
    // which records how the order was solved against the bounding boxes MODF
    // carries: composing it in both places is how they came to disagree, and
    // how a building on flat ground could look right while a bridge across a
    // ravine did not.
    modelMatrix = placementModelMatrix(position, rotation, scale);

    // Cache inverse for collision detection
    invModelMatrix = glm::inverse(modelMatrix);
}

pipeline::BLPImage WMORenderer::generateNormalHeightMapPixels(
        const uint8_t* pixels, uint32_t width, uint32_t height, float& outVariance) {
    pipeline::BLPImage result;
    // Two, where the character renderer asks for five: stone and wood at the
    // distance a wall is seen from, which a stronger gradient only roughens.
    std::vector<uint8_t> output = rendering::generateNormalHeightMap(
        pixels, width, height, /*strength=*/2.0f, outVariance);
    if (output.empty()) return result;
    result.width = static_cast<int>(width);
    result.height = static_cast<int>(height);
    result.channels = 4;
    result.data = std::move(output);
    return result;
}

std::unique_ptr<VkTexture> WMORenderer::generateNormalHeightMap(
        const uint8_t* pixels, uint32_t width, uint32_t height, float& outVariance) {
    if (!vkCtx_) return nullptr;
    auto normalPixels = generateNormalHeightMapPixels(pixels, width, height, outVariance);
    if (!normalPixels.isValid()) return nullptr;

    // Upload the CPU-generated pixels to the GPU with mipmaps.
    auto tex = std::make_unique<VkTexture>();
    if (!tex->upload(*vkCtx_, normalPixels.data.data(), width, height,
                     VK_FORMAT_R8G8B8A8_UNORM, true)) {
        return nullptr;
    }
    tex->createSampler(vkCtx_->getDevice(), VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                        VK_SAMPLER_ADDRESS_MODE_REPEAT);
    return tex;
}

VkTexture* WMORenderer::loadTexture(const std::string& path) {
    constexpr uint64_t kFailedTextureRetryLookups = 512;
    if (!assetManager || !vkCtx_) {
        return whiteTexture_.get();
    }

    auto normalizeKey = [](std::string key) {
        std::replace(key.begin(), key.end(), '/', '\\');
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return key;
    };
    std::string key = path;
    // Some assets contain stray bytes after a NUL in path chunks.
    size_t nul = key.find('\0');
    if (nul != std::string::npos) key.resize(nul);
    key = normalizeKey(key);
    if (key.rfind(".\\", 0) == 0) key = key.substr(2);
    while (!key.empty() && key.front() == '\\') key.erase(key.begin());
    if (key.empty()) return whiteTexture_.get();

    auto hasKnownExt = [](const std::string& p) {
        if (p.size() < 4) return false;
        std::string ext = p.substr(p.size() - 4);
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return (ext == ".blp" || ext == ".tga" || ext == ".dds");
    };
    auto toBlp = [](std::string p) {
        if (p.size() >= 4) {
            std::string ext = p.substr(p.size() - 4);
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext == ".tga" || ext == ".dds") {
                p = p.substr(0, p.size() - 4) + ".blp";
            }
        }
        return p;
    };

    std::vector<std::string> candidates;
    auto addCandidate = [&](const std::string& raw) {
        std::string c = normalizeKey(raw);
        if (c.rfind(".\\", 0) == 0) c = c.substr(2);
        while (!c.empty() && c.front() == '\\') c.erase(c.begin());
        if (!c.empty()) candidates.push_back(c);
    };

    addCandidate(toBlp(key));
    if (!hasKnownExt(key)) addCandidate(key + ".blp");

    // Common WMO references omit folder prefix; manifest often stores these under textures\...
    std::string keyWithExt = hasKnownExt(key) ? toBlp(key) : (key + ".blp");
    if (key.find('\\') == std::string::npos) {
        addCandidate(std::string("textures\\") + keyWithExt);
    }
    if (key.rfind("texture\\", 0) == 0) {
        addCandidate(std::string("textures\\") + key.substr(8));
    }

    // De-duplicate while preserving order.
    std::vector<std::string> uniqueCandidates;
    uniqueCandidates.reserve(candidates.size());
    std::unordered_set<std::string> seen;
    for (const auto& c : candidates) {
        if (seen.insert(c).second) uniqueCandidates.push_back(c);
    }

    // Cache lookup across all candidate keys
    for (const auto& c : uniqueCandidates) {
        auto it = textureCache.find(c);
        if (it != textureCache.end()) {
            it->second.lastUse = ++textureCacheCounter_;
            return it->second.texture.get();
        }
    }

    const uint64_t lookupSerial = ++textureLookupSerial_;
    std::vector<std::string> attemptedCandidates;
    attemptedCandidates.reserve(uniqueCandidates.size());
    for (const auto& c : uniqueCandidates) {
        auto fit = failedTextureRetryAt_.find(c);
        if (fit != failedTextureRetryAt_.end() && lookupSerial < fit->second) {
            continue;
        }
        attemptedCandidates.push_back(c);
    }
    if (attemptedCandidates.empty()) {
        return whiteTexture_.get();
    }

    // Try loading all candidates until one succeeds
    // Check pre-decoded BLP cache first (populated by background worker threads)
    pipeline::BLPImage blp;
    std::string resolvedKey;
    if (predecodedBLPCache_) {
        for (const auto& c : uniqueCandidates) {
            auto pit = predecodedBLPCache_->find(c);
            if (pit != predecodedBLPCache_->end()) {
                blp = std::move(pit->second);
                predecodedBLPCache_->erase(pit);
                resolvedKey = c;
                break;
            }
        }
    }
    if (!blp.isValid()) {
        for (const auto& c : attemptedCandidates) {
            // Blocks. The diffuse is only ever sampled; the normal map
            // below needs pixels and decodes them from these when it runs.
            blp = assetManager->loadTexture(c, true);
            if (blp.isValid()) {
                resolvedKey = c;
                break;
            }
        }
    }
    if (!blp.isValid()) {
        for (const auto& c : attemptedCandidates) {
            failedTextureCache_.insert(c);
            failedTextureRetryAt_[c] = lookupSerial + kFailedTextureRetryLookups;
        }
        if (loggedTextureLoadFails_.insert(key).second) {
            core::Logger::getInstance().warning("WMO: Failed to load texture: ", path);
        }
        // Do not cache failures as white. MPQ reads can fail transiently
        // during streaming/contention, and caching white here permanently
        // poisons the texture for this session.
        return whiteTexture_.get();
    }

    core::Logger::getInstance().debug("WMO texture: ", path, " size=", blp.width, "x", blp.height);

    size_t approxBytes = blp.approxUploadBytes();
    if (textureCacheBytes_ + approxBytes > textureCacheBudgetBytes_) {
        for (const auto& c : attemptedCandidates) {
            failedTextureCache_.insert(c);
            failedTextureRetryAt_[c] = lookupSerial + kFailedTextureRetryLookups;
        }
        if (textureBudgetRejectWarnings_ < 3) {
            core::Logger::getInstance().warning(
                "WMO texture cache full (", textureCacheBytes_ / (1024 * 1024),
                " MB / ", textureCacheBudgetBytes_ / (1024 * 1024),
                " MB), rejecting texture: ", path);
        }
        ++textureBudgetRejectWarnings_;
        return whiteTexture_.get();
    }

    // Create Vulkan texture
    auto texture = std::make_unique<VkTexture>();
    if (!texture->uploadBLP(*vkCtx_, blp)) {
        core::Logger::getInstance().warning("WMO: Failed to upload texture to GPU: ", path);
        return whiteTexture_.get();
    }
    texture->createSampler(vkCtx_->getDevice(), VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                            VK_SAMPLER_ADDRESS_MODE_REPEAT);

    // Prefer normal/height pixels generated by terrain workers. This preserves
    // advanced materials without running the Sobel/blur pass on the render
    // thread. Non-streamed loads retain the synchronous fallback.
    float nhVariance = 0.0f;
    std::unique_ptr<VkTexture> nhMap;
    if (normalMappingEnabled_ || pomEnabled_) {
        if (predecodedNormalMapCache_ && !resolvedKey.empty()) {
            auto normalIt = predecodedNormalMapCache_->find(resolvedKey);
            if (normalIt != predecodedNormalMapCache_->end()) {
                auto& normalPixels = normalIt->second;
                auto uploaded = std::make_unique<VkTexture>();
                if (normalPixels.isValid() &&
                    uploaded->upload(*vkCtx_, normalPixels.data.data(),
                                     static_cast<uint32_t>(normalPixels.width),
                                     static_cast<uint32_t>(normalPixels.height),
                                     VK_FORMAT_R8G8B8A8_UNORM, true)) {
                    uploaded->createSampler(vkCtx_->getDevice(), VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                                            VK_SAMPLER_ADDRESS_MODE_REPEAT);
                    nhMap = std::move(uploaded);
                    if (predecodedNormalMapVariances_) {
                        auto varianceIt = predecodedNormalMapVariances_->find(resolvedKey);
                        if (varianceIt != predecodedNormalMapVariances_->end()) {
                            nhVariance = varianceIt->second;
                            predecodedNormalMapVariances_->erase(varianceIt);
                        }
                    }
                }
                predecodedNormalMapCache_->erase(normalIt);
            }
        }
        if (!nhMap && !deferNormalMaps_) {
            // Decoded here and thrown away, rather than kept and uploaded.
            // The Sobel pass has always needed pixels; what changes is that
            // the texture itself no longer has to be RGBA8 to provide them.
            const std::vector<uint8_t> decoded =
                blp.isBlockCompressed() ? pipeline::BLPLoader::decodeBaseLevel(blp)
                                        : blp.data;
            if (!decoded.empty()) {
                nhMap = generateNormalHeightMap(decoded.data(), blp.width, blp.height,
                                                nhVariance);
            }
        }
        if (nhMap) {
            approxBytes *= 2;  // account for normal map in budget
        }
    }

    // Cache it
    TextureCacheEntry e;
    VkTexture* rawPtr = texture.get();
    e.approxBytes = approxBytes;
    e.lastUse = ++textureCacheCounter_;
    e.texture = std::move(texture);
    e.normalHeightMap = std::move(nhMap);
    e.heightMapVariance = nhVariance;
    textureCacheBytes_ += e.approxBytes;
    if (!resolvedKey.empty()) {
        textureCache[resolvedKey] = std::move(e);
        failedTextureCache_.erase(resolvedKey);
        failedTextureRetryAt_.erase(resolvedKey);
    } else {
        textureCache[key] = std::move(e);
        failedTextureCache_.erase(key);
        failedTextureRetryAt_.erase(key);
    }
    core::Logger::getInstance().debug("WMO: Loaded texture: ", path, " (", blp.width, "x", blp.height, ")");

    return rawPtr;
}

// Ray-AABB intersection (slab method)
// Returns true if the ray intersects the axis-aligned bounding box
/// Where a ray enters and leaves a box, as distances along it. False when it
/// misses. The two are what a query has to search between: for a ray that is
/// not axis-aligned in the space the box is in, the origin's XY says nothing
/// about where the ray actually crosses the box.
static bool rayAABBRange(const glm::vec3& origin, const glm::vec3& dir,
                         const glm::vec3& bmin, const glm::vec3& bmax,
                         float& outMin, float& outMax) {
    float tmin = -1e30f, tmax = 1e30f;
    for (int i = 0; i < 3; i++) {
        if (std::abs(dir[i]) < 1e-8f) {
            if (origin[i] < bmin[i] || origin[i] > bmax[i]) return false;
        } else {
            const float invD = 1.0f / dir[i];
            float t0 = (bmin[i] - origin[i]) * invD;
            float t1 = (bmax[i] - origin[i]) * invD;
            if (t0 > t1) std::swap(t0, t1);
            tmin = std::max(tmin, t0);
            tmax = std::min(tmax, t1);
            if (tmin > tmax) return false;
        }
    }
    if (tmax < 0.0f) return false;
    outMin = std::max(tmin, 0.0f);
    outMax = tmax;
    return true;
}

/// The triangles a ray could hit inside one group, fetched from its grid.
///
/// Between where the ray enters the group's box and where it leaves it, not
/// around the ray's origin. Those are the same place only while the ray is
/// vertical in the group's own space, and a placement with pitch makes it lean
/// - see the note in getFloorHeight. False when the ray misses the box.
///
/// Written out three times before this, once per query, which is two more
/// chances to look in the wrong place.
template <typename Group>
static bool trianglesAlongRay(const Group& group, const glm::vec3& localOrigin,
                              const glm::vec3& localDir, std::vector<uint32_t>& out) {
    float tEnter = 0.0f, tExit = 0.0f;
    if (!rayAABBRange(localOrigin, localDir, group.boundingBoxMin,
                      group.boundingBoxMax, tEnter, tExit)) {
        return false;
    }
    const glm::vec3 enter = localOrigin + localDir * tEnter;
    const glm::vec3 exit = localOrigin + localDir * tExit;
    group.getTrianglesInRange(std::min(enter.x, exit.x) - 1.0f,
                              std::min(enter.y, exit.y) - 1.0f,
                              std::max(enter.x, exit.x) + 1.0f,
                              std::max(enter.y, exit.y) + 1.0f, out);
    return true;
}

int WMORenderer::groupBeneath(const ModelData& model, const glm::vec3& localPos,
                              const glm::vec3& localDown) const {
    int best = -1;
    float bestDistance = std::numeric_limits<float>::max();
    bool bestBackface = true;
    for (size_t gi = 0; gi < model.groups.size(); ++gi) {
        const auto& group = model.groups[gi];
        if (!trianglesAlongRay(group, localPos, localDown, tl_triScratch)) continue;
        const auto& indices = group.collisionIndices;
        const auto& vertices = group.collisionVertices;
        for (uint32_t triangle : tl_triScratch) {
            const glm::vec3& v0 = vertices[indices[triangle]];
            const glm::vec3& v1 = vertices[indices[triangle + 1]];
            const glm::vec3& v2 = vertices[indices[triangle + 2]];
            const float distance = rayTriangleIntersect(localPos, localDown, v0, v1, v2);
            if (distance <= 0.0f) continue;
            const bool backface = isDownwardHitInteriorCeilingBackface(v0, v1, v2, localDown);
            if (!preferDownwardSurfaceHit(distance, backface, bestDistance, bestBackface)) continue;
            bestDistance = distance;
            bestBackface = backface;
            best = static_cast<int>(gi);
        }
    }
    if (best >= 0 && bestBackface && wmoGroupIsIndoorOnly(model.groups[static_cast<size_t>(best)].groupFlags)) {
        return -1;
    }
    return best;
}

static bool rayIntersectsAABB(const glm::vec3& origin, const glm::vec3& dir,
                               const glm::vec3& bmin, const glm::vec3& bmax) {
    float tmin = -1e30f, tmax = 1e30f;
    for (int i = 0; i < 3; i++) {
        if (std::abs(dir[i]) < 1e-8f) {
            // Ray is parallel to this slab - check if origin is inside
            if (origin[i] < bmin[i] || origin[i] > bmax[i]) return false;
        } else {
            float invD = 1.0f / dir[i];
            float t0 = (bmin[i] - origin[i]) * invD;
            float t1 = (bmax[i] - origin[i]) * invD;
            if (t0 > t1) std::swap(t0, t1);
            tmin = std::max(tmin, t0);
            tmax = std::min(tmax, t1);
            if (tmin > tmax) return false;
        }
    }
    return tmax >= 0.0f;  // At least part of the ray is forward
}

static void transformAABB(const glm::mat4& modelMatrix,
                          const glm::vec3& localMin,
                          const glm::vec3& localMax,
                          glm::vec3& outMin,
                          glm::vec3& outMax) {
    const glm::vec3 corners[8] = {
        {localMin.x, localMin.y, localMin.z},
        {localMin.x, localMin.y, localMax.z},
        {localMin.x, localMax.y, localMin.z},
        {localMin.x, localMax.y, localMax.z},
        {localMax.x, localMin.y, localMin.z},
        {localMax.x, localMin.y, localMax.z},
        {localMax.x, localMax.y, localMin.z},
        {localMax.x, localMax.y, localMax.z}
    };

    outMin = glm::vec3(std::numeric_limits<float>::max());
    outMax = glm::vec3(-std::numeric_limits<float>::max());
    for (const glm::vec3& corner : corners) {
        glm::vec3 world = glm::vec3(modelMatrix * glm::vec4(corner, 1.0f));
        outMin = glm::min(outMin, world);
        outMax = glm::max(outMax, world);
    }
}

// ---- Per-group 2D collision grid ----

void WMORenderer::GroupResources::buildCollisionGrid() {
    if (collisionVertices.empty() || collisionIndices.size() < 3) {
        gridCellsX = 0;
        gridCellsY = 0;
        return;
    }

    gridOrigin = glm::vec2(boundingBoxMin.x, boundingBoxMin.y);
    float extentX = boundingBoxMax.x - boundingBoxMin.x;
    float extentY = boundingBoxMax.y - boundingBoxMin.y;

    gridCellsX = std::max(1, static_cast<int>(std::ceil(extentX / COLLISION_CELL_SIZE)));
    gridCellsY = std::max(1, static_cast<int>(std::ceil(extentY / COLLISION_CELL_SIZE)));

    // Cap grid size to avoid excessive memory for huge groups
    if (gridCellsX > 64) gridCellsX = 64;
    if (gridCellsY > 64) gridCellsY = 64;

    size_t totalCells = static_cast<size_t>(gridCellsX) * static_cast<size_t>(gridCellsY);
    cellTriangles.resize(totalCells);
    cellFloorTriangles.resize(totalCells);
    cellWallTriangles.resize(totalCells);

    size_t numTriangles = collisionIndices.size() / 3;
    triBounds.resize(numTriangles);
    triNormals.resize(numTriangles);
    triVisited.resize(numTriangles, 0);

    for (size_t i = 0; i + 2 < collisionIndices.size(); i += 3) {
        const glm::vec3& v0 = collisionVertices[collisionIndices[i]];
        const glm::vec3& v1 = collisionVertices[collisionIndices[i + 1]];
        const glm::vec3& v2 = collisionVertices[collisionIndices[i + 2]];

        // Triangle XY bounding box
        float triMinX = std::min({v0.x, v1.x, v2.x});
        float triMinY = std::min({v0.y, v1.y, v2.y});
        float triMaxX = std::max({v0.x, v1.x, v2.x});
        float triMaxY = std::max({v0.y, v1.y, v2.y});

        // Per-triangle Z bounds
        float triMinZ = std::min({v0.z, v1.z, v2.z});
        float triMaxZ = std::max({v0.z, v1.z, v2.z});
        triBounds[i / 3] = { .minZ = triMinZ, .maxZ = triMaxZ };

        // Precompute and store unit normal
        glm::vec3 edge1 = v1 - v0;
        glm::vec3 edge2 = v2 - v0;
        glm::vec3 normal = glm::cross(edge1, edge2);
        float normalLen = glm::length(normal);
        if (normalLen > 0.001f) {
            normal /= normalLen;
        } else {
            normal = glm::vec3(0.0f, 0.0f, 1.0f);
        }
        triNormals[i / 3] = normal;

        // Classify floor vs wall by normal; see kWallMaxAbsNormalZ.
        float absNz = std::abs(normal.z);
        bool isFloor = (absNz >= kWallMaxAbsNormalZ);
        bool isWall = (absNz < kWallMaxAbsNormalZ);

        // Filed by the same arithmetic the queries use, so a triangle is
        // always in a cell a query covering it reaches.
        const auto cells = cellRangeCovering(gridCellsX, gridCellsY, extentX, extentY, gridOrigin,
                                             triMinX, triMinY, triMaxX, triMaxY);
        assert(cells && "a group triangle lies outside its own bounding box");

        uint32_t triIdx = static_cast<uint32_t>(i);
        for (int cy = cells->minY; cy <= cells->maxY; ++cy) {
            for (int cx = cells->minX; cx <= cells->maxX; ++cx) {
                int cellIdx = cy * gridCellsX + cx;
                cellTriangles[cellIdx].push_back(triIdx);
                if (isFloor) cellFloorTriangles[cellIdx].push_back(triIdx);
                if (isWall) cellWallTriangles[cellIdx].push_back(triIdx);
            }
        }
    }
}
/// The triangles of one cell array that a query box reaches.
///
/// Three queries walk this grid, for any triangle, for floors and for walls,
/// and they differed in one token: which of the three per-cell arrays they
/// read. Everything else, the cell rectangle, the reserve estimate, the
/// visited-bit dedup and the single-cell shortcut that skips it, was written
/// out three times.
///
/// The dedup matters because a triangle spanning several cells is filed under
/// each of them, and a caller that tests it twice counts two hits: a raycast
/// then reports an even number of crossings where there was one surface, which
/// is how a solid wall reads as empty air.
void WMORenderer::GroupResources::gatherCellTriangles(
        const std::vector<std::vector<uint32_t>>& cells,
        float minX, float minY, float maxX, float maxY,
        std::vector<uint32_t>& out) const {
    out.clear();
    if (gridCellsX == 0 || gridCellsY == 0 || cells.empty()) return;

    const auto range = cellRangeCovering(
        gridCellsX, gridCellsY,
        boundingBoxMax.x - boundingBoxMin.x, boundingBoxMax.y - boundingBoxMin.y,
        glm::vec2(gridOrigin.x, gridOrigin.y), minX, minY, maxX, maxY);
    if (!range) return;

    // About eight triangles a cell, which is what the meshes here average.
    out.reserve(range->count() * 8);

    // One cell cannot hand out the same triangle twice, so the dedup and the
    // bitset clear it needs are pure cost in the commonest case.
    const bool multiCell = (range->minX != range->maxX || range->minY != range->maxY);
    if (multiCell && !triVisited.empty()) {
        for (int cy = range->minY; cy <= range->maxY; ++cy) {
            for (int cx = range->minX; cx <= range->maxX; ++cx) {
                for (uint32_t tri : cells[cy * gridCellsX + cx]) {
                    const uint32_t index = tri / 3;
                    if (!triVisited[index]) {
                        triVisited[index] = 1;
                        out.push_back(tri);
                    }
                }
            }
        }
        for (uint32_t tri : out) triVisited[tri / 3] = 0;
    } else {
        for (int cy = range->minY; cy <= range->maxY; ++cy) {
            for (int cx = range->minX; cx <= range->maxX; ++cx) {
                const auto& cell = cells[cy * gridCellsX + cx];
                out.insert(out.end(), cell.begin(), cell.end());
            }
        }
    }
}

void WMORenderer::GroupResources::getTrianglesInRange(
        float minX, float minY, float maxX, float maxY,
        std::vector<uint32_t>& out) const {
    gatherCellTriangles(cellTriangles, minX, minY, maxX, maxY, out);
}

void WMORenderer::GroupResources::getFloorTrianglesInRange(
        float minX, float minY, float maxX, float maxY,
        std::vector<uint32_t>& out) const {
    gatherCellTriangles(cellFloorTriangles, minX, minY, maxX, maxY, out);
}

void WMORenderer::GroupResources::getWallTrianglesInRange(
        float minX, float minY, float maxX, float maxY,
        std::vector<uint32_t>& out) const {
    gatherCellTriangles(cellWallTriangles, minX, minY, maxX, maxY, out);
}

void WMORenderer::appendSurfaceTriangles(const glm::vec3& boxMin, const glm::vec3& boxMax,
                                         std::vector<glm::vec3>& out) const {
    std::vector<size_t> candidates;
    gatherCandidates(boxMin, boxMax, candidates);
    std::vector<uint32_t> triangles;
    for (size_t idx : candidates) {
        const auto& instance = instances[idx];
        if (glm::any(glm::lessThan(instance.worldBoundsMax, boxMin)) ||
            glm::any(glm::greaterThan(instance.worldBoundsMin, boxMax))) continue;
        auto it = loadedModels.find(instance.modelId);
        if (it == loadedModels.end()) continue;
        const ModelData& model = it->second;
        // The box in the model's own space, for the group's cell grid.
        glm::vec3 localMin(std::numeric_limits<float>::max());
        glm::vec3 localMax(std::numeric_limits<float>::lowest());
        for (int corner = 0; corner < 8; ++corner) {
            const glm::vec3 world((corner & 1) ? boxMax.x : boxMin.x, (corner & 2) ? boxMax.y : boxMin.y,
                                  (corner & 4) ? boxMax.z : boxMin.z);
            const glm::vec3 local(instance.invModelMatrix * glm::vec4(world, 1.0f));
            localMin = glm::min(localMin, local);
            localMax = glm::max(localMax, local);
        }
        for (size_t gi = 0; gi < model.groups.size(); ++gi) {
            const auto& group = model.groups[gi];
            if (gi < instance.worldGroupBounds.size()) {
                const auto& [gMin, gMax] = instance.worldGroupBounds[gi];
                if (glm::any(glm::lessThan(gMax, boxMin)) || glm::any(glm::greaterThan(gMin, boxMax))) continue;
            }
            // What drawWMOs leaves out: antiportals.
            if (group.antiportal) continue;
            if (!instance.drawnGroups.empty() && !instance.drawnGroups[gi]) continue;
            group.getTrianglesInRange(localMin.x, localMin.y, localMax.x, localMax.y, triangles);
            for (uint32_t triStart : triangles) {
                const size_t triangle = triStart / 3;
                // MOPY material 0xFF: collision only, in no render batch.
                if (triangle < group.triMaterials.size() && group.triMaterials[triangle] == 0xFF) continue;
                glm::vec3 v[3];
                for (int k = 0; k < 3; ++k) {
                    v[k] = glm::vec3(instance.modelMatrix *
                                     glm::vec4(group.collisionVertices[group.collisionIndices[triStart + k]], 1.0f));
                }
                const glm::vec3 lo = glm::min(v[0], glm::min(v[1], v[2]));
                const glm::vec3 hi = glm::max(v[0], glm::max(v[1], v[2]));
                if (glm::any(glm::lessThan(hi, boxMin)) || glm::any(glm::greaterThan(lo, boxMax))) continue;
                out.insert(out.end(), {v[0], v[1], v[2]});
            }
        }
    }
}

std::optional<float> WMORenderer::getFloorHeight(float glX, float glY, float glZ, float* outNormalZ, float referenceZ,
                                                  std::optional<uint32_t>* outTerrainType) const {
    if (outTerrainType) outTerrainType->reset();
    const bool byReference = std::isfinite(referenceZ);
    // Per-frame cache disabled: camera and player query the same (x,y) at
    // different Z within a single frame. The allowAbove filter depends on glZ,
    // so caching by (x,y) alone returns wrong floors across Z contexts.

    QueryTimer timer(&queryTimeMs, &queryCallCount);
    std::optional<float> bestFloor;
    float bestNormalZ = 1.0f;
    bool bestFromLowPlatform = false;
    // For the "no floor where there plainly is one" report at the end.
    bool overlappedInXY = false;
    bool rejectedByZ = false;
    float rejectedZMin = 0.0f, rejectedZMax = 0.0f;

    // A moving transport (elevator, ship hull) IS ordinary collision - you walk
    // up a docked ship's hull and you stand on a lift with no attachment at all
    // (an elevator is neither an M2 nor a client-animated ship, so client-side
    // boarding never fires for it - see GameHandler::updateM2TransportBoarding).
    // Excluding transports outright therefore deletes the only floor those cases
    // have. But an unrestricted transport hit is the Undercity elevator yo-yo: as
    // the lift cycles through a bystander's (x,y) its deck enters the candidate
    // set metres above or below them and the pick flips to it and back.
    // So a transport deck counts only when it is genuinely underfoot, not
    // anywhere along the shaft. referenceZ is the true feet when the caller
    // supplies one (the player's physics query does); otherwise glZ stands in,
    // and that is the probe rather than the feet - the caller raised it by its
    // step-up budget - so back that lift out first.
    constexpr float kTransportProbeLift = 2.0f;
    constexpr float kTransportFloorReach = 3.0f;
    const float transportFloorMinZ =
        (byReference ? referenceZ : glZ - kTransportProbeLift) - kTransportFloorReach;

    // World-space ray pointing straight down from just above the instance.
    //
    // The start must not depend on glZ. It used to be glZ + 500, and a float
    // near 530 has a step of 6e-5, so the hit height carried that much noise
    // from the probe height. A player standing still re-probes from the floor
    // it just found; the answer came back one step different, the next probe
    // one step back, and the feet alternated between two heights every frame
    // (Scarlet Monastery chapel: 31.5495 / 31.5496). Each heartbeat then told
    // the server the player had moved, and CMaNGOS Spell::update cancels a
    // cast with SPELL_INTERRUPT_FLAG_MOVEMENT on any change of position:
    // "Opening" on a torch was interrupted while nobody touched a key.
    // Starting from the instance's top makes the same column give the same
    // floor whatever the probe height, and every floor of the instance is
    // still below the start.
    const auto worldOriginFor = [&](const WMOInstance& instance) {
        return glm::vec3(glX, glY, instance.worldBoundsMax.z + 1.0f);
    };
    glm::vec3 worldDir(0.0f, 0.0f, -1.0f);

    // Lambda to test a single group for floor hits
    auto testGroupFloor = [&](const WMOInstance& instance, const ModelData& model,
                              const GroupResources& group,
                              const glm::vec3& localOrigin, const glm::vec3& localDir) {
        const auto& verts = group.collisionVertices;
        const auto& indices = group.collisionIndices;

        // Use unfiltered triangle list: a vertical ray naturally misses vertical
        // geometry via ray-triangle intersection, so pre-filtering by normal is
        // unnecessary and risks excluding legitimate floor geometry (steep ramps,
        // stair treads with non-trivial normals).
        // Search where the ray actually crosses this group, not where it
        // starts.
        //
        // The ray is straight down in world space and begins five hundred units
        // above the query. Transformed into the model's own space it stays
        // straight only while the model has no pitch and no roll - a yaw keeps
        // a vertical ray vertical, which is why every building placed flat has
        // always worked. Give it pitch and the local ray leans, and five
        // hundred units of lean puts the origin's XY nowhere near the deck it
        // passes through. Searching a metre either side of that origin found
        // nothing, every time, and the log said so: considered, and no triangle
        // hit.
        //
        // Darkshore's bridges are the placements with pitch. They are also the
        // ones you fall through.
        if (!trianglesAlongRay(group, localOrigin, localDir, tl_triScratch)) return;

        for (uint32_t triStart : tl_triScratch) {
            const uint32_t triIdx = triStart / 3;
            if (triIdx < group.triMopyFlags.size() &&
                !wmoTriangleBlocksMovement(group.triMopyFlags[triIdx])) {
                continue;
            }
            const glm::vec3& v0 = verts[indices[triStart]];
            const glm::vec3& v1 = verts[indices[triStart + 1]];
            const glm::vec3& v2 = verts[indices[triStart + 2]];

            // One test, not two: the intersection is two-sided, so the
            // reversed winding this used to retry gives the same answer.
            const float t = rayTriangleIntersect(localOrigin, localDir, v0, v1, v2);

            if (t > 0.0f) {
                glm::vec3 hitLocal = localOrigin + localDir * t;
                glm::vec3 hitWorld = glm::vec3(instance.modelMatrix * glm::vec4(hitLocal, 1.0f));

                // Accept floors at or below glZ (the caller already elevates
                // glZ by stepUpBudget to handle step-up range).  Among those,
                // pick the highest (closest to feet).
                if (hitWorld.z <= glZ &&
                    (!instance.isTransport || hitWorld.z >= transportFloorMinZ)) {
                    const bool better = !bestFloor
                        ? true
                        : (byReference
                               ? std::abs(hitWorld.z - referenceZ) <
                                     std::abs(*bestFloor - referenceZ)
                               : hitWorld.z > *bestFloor);
                    if (better) {
                        bestFloor = hitWorld.z;
                        if (outTerrainType) {
                            outTerrainType->reset();
                            const size_t triangle = triStart / 3;
                            if (triangle < group.triMaterials.size()) {
                                const uint8_t material = group.triMaterials[triangle];
                                if (material < model.materialTerrainTypes.size())
                                    *outTerrainType = model.materialTerrainTypes[material];
                            }
                        }
                        bestFromLowPlatform = model.isLowPlatform;

                        // Use precomputed normal, ensure upward, transform to world
                        glm::vec3 localNormal = group.triNormals[triStart / 3];
                        if (localNormal.z < 0.0f) localNormal = -localNormal;
                        glm::vec3 worldNormal = glm::normalize(
                            glm::vec3(instance.modelMatrix * glm::vec4(localNormal, 0.0f)));
                        bestNormalZ = std::abs(worldNormal.z);
                    }
                }
            }
        }
    };

    // Fast path: current active interior group and its neighbors are usually
    // the right answer for player-floor queries while moving in cities/buildings.
    if (activeGroup_.isValid() && activeGroup_.instanceIdx < instances.size()) {
        const auto& instance = instances[activeGroup_.instanceIdx];
        auto it = loadedModels.find(instance.modelId);
        if (it != loadedModels.end() && instance.modelId == activeGroup_.modelId) {
            const ModelData& model = it->second;
            glm::vec3 localOrigin = glm::vec3(instance.invModelMatrix * glm::vec4(worldOriginFor(instance), 1.0f));
            glm::vec3 localDir = glm::normalize(glm::vec3(instance.invModelMatrix * glm::vec4(worldDir, 0.0f)));

            auto testGroupIdx = [&](uint32_t gi) {
                if (gi >= model.groups.size()) return;
                if (gi < instance.worldGroupBounds.size()) {
                    const auto& [gMin, gMax] = instance.worldGroupBounds[gi];
                    if (glX < gMin.x || glX > gMax.x ||
                        glY < gMin.y || glY > gMax.y ||
                        glZ - 4.0f > gMax.z) {
                        return;
                    }
                }
                const auto& group = model.groups[gi];
                if (!rayIntersectsAABB(localOrigin, localDir, group.boundingBoxMin, group.boundingBoxMax)) {
                    return;
                }
                testGroupFloor(instance, model, group, localOrigin, localDir);
            };

            if (activeGroup_.groupIdx >= 0) {
                testGroupIdx(static_cast<uint32_t>(activeGroup_.groupIdx));
            }
            for (uint32_t ngi : activeGroup_.neighborGroups) {
                testGroupIdx(ngi);
            }
        }
    }

    // Full scan: test all instances (active group result above is not
    // early-returned because overlapping WMO instances need full coverage).
    glm::vec3 queryMin(glX - 2.0f, glY - 2.0f, glZ - 8.0f);
    glm::vec3 queryMax(glX + 2.0f, glY + 2.0f, glZ + 10.0f);
    gatherCandidates(queryMin, queryMax, tl_candidateScratch);

    for (size_t idx : tl_candidateScratch) {
        const auto& instance = instances[idx];
        if (glX < instance.worldBoundsMin.x || glX > instance.worldBoundsMax.x ||
            glY < instance.worldBoundsMin.y || glY > instance.worldBoundsMax.y) {
            continue;
        }

        auto it = loadedModels.find(instance.modelId);
        if (it == loadedModels.end()) continue;

        const ModelData& model = it->second;
        float zMarginDown = model.isLowPlatform ? 20.0f : 2.0f;
        float zMarginUp = model.isLowPlatform ? 20.0f : 4.0f;

        // Broad-phase reject in world space to avoid expensive matrix transforms.
        if (bestFloor && instance.worldBoundsMax.z <= (*bestFloor + 0.05f)) {
            continue;
        }
        const bool insideXY = glX >= instance.worldBoundsMin.x && glX <= instance.worldBoundsMax.x &&
                              glY >= instance.worldBoundsMin.y && glY <= instance.worldBoundsMax.y;
        if (!withinWorldBounds(instance, glX, glY, glZ, zMarginDown, zMarginUp)) {
            // Over a building and rejected anyway: the only thing that can do
            // that here is the Z window, and it is worth naming when the answer
            // comes back "no floor" - falling through something you are
            // standing on looks the same whether it was never considered or
            // considered and missed.
            if (insideXY) {
                overlappedInXY = true;
                rejectedByZ = true;
                rejectedZMin = instance.worldBoundsMin.z;
                rejectedZMax = instance.worldBoundsMax.z;
            }
            continue;
        }
        if (insideXY) overlappedInXY = true;

        // World-space pre-pass: check which groups' world XY bounds contain
        // the query point. For a vertical ray this eliminates most groups
        // before any local-space math.
        bool anyGroupOverlaps = false;
        for (size_t gi = 0; gi < model.groups.size() && gi < instance.worldGroupBounds.size(); ++gi) {
            const auto& [gMin, gMax] = instance.worldGroupBounds[gi];
            if (glX >= gMin.x && glX <= gMax.x &&
                glY >= gMin.y && glY <= gMax.y &&
                glZ - 4.0f <= gMax.z) {
                anyGroupOverlaps = true;
                break;
            }
        }
        if (!anyGroupOverlaps) continue;

        // Use cached inverse matrix
        glm::vec3 localOrigin = glm::vec3(instance.invModelMatrix * glm::vec4(worldOriginFor(instance), 1.0f));
        glm::vec3 localDir = glm::normalize(glm::vec3(instance.invModelMatrix * glm::vec4(worldDir, 0.0f)));

        for (size_t gi = 0; gi < model.groups.size(); ++gi) {
            // World-space group cull - vertical ray at (glX, glY)
            if (gi < instance.worldGroupBounds.size()) {
                const auto& [gMin, gMax] = instance.worldGroupBounds[gi];
                if (glX < gMin.x || glX > gMax.x ||
                    glY < gMin.y || glY > gMax.y ||
                    glZ - 4.0f > gMax.z) {
                    continue;
                }
            }

            const auto& group = model.groups[gi];
            if (!rayIntersectsAABB(localOrigin, localDir, group.boundingBoxMin, group.boundingBoxMax)) {
                continue;
            }

            testGroupFloor(instance, model, group, localOrigin, localDir);
        }
    }

    // Persistent grid cache disabled (see above comment about stairs fall-through)

    // Standing over a building and told there is no floor.
    //
    // Falling through something looks the same whether the building was never
    // considered, considered and rejected by the height window, or considered
    // and simply missed by the ray - and those need different fixes.
    //
    // Once per place, not once a second. The floor is queried every frame, so
    // a player standing on a spot with no floor under it wrote the same line
    // with the same three coordinates for as long as they stood there - a
    // second apart, forever - and the second line said nothing the first did
    // not. A place is a couple of units across, and after enough of them the
    // report says so and stops: a run that is finding hundreds has one
    // problem, not hundreds.
    if (!bestFloor && overlappedInXY) {
        struct ReportedPlace { float x, y, z; };
        static std::vector<ReportedPlace> reported;
        static bool reportedCap = false;
        constexpr float kSamePlace = 2.0f;
        constexpr size_t kMaxPlaces = 32;
        const bool seenHere = std::any_of(
            reported.begin(), reported.end(), [&](const ReportedPlace& p) {
                const float dx = p.x - glX, dy = p.y - glY, dz = p.z - glZ;
                return dx * dx + dy * dy + dz * dz <= kSamePlace * kSamePlace;
            });
        if (!seenHere && reported.size() >= kMaxPlaces) {
            if (!reportedCap) {
                reportedCap = true;
                LOG_WARNING("No WMO floor: ", kMaxPlaces, " places reported and no more "
                            "will be - this is one problem, not ", kMaxPlaces, " of them");
            }
        } else if (!seenHere) {
            reported.push_back({glX, glY, glZ});
            if (rejectedByZ) {
                LOG_WARNING("No WMO floor at (", glX, ",", glY, ",", glZ,
                            ") - a building covers that spot but its height window "
                            "rejected the query: bounds z=[", rejectedZMin, "..",
                            rejectedZMax, "]");
            } else {
                LOG_WARNING("No WMO floor at (", glX, ",", glY, ",", glZ,
                            ") - a building covers that spot, was considered, and "
                            "no triangle was hit");
            }
        }
    }

    if (bestFloor && outNormalZ) {
        *outNormalZ = bestNormalZ;
    }

    return bestFloor;
}

std::optional<float> WMORenderer::getInstanceFloorHeight(uint32_t instanceId,
                                                         float glX, float glY, float glZ,
                                                         float* outNormalZ) const {
    const auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end() || idxIt->second >= instances.size()) {
        return std::nullopt;
    }

    const auto& instance = instances[idxIt->second];
    const auto modelIt = loadedModels.find(instance.modelId);
    if (modelIt == loadedModels.end()) return std::nullopt;
    const auto& model = modelIt->second;

    if (!withinWorldBounds(instance, glX, glY, glZ, 2.0f, 4.0f)) {
        return std::nullopt;
    }

    // From the instance's top, not from glZ: see getFloorHeight.
    const glm::vec3 worldOrigin(glX, glY, instance.worldBoundsMax.z + 1.0f);
    const glm::vec3 worldDir(0.0f, 0.0f, -1.0f);
    const glm::vec3 localOrigin(instance.invModelMatrix * glm::vec4(worldOrigin, 1.0f));
    const glm::vec3 localDir = glm::normalize(
        glm::vec3(instance.invModelMatrix * glm::vec4(worldDir, 0.0f)));

    std::optional<float> bestFloor;
    float bestNormalZ = 1.0f;
    for (size_t gi = 0; gi < model.groups.size(); ++gi) {
        if (gi < instance.worldGroupBounds.size()) {
            const auto& [gMin, gMax] = instance.worldGroupBounds[gi];
            if (glX < gMin.x || glX > gMax.x ||
                glY < gMin.y || glY > gMax.y || glZ - 4.0f > gMax.z) {
                continue;
            }
        }

        const auto& group = model.groups[gi];
        if (!rayIntersectsAABB(localOrigin, localDir,
                               group.boundingBoxMin, group.boundingBoxMax)) {
            continue;
        }

        // Between where the ray enters this group and where it leaves, for the
        // reason getFloorHeight gives: a vertical world ray is not vertical in
        // the local space of anything with pitch, and its origin is five
        // hundred units away from what it crosses.
        if (!trianglesAlongRay(group, localOrigin, localDir, tl_triScratch)) continue;
        for (uint32_t triStart : tl_triScratch) {
            const uint32_t triIdx = triStart / 3;
            if (triIdx < group.triMopyFlags.size() &&
                !wmoTriangleBlocksMovement(group.triMopyFlags[triIdx])) {
                continue;
            }
            const auto& verts = group.collisionVertices;
            const auto& indices = group.collisionIndices;
            const glm::vec3& v0 = verts[indices[triStart]];
            const glm::vec3& v1 = verts[indices[triStart + 1]];
            const glm::vec3& v2 = verts[indices[triStart + 2]];

            const float t = rayTriangleIntersect(localOrigin, localDir, v0, v1, v2);
            if (t <= 0.0f) continue;

            const glm::vec3 hitLocal = localOrigin + localDir * t;
            const glm::vec3 hitWorld(instance.modelMatrix * glm::vec4(hitLocal, 1.0f));
            if (hitWorld.z > glZ || (bestFloor && hitWorld.z <= *bestFloor)) continue;

            bestFloor = hitWorld.z;
            glm::vec3 localNormal = group.triNormals[triStart / 3];
            if (localNormal.z < 0.0f) localNormal = -localNormal;
            const glm::vec3 worldNormal = glm::normalize(
                glm::vec3(instance.modelMatrix * glm::vec4(localNormal, 0.0f)));
            bestNormalZ = std::abs(worldNormal.z);
        }
    }

    if (bestFloor && outNormalZ) *outNormalZ = bestNormalZ;
    return bestFloor;
}

void WMORenderer::debugDumpGroupsAtPosition(float glX, float glY, float glZ) const {
    LOG_WARNING("=== WMO Floor Debug at render(", glX, ", ", glY, ", ", glZ, ") ===");

    glm::vec3 worldOrigin(glX, glY, glZ + 500.0f);
    glm::vec3 worldDir(0.0f, 0.0f, -1.0f);

    int totalInstancesChecked = 0;
    int totalGroupsOverlapping = 0;
    int totalFloorHits = 0;

    for (const auto& instance : instances) {
        auto it = loadedModels.find(instance.modelId);
        if (it == loadedModels.end()) continue;
        const ModelData& model = it->second;

        // Check instance world bounds
        if (!withinWorldBounds(instance, glX, glY, glZ, 20.0f, 20.0f)) {
            continue;
        }
        totalInstancesChecked++;
        LOG_WARNING("  Instance modelId=", instance.modelId,
                    " isTransport=", instance.isTransport ? 1 : 0,
                    " worldBounds=(", instance.worldBoundsMin.x, ",", instance.worldBoundsMin.y, ",", instance.worldBoundsMin.z,
                    ")-(", instance.worldBoundsMax.x, ",", instance.worldBoundsMax.y, ",", instance.worldBoundsMax.z,
                    ") groups=", model.groups.size());

        glm::vec3 localOrigin = glm::vec3(instance.invModelMatrix * glm::vec4(worldOrigin, 1.0f));
        glm::vec3 localDir = glm::normalize(glm::vec3(instance.invModelMatrix * glm::vec4(worldDir, 0.0f)));
        // If the model is rotated, localDir is not straight down, so the vertical
        // world ray drifts in local XY as it descends - the reason the grid query
        // (a box at the ray ORIGIN's local xy) can miss a floor the full scan hits.
        LOG_WARNING("    localDir=(", localDir.x, ",", localDir.y, ",", localDir.z, ")");

        for (size_t gi = 0; gi < model.groups.size(); ++gi) {
            // Check world-space group bounds
            if (gi < instance.worldGroupBounds.size()) {
                const auto& [gMin, gMax] = instance.worldGroupBounds[gi];
                if (glX < gMin.x || glX > gMax.x ||
                    glY < gMin.y || glY > gMax.y) {
                    continue;
                }
            }
            totalGroupsOverlapping++;
            const auto& group = model.groups[gi];

            // Count floor triangles in this group under the player, and - this
            // is what the fall-through needs - the hit CLOSEST to the query Z,
            // which is the floor the by-reference player pick would want. If a
            // group has a floorHit near the feet but the pick still dropped, the
            // grid query (getTrianglesInRange) missed it; if the closest hit is
            // the low one it dropped to, the floor at the feet is genuinely
            // absent here.
            int floorTris = 0;
            float bestHitZ = -999999.0f;
            float lowestHitZ = 999999.0f;
            float closestHitZ = 0.0f;
            float closestDist = 1e30f;
            const auto& verts = group.collisionVertices;
            const auto& indices = group.collisionIndices;
            for (size_t ti = 0; ti + 2 < indices.size(); ti += 3) {
                const glm::vec3& v0 = verts[indices[ti]];
                const glm::vec3& v1 = verts[indices[ti + 1]];
                const glm::vec3& v2 = verts[indices[ti + 2]];
                const float t = rayTriangleIntersect(localOrigin, localDir, v0, v1, v2);
                if (t > 0.0f) {
                    glm::vec3 hitLocal = localOrigin + localDir * t;
                    glm::vec3 hitWorld = glm::vec3(instance.modelMatrix * glm::vec4(hitLocal, 1.0f));
                    floorTris++;
                    totalFloorHits++;
                    if (hitWorld.z > bestHitZ) bestHitZ = hitWorld.z;
                    if (hitWorld.z < lowestHitZ) lowestHitZ = hitWorld.z;
                    const float d = std::abs(hitWorld.z - glZ);
                    if (d < closestDist) { closestDist = d; closestHitZ = hitWorld.z; }
                }
            }

            // The grid path getFloorHeight uses. This was written to catch
            // exactly the fault it names, and it named it correctly: a box at
            // the ray origin's local xy misses everything a slanted local ray
            // crosses. That is fixed - both queries now search between where
            // the ray enters the group and where it leaves - and this stays as
            // the check that they agree.
            std::vector<uint32_t> gridTris;
            trianglesAlongRay(group, localOrigin, localDir, gridTris);
            int gridFloorTris = 0;
            float gridClosestZ = 0.0f, gridClosestDist = 1e30f;
            for (uint32_t triStart : gridTris) {
                if (triStart + 2 >= indices.size()) continue;
                const glm::vec3& g0 = verts[indices[triStart]];
                const glm::vec3& g1 = verts[indices[triStart + 1]];
                const glm::vec3& g2 = verts[indices[triStart + 2]];
                const float gt = rayTriangleIntersect(localOrigin, localDir, g0, g1, g2);
                if (gt > 0.0f) {
                    glm::vec3 gHitLocal = localOrigin + localDir * gt;
                    float gz = (instance.modelMatrix * glm::vec4(gHitLocal, 1.0f)).z;
                    gridFloorTris++;
                    const float gd = std::abs(gz - glZ);
                    if (gd < gridClosestDist) { gridClosestDist = gz; gridClosestZ = gz; gridClosestDist = gd; }
                }
            }

            glm::vec3 gWorldMin(0), gWorldMax(0);
            if (gi < instance.worldGroupBounds.size()) {
                gWorldMin = instance.worldGroupBounds[gi].first;
                gWorldMax = instance.worldGroupBounds[gi].second;
            }
            LOG_WARNING("    Group[", gi, "] flags=0x", std::hex, group.groupFlags, std::dec,
                        " verts=", group.collisionVertices.size(),
                        " tris=", group.collisionIndices.size()/3,
                        " batches=", group.mergedBatches.size(),
                        " floorHits=", floorTris,
                        " bestHitZ=", bestHitZ,
                        " lowestHitZ=", (floorTris ? lowestHitZ : -999999.0f),
                        " closestToFeetZ=", (floorTris ? closestHitZ : -999999.0f),
                        " GRIDhits=", gridFloorTris,
                        " GRIDclosestZ=", (gridFloorTris ? gridClosestZ : -999999.0f),
                        " wBounds=(", gWorldMin.x, ",", gWorldMin.y, ",", gWorldMin.z,
                        ")-(", gWorldMax.x, ",", gWorldMax.y, ",", gWorldMax.z, ")");
        }
    }

    // Is there a floor at the feet a step to either side?
    //
    // Every dump so far shows the pick taking the nearest surface at or below
    // the probe with nothing nearer losing, so the selection is not the
    // problem: there is simply no floor at the feet. Two things look identical
    // from here - a gap in the mesh the player is straddling, and a real ledge
    // they walked off - and a floor found a third of a yard away at the height
    // they were standing separates them. If the neighbours have it and the
    // centre does not, the mesh is missing a floor; if none of them do, the
    // player stepped off something real.
    {
        const float probeZ = glZ;
        constexpr float kStep = 0.35f;
        const std::pair<float, float> offsets[] = {
            {0.0f, 0.0f}, {kStep, 0.0f}, {-kStep, 0.0f}, {0.0f, kStep}, {0.0f, -kStep},
        };
        const char* names[] = {"centre", "+x", "-x", "+y", "-y"};
        std::string line;
        for (size_t i = 0; i < 5; ++i) {
            const auto h = getFloorHeight(glX + offsets[i].first,
                                          glY + offsets[i].second, probeZ);
            line += names[i];
            line += h ? ("=" + std::to_string(*h)) : std::string("=none");
            line += "  ";
        }
        LOG_WARNING("    floor a step aside: ", line);
    }

    LOG_WARNING("=== Total: ", totalInstancesChecked, " instances, ",
                totalGroupsOverlapping, " overlapping groups, ",
                totalFloorHits, " floor hits ===");
}

/// Whether solid WMO geometry stands between two points.
///
/// A line of sight test, and deliberately not checkWallCollision: that one
/// models a player cylinder stepping a fraction of a metre, and its group
/// pre-pass only keeps groups near `to` - handed a forty-unit ray from the
/// camera it would never look at the wall in the middle. This walks the whole
/// segment instead, and asks nothing about radius or step height. A sight line
/// is a line.
///
/// Both ends are excluded by a small margin. The far end because a player
/// standing against a wall is in front of it, not behind it, and the near end
/// because a camera clipped into geometry would otherwise call everything
/// blocked.
bool WMORenderer::segmentBlocked(const glm::vec3& from, const glm::vec3& to) const {
    QueryTimer timer(&queryTimeMs, &queryCallCount);

    const glm::vec3 delta = to - from;
    const float dist = glm::length(delta);
    if (dist < 1e-4f) return false;
    const glm::vec3 dir = delta / dist;

    // A metre of slack around the segment: gatherCandidates works in world
    // bounds and a wall's instance box can begin just outside them.
    const glm::vec3 queryMin = glm::min(from, to) - glm::vec3(1.0f);
    const glm::vec3 queryMax = glm::max(from, to) + glm::vec3(1.0f);
    gatherCandidates(queryMin, queryMax, tl_candidateScratch);

    for (size_t idx : tl_candidateScratch) {
        const auto& instance = instances[idx];
        if (outsideCollisionFocus(instance)) continue;
        if (!rayIntersectsAABB(from, dir, instance.worldBoundsMin, instance.worldBoundsMax)) {
            continue;
        }

        auto it = loadedModels.find(instance.modelId);
        if (it == loadedModels.end()) continue;
        const ModelData& model = it->second;

        // Into the instance's own space, where its collision mesh lives. The
        // distance is measured there too: a scaled placement makes local units
        // and world units different lengths, and t comes back in local ones.
        const glm::vec3 localFrom = glm::vec3(instance.invModelMatrix * glm::vec4(from, 1.0f));
        const glm::vec3 localTo   = glm::vec3(instance.invModelMatrix * glm::vec4(to, 1.0f));
        const glm::vec3 localDelta = localTo - localFrom;
        const float localDist = glm::length(localDelta);
        if (localDist < 1e-4f) continue;
        const glm::vec3 localDir = localDelta / localDist;
        const float nearMargin = 0.15f * (localDist / dist);
        const float farMargin  = 0.35f * (localDist / dist);

        for (size_t gi = 0; gi < model.groups.size(); ++gi) {
            if (gi < instance.worldGroupBounds.size()) {
                const auto& [gMin, gMax] = instance.worldGroupBounds[gi];
                if (!rayIntersectsAABB(from, dir, gMin, gMax)) continue;
            }
            const auto& group = model.groups[gi];
            if (!trianglesAlongRay(group, localFrom, localDir, tl_triScratch)) continue;

            const auto& verts = group.collisionVertices;
            const auto& indices = group.collisionIndices;
            for (uint32_t triStart : tl_triScratch) {
                if (triStart + 2 >= indices.size()) continue;
                const uint32_t triIdx = triStart / 3;
                if (triIdx < group.triMopyFlags.size() &&
                    !wmoTriangleBlocksMovement(group.triMopyFlags[triIdx])) {
                    continue;
                }
                const glm::vec3& v0 = verts[indices[triStart]];
                const glm::vec3& v1 = verts[indices[triStart + 1]];
                const glm::vec3& v2 = verts[indices[triStart + 2]];
                const float t = rayTriangleIntersect(localFrom, localDir, v0, v1, v2);
                if (t > nearMargin && t < localDist - farMargin) return true;
            }
        }
    }
    return false;
}

void WMORenderer::appendBodyCollisionTriangles(const glm::vec3& worldLow, const glm::vec3& worldHigh,
                                               std::vector<movement::BodyTriangle>& triangles) const {
    gatherCandidates(worldLow, worldHigh, tl_candidateScratch);
    for (size_t idx : tl_candidateScratch) {
        const auto& instance = instances[idx];
        if (outsideCollisionFocus(instance)) continue;
        if (glm::any(glm::lessThan(instance.worldBoundsMax, worldLow)) ||
            glm::any(glm::greaterThan(instance.worldBoundsMin, worldHigh))) {
            continue;
        }

        const auto it = loadedModels.find(instance.modelId);
        if (it == loadedModels.end()) continue;
        const ModelData& model = it->second;

        glm::vec3 localLow(std::numeric_limits<float>::max());
        glm::vec3 localHigh(std::numeric_limits<float>::lowest());
        for (int corner = 0; corner < 8; ++corner) {
            const glm::vec3 world((corner & 1) ? worldHigh.x : worldLow.x,
                                  (corner & 2) ? worldHigh.y : worldLow.y,
                                  (corner & 4) ? worldHigh.z : worldLow.z);
            const glm::vec3 local(instance.invModelMatrix * glm::vec4(world, 1.0f));
            localLow = glm::min(localLow, local);
            localHigh = glm::max(localHigh, local);
        }

        for (size_t gi = 0; gi < model.groups.size(); ++gi) {
            const auto& group = model.groups[gi];
            if (group.unreachable || group.antiportal || group.collisionIndices.empty()) continue;
            if (gi < instance.worldGroupBounds.size()) {
                const auto& [gMin, gMax] = instance.worldGroupBounds[gi];
                if (glm::any(glm::lessThan(gMax, worldLow)) ||
                    glm::any(glm::greaterThan(gMin, worldHigh))) {
                    continue;
                }
            }
            if (glm::any(glm::lessThan(group.boundingBoxMax, localLow)) ||
                glm::any(glm::greaterThan(group.boundingBoxMin, localHigh))) {
                continue;
            }

            group.getTrianglesInRange(localLow.x, localLow.y, localHigh.x, localHigh.y, tl_triScratch);
            const auto& verts = group.collisionVertices;
            const auto& indices = group.collisionIndices;
            for (uint32_t triStart : tl_triScratch) {
                const uint32_t triIdx = triStart / 3;
                if (triIdx < group.triBounds.size()) {
                    const auto& tb = group.triBounds[triIdx];
                    if (tb.maxZ < localLow.z || tb.minZ > localHigh.z) continue;
                }
                if (triIdx < group.triMopyFlags.size() &&
                    !wmoTriangleBlocksMovement(group.triMopyFlags[triIdx])) {
                    continue;
                }
                triangles.push_back({
                    glm::vec3(instance.modelMatrix * glm::vec4(verts[indices[triStart]], 1.0f)),
                    glm::vec3(instance.modelMatrix * glm::vec4(verts[indices[triStart + 1]], 1.0f)),
                    glm::vec3(instance.modelMatrix * glm::vec4(verts[indices[triStart + 2]], 1.0f))});
            }
        }
    }
}

bool WMORenderer::constrainFloorSnap(const glm::vec3& from, float requestedZ,
                                     float radius, float height,
                                     float& supportZ, float& supportNormalZ) const {
    assert(requestedZ < from.z);
    QueryTimer timer(&queryTimeMs, &queryCallCount);
    const float bodyRadius = std::max(radius, 0.1f);
    const float bodyHeight = std::max(height, bodyRadius * movement::kBodyBevel + 0.01f);
    thread_local std::vector<movement::BodyTriangle> triangles;
    triangles.clear();
    appendBodyCollisionTriangles(
        glm::vec3(from.x - bodyRadius - 0.1f, from.y - bodyRadius - 0.1f, requestedZ - 0.1f),
        glm::vec3(from.x + bodyRadius + 0.1f, from.y + bodyRadius + 0.1f, from.z + bodyHeight + 0.1f),
        triangles);
    return movement::constrainBodyDescent(from, requestedZ, bodyRadius, bodyHeight,
                                          triangles, supportZ, supportNormalZ);
}

glm::vec3 WMORenderer::stepBody(const glm::vec3& from, const glm::vec3& to,
                                float radius, float height, float maxStepUp,
                                glm::vec3* landingFace) const {
    QueryTimer timer(&queryTimeMs, &queryCallCount);
    const float bodyRadius = std::max(radius, 0.1f);
    const float bodyHeight = std::max(height, bodyRadius * movement::kBodyBevel + 0.01f);
    thread_local std::vector<movement::BodyTriangle> triangles;
    triangles.clear();
    appendBodyCollisionTriangles(
        glm::min(from, to) - glm::vec3(bodyRadius + 0.25f, bodyRadius + 0.25f, 0.25f),
        glm::max(from, to) + glm::vec3(bodyRadius + 0.25f, bodyRadius + 0.25f, bodyHeight + maxStepUp + 0.25f),
        triangles);
    return movement::stepBodyAcross(from, to, bodyRadius, bodyHeight, triangles, maxStepUp, landingFace);
}

bool WMORenderer::checkWallCollision(const glm::vec3& from, const glm::vec3& to, glm::vec3& adjustedPos, bool /*insideWMO*/,
                                      bool airborne, float playerRadius, float playerHeight) const {
    QueryTimer timer(&queryTimeMs, &queryCallCount);
    adjustedPos = to;

    const glm::vec3 moveDir = to - from;
    if (glm::dot(moveDir, moveDir) < 1e-6f) return false;

    const float bodyRadius = std::max(playerRadius, 0.1f);
    const float bodyHeight = std::max(playerHeight, bodyRadius * movement::kBodyBevel + 0.01f);
    const float maxLift = airborne ? 0.0f : movement::kMaxStepUp;
    thread_local std::vector<movement::BodyTriangle> bodyTriangles;
    bodyTriangles.clear();
    appendBodyCollisionTriangles(
        glm::min(from, to) - glm::vec3(bodyRadius + 0.25f, bodyRadius + 0.25f, 0.25f),
        glm::max(from, to) + glm::vec3(bodyRadius + 0.25f, bodyRadius + 0.25f, bodyHeight + maxLift + 0.25f),
        bodyTriangles);
    if (bodyTriangles.empty()) return false;

    if (airborne) {
        return movement::moveBody(from, to, bodyRadius, bodyHeight, bodyTriangles, adjustedPos, false);
    }

    const glm::vec3 horizontal(to.x, to.y, from.z);
    glm::vec3 horizontalAdjusted = horizontal;
    bool collided = false;
    if (glm::dot(glm::vec2(horizontal - from), glm::vec2(horizontal - from)) > 1e-12f) {
        collided = movement::moveBody(from, horizontal, bodyRadius, bodyHeight,
                                      bodyTriangles, horizontalAdjusted, false);
        if (collided) {
            const glm::vec3 stepped = movement::stepBodyAcross(
                from, horizontal, bodyRadius, bodyHeight, bodyTriangles, movement::kMaxStepUp);
            const glm::vec2 intent(horizontal - from);
            if (glm::dot(glm::vec2(stepped - from), intent) >
                glm::dot(glm::vec2(horizontalAdjusted - from), intent) + 1e-6f) {
                horizontalAdjusted = stepped;
                // A step that carried virtually the whole horizontal move is a
                // stair/curb climb rather than a blocking wall.
                collided = glm::distance(glm::vec2(stepped), glm::vec2(horizontal)) > 1e-3f;
            }
        }
    }
    if (horizontalAdjusted.z > from.z + 1e-5f && to.z <= from.z) {
        adjustedPos = horizontalAdjusted;
        return collided;
    }
    const glm::vec3 verticalTarget = horizontalAdjusted + glm::vec3(0.0f, 0.0f, to.z - from.z);
    if (std::abs(verticalTarget.z - horizontalAdjusted.z) > 1e-6f) {
        glm::vec3 verticalAdjusted = verticalTarget;
        collided |= movement::moveBody(horizontalAdjusted, verticalTarget, bodyRadius, bodyHeight,
                                       bodyTriangles, verticalAdjusted, true);
        adjustedPos = verticalAdjusted;
    } else {
        adjustedPos = horizontalAdjusted;
    }
    return collided;
}

void WMORenderer::updateActiveGroup(float glX, float glY, float glZ) {
    // If active group is still valid, check if player is still inside it
    if (activeGroup_.isValid() && activeGroup_.instanceIdx < instances.size()) {
        const auto& instance = instances[activeGroup_.instanceIdx];
        if (instance.modelId == activeGroup_.modelId) {
            auto it = loadedModels.find(instance.modelId);
            if (it != loadedModels.end()) {
                const ModelData& model = it->second;
                glm::vec3 localPos = glm::vec3(instance.invModelMatrix * glm::vec4(glX, glY, glZ, 1.0f));

                // Still inside active group?
                if (activeGroup_.groupIdx >= 0 && static_cast<size_t>(activeGroup_.groupIdx) < model.groups.size()) {
                    const auto& group = model.groups[activeGroup_.groupIdx];
                    if (!group.unreachable && !group.antiportal &&
                        localPos.x >= group.boundingBoxMin.x && localPos.x <= group.boundingBoxMax.x &&
                        localPos.y >= group.boundingBoxMin.y && localPos.y <= group.boundingBoxMax.y &&
                        localPos.z >= group.boundingBoxMin.z && localPos.z <= group.boundingBoxMax.z) {
                        return;  // Still in same group
                    }
                }

                // Check portal-neighbor groups
                for (uint32_t ngi : activeGroup_.neighborGroups) {
                    if (ngi < model.groups.size()) {
                        const auto& group = model.groups[ngi];
                        if (!group.unreachable && !group.antiportal &&
                            localPos.x >= group.boundingBoxMin.x && localPos.x <= group.boundingBoxMax.x &&
                            localPos.y >= group.boundingBoxMin.y && localPos.y <= group.boundingBoxMax.y &&
                            localPos.z >= group.boundingBoxMin.z && localPos.z <= group.boundingBoxMax.z) {
                            // Moved to a neighbor group - update
                            activeGroup_.groupIdx = static_cast<int32_t>(ngi);
                            // Rebuild neighbors for new group
                            activeGroup_.neighborGroups.clear();
                            if (ngi < model.groupPortalRefs.size()) {
                                auto [portalStart, portalCount] = model.groupPortalRefs[ngi];
                                for (uint16_t pi = 0; pi < portalCount; pi++) {
                                    uint16_t refIdx = portalStart + pi;
                                    if (refIdx < model.portalRefs.size()) {
                                        uint32_t tgt = model.portalRefs[refIdx].groupIndex;
                                        if (tgt < model.groups.size()) {
                                            activeGroup_.neighborGroups.push_back(tgt);
                                        }
                                    }
                                }
                            }
                            return;
                        }
                    }
                }
            }
        }
    }

    // Full scan: find which instance/group contains the player
    activeGroup_.invalidate();

    glm::vec3 queryMin(glX - 0.5f, glY - 0.5f, glZ - 0.5f);
    glm::vec3 queryMax(glX + 0.5f, glY + 0.5f, glZ + 0.5f);
    gatherCandidates(queryMin, queryMax, tl_candidateScratch);

    for (size_t idx : tl_candidateScratch) {
        const auto& instance = instances[idx];
        if (!withinWorldBounds(instance, glX, glY, glZ)) {
            continue;
        }

        auto it = loadedModels.find(instance.modelId);
        if (it == loadedModels.end()) continue;

        const ModelData& model = it->second;
        glm::vec3 localPos = glm::vec3(instance.invModelMatrix * glm::vec4(glX, glY, glZ, 1.0f));

        int gi = findContainingGroup(model, localPos);
        if (gi >= 0) {
            activeGroup_.instanceIdx = static_cast<uint32_t>(idx);
            activeGroup_.modelId = instance.modelId;
            activeGroup_.groupIdx = gi;

            // Build neighbor list from portal refs
            activeGroup_.neighborGroups.clear();
            uint32_t groupIdx = static_cast<uint32_t>(gi);
            if (groupIdx < model.groupPortalRefs.size()) {
                auto [portalStart, portalCount] = model.groupPortalRefs[groupIdx];
                for (uint16_t pi = 0; pi < portalCount; pi++) {
                    uint16_t refIdx = portalStart + pi;
                    if (refIdx < model.portalRefs.size()) {
                        uint32_t tgt = model.portalRefs[refIdx].groupIndex;
                        if (tgt < model.groups.size()) {
                            activeGroup_.neighborGroups.push_back(tgt);
                        }
                    }
                }
            }
            return;
        }
    }
}

/// Whether a collision focus is set and this instance falls outside it.
///
/// Five queries ask this before they look at an instance at all, and each had
/// its own copy. The focus is what keeps a raycast from walking every building
/// in the zone when the caller only cares about what is near the player.
/// MOGP flag 0x2000: the group is indoors. It is what separates a
/// building's inside from the porch and the roof, which are groups of the
/// same model, and so what decides whether the sky is still drawn.
constexpr uint32_t kWMOGroupIndoor = 0x2000;

bool WMORenderer::outsideCollisionFocus(const WMOInstance& instance) const {
    return collisionFocus.excludes(instance.worldBoundsMin,
                                   instance.worldBoundsMax);
}

/// Whether a point is inside an instance's world bounds, with the Z window
/// widened by the margins the caller asks for.
///
/// The X and Y test is the same everywhere; the Z margins are not, and that is
/// why they are arguments rather than a constant. Three different pairs are in
/// use: none for the containment queries, the model-dependent pair the floor
/// query computes, and a fixed 2 down by 4 up. Two call sites spell out
/// numbers that match the two branches of the model-dependent rule, which may
/// mean they were copied from it before it learned about low platforms.
bool WMORenderer::withinWorldBounds(const WMOInstance& instance,
                                    float glX, float glY, float glZ,
                                    float zMarginDown, float zMarginUp) const {
    return glX >= instance.worldBoundsMin.x && glX <= instance.worldBoundsMax.x &&
           glY >= instance.worldBoundsMin.y && glY <= instance.worldBoundsMax.y &&
           glZ >= instance.worldBoundsMin.z - zMarginDown &&
           glZ <= instance.worldBoundsMax.z + zMarginUp;
}

/// Whether a point is inside any of a WMO's groups.
///
/// Asked two ways: of every group, which is what tells the client it is under
/// a roof at all, and of interior groups only, which is what decides whether
/// the sky and the outdoor lighting are drawn. The two differed by one line
/// and each had its own copy of the walk: the candidate gather, the focus
/// filter, the world-bounds reject, the per-group world-bounds pre-check and
/// the transform into model space.
///
/// The pre-check is not redundant with the transform below it. Inverting the
/// model matrix and multiplying is the expensive part, and a building whose
/// own bounds contain the point usually has no *group* that does.
bool WMORenderer::isInsideWMOGroups(float glX, float glY, float glZ,
                                    bool interiorOnly, uint32_t* outModelId) const {
    const glm::vec3 queryMin(glX - 0.5f, glY - 0.5f, glZ - 0.5f);
    const glm::vec3 queryMax(glX + 0.5f, glY + 0.5f, glZ + 0.5f);
    gatherCandidates(queryMin, queryMax, tl_candidateScratch);

    for (size_t idx : tl_candidateScratch) {
        const auto& instance = instances[idx];
        if (outsideCollisionFocus(instance)) continue;
        if (!withinWorldBounds(instance, glX, glY, glZ)) continue;

        auto it = loadedModels.find(instance.modelId);
        if (it == loadedModels.end()) continue;
        const ModelData& model = it->second;

        bool anyGroupContains = false;
        for (size_t gi = 0; gi < model.groups.size() && gi < instance.worldGroupBounds.size(); ++gi) {
            const auto& [gMin, gMax] = instance.worldGroupBounds[gi];
            if (glX >= gMin.x && glX <= gMax.x &&
                glY >= gMin.y && glY <= gMax.y &&
                glZ >= gMin.z && glZ <= gMax.z) {
                anyGroupContains = true;
                break;
            }
        }
        if (!anyGroupContains) continue;

        const glm::vec3 localPos =
            glm::vec3(instance.invModelMatrix * glm::vec4(glX, glY, glZ, 1.0f));
        for (const auto& group : model.groups) {
            if (group.unreachable || group.antiportal) continue;
            if (interiorOnly && !(group.groupFlags & kWMOGroupIndoor)) continue;
            if (localPos.x >= group.boundingBoxMin.x && localPos.x <= group.boundingBoxMax.x &&
                localPos.y >= group.boundingBoxMin.y && localPos.y <= group.boundingBoxMax.y &&
                localPos.z >= group.boundingBoxMin.z && localPos.z <= group.boundingBoxMax.z) {
                if (outModelId) *outModelId = instance.modelId;
                return true;
            }
        }
    }
    return false;
}

bool WMORenderer::isInsideWMO(float glX, float glY, float glZ, uint32_t* outModelId) const {
    QueryTimer timer(&queryTimeMs, &queryCallCount);
    return isInsideWMOGroups(glX, glY, glZ, /*interiorOnly=*/false, outModelId);
}

bool WMORenderer::isInsideInteriorWMO(float glX, float glY, float glZ) const {
    return getInteriorLocation(glm::vec3(glX, glY, glZ)).has_value();
}

std::optional<WMOFogSample> WMORenderer::getFog(
    const glm::vec3& position, bool underwater,
    std::optional<WMOFogSample> outdoorFallback) const {
    const auto location = getInteriorLocation(position);
    if (!location) return std::nullopt;
    const auto instance = std::find_if(instances.begin(), instances.end(),
        [&](const auto& value) { return value.id == location->instanceId; });
    assert(instance != instances.end());
    const auto& model = loadedModels.at(instance->modelId);
    const auto& group = model.groups[location->groupIndex];
    // An indoor group with MOGP 0x40 is lit as outdoors, and fogged as
    // outdoors too: Light.dbc's fog, not MFOG. Black Temple's courtyard
    // (blacktemple.wmo group 8, 0x43a41, open to the sky) took MFOG record 0,
    // a teal (35,190,152) sphere around the sewers 600 yards away, and the
    // facades and far walls past its 417-yard end drew as flat teal shapes;
    // the original fogs them into Light 592's dark green (LightParams 604).
    if (!pipeline::wmoUsesLocalLighting(group.groupFlags)) return std::nullopt;
    return sampleWMOFog(
        model.fogs, group.fogIndices, location->localPosition, underwater, outdoorFallback);
}

namespace {

void mergeWmoLightRefs(DirectionalLightRig& rig,
                       const std::vector<pipeline::WMOLight>& lights,
                       const std::vector<uint16_t>& lightRefs,
                       const glm::mat4& wmoModelMatrix,
                       const glm::vec3& sampleWorldPos) {
    for (uint16_t lightIdx : lightRefs) {
        if (lightIdx >= lights.size()) continue;
        const auto& L = lights[lightIdx];
        if (L.attenuationEnd <= 0.0f || L.intensity <= 0.0f) continue;
        const glm::vec3 lightWorldPos(wmoModelMatrix * glm::vec4(L.position, 1.0f));
        const glm::vec3 delta = sampleWorldPos - lightWorldPos;
        const float dist = glm::length(delta);
        if (dist >= L.attenuationEnd) continue;
        const float att = L.useAttenuation
            ? glm::clamp((L.attenuationEnd - dist) /
                             std::max(L.attenuationEnd - L.attenuationStart, 1e-4f),
                         0.0f, 1.0f)
            : 1.0f;
        const glm::vec3 dColor = glm::vec3(L.color) * (L.intensity * att);
        const glm::vec3 lDir = dist > 1e-5f ? (delta / dist) : kLegacyIndoorLightDir;
        rig = mergeDirectionalRig(rig, {.ambient = glm::vec3(0.0f), .diffuse = dColor, .dir = lDir});
    }
}

} // namespace

std::optional<WMORenderer::FloorLightSample> WMORenderer::sampleFloorLight(
        const glm::vec3& position, float sampleHeightOffset, bool isUnit) const {
    // From a little above the feet, so the floor stood on is found and a
    // ceiling just overhead is not.
    const glm::vec3 origin = position + glm::vec3(0.0f, 0.0f, 0.5f);
    gatherCandidates(origin - glm::vec3(0.5f, 0.5f, 500.0f), origin + glm::vec3(0.5f), tl_candidateScratch);
    float bestWorldDist = std::numeric_limits<float>::max();
    std::optional<FloorLightSample> bestSample;
    const ModelData* bestModel = nullptr;
    const GroupResources* bestGroup = nullptr;
    const WMOInstance* bestInstance = nullptr;

    for (size_t index : tl_candidateScratch) {
        const auto& instance = instances[index];
        if (instance.hidden) continue;
        const auto modelIt = loadedModels.find(instance.modelId);
        if (modelIt == loadedModels.end()) continue;
        const auto& model = modelIt->second;
        const glm::vec3 local(instance.invModelMatrix * glm::vec4(origin, 1.0f));
        const glm::vec3 down = glm::normalize(glm::vec3(instance.invModelMatrix * glm::vec4(0, 0, -1, 0)));
        for (const auto& group : model.groups) {
            // An interior lights only what stands within its box: Undercity's
            // floors lie under Tirisfal's ruins, and a unit up on the terrain
            // is not in them.
            if (!group.collisionColors.empty() &&
                (glm::any(glm::lessThan(local, group.boundingBoxMin)) ||
                 glm::any(glm::greaterThan(local, group.boundingBoxMax)))) continue;
            if (!trianglesAlongRay(group, local, down, tl_triScratch)) continue;
            const auto& indices = group.collisionIndices;
            const auto& vertices = group.collisionVertices;
            for (uint32_t triangle : tl_triScratch) {
                assert(triangle / 3 < group.triMaterials.size() &&
                       "WMO group without one MOPY material per triangle");
                // Collision-only triangles (MOPY material 0xFF) carry white MOCV
                // on invisible walking ramps (e.g. Scholomance Ossuary stairs).
                if (group.triMaterials[triangle / 3] == 0xFF) continue;
                const glm::vec3& v0 = vertices[indices[triangle]];
                const glm::vec3& v1 = vertices[indices[triangle + 1]];
                const glm::vec3& v2 = vertices[indices[triangle + 2]];
                const float distance = rayTriangleIntersect(local, down, v0, v1, v2);
                if (distance <= 0.0f) continue;
                const glm::vec3 hit = local + down * distance;
                const glm::vec3 worldHit(instance.modelMatrix * glm::vec4(hit, 1.0f));
                const float worldDist = origin.z - worldHit.z;
                if (worldDist <= 0.0f || worldDist >= bestWorldDist) continue;
                bestWorldDist = worldDist;

                if (group.collisionColors.empty()) {
                    bestSample = FloorLightSample{
                        .floorZ = worldHit.z,
                        .isInterior = false,
                    };
                    bestModel = nullptr;
                    bestGroup = nullptr;
                    bestInstance = nullptr;
                    continue;
                }
                // Barycentric weights of the hit point.
                const glm::vec3 e1 = v1 - v0, e2 = v2 - v0, d = hit - v0;
                const float d11 = glm::dot(e1, e1), d12 = glm::dot(e1, e2), d22 = glm::dot(e2, e2);
                const float dp1 = glm::dot(d, e1), dp2 = glm::dot(d, e2);
                const float denom = d11 * d22 - d12 * d12;
                if (denom == 0.0f) continue;
                const float b1 = (d22 * dp1 - d12 * dp2) / denom;
                const float b2 = (d11 * dp2 - d12 * dp1) / denom;
                const auto& colors = group.collisionColors;
                const glm::vec4 mocv = colors[indices[triangle]] * (1.0f - b1 - b2) +
                                       colors[indices[triangle + 1]] * b1 +
                                       colors[indices[triangle + 2]] * b2;
                const glm::vec3 mocvRgb = glm::clamp(glm::vec3(mocv), 0.0f, 1.0f);
                bestSample = FloorLightSample{
                    .floorZ = worldHit.z,
                    .isInterior = true,
                    .exteriorBlend = glm::clamp(mocv.a, 0.0f, 1.0f),
                    .mocv = mocvRgb,
                    .indoorRig = isUnit ? indoorUnitRig(mocvRgb) : indoorDoodadRig(mocvRgb),
                };
                bestModel = &model;
                bestGroup = &group;
                bestInstance = &instance;
            }
        }
    }

    if (bestSample && bestSample->isInterior && bestModel && bestGroup && bestInstance &&
        !bestGroup->lightRefs.empty()) {
        const glm::vec3 samplePos = position + glm::vec3(0.0f, 0.0f, sampleHeightOffset);
        mergeWmoLightRefs(bestSample->indoorRig, bestModel->lights, bestGroup->lightRefs,
                          bestInstance->modelMatrix, samplePos);
    }
    return bestSample;
}

std::optional<glm::vec3> WMORenderer::interiorObjectLight(const glm::vec3& position) const {
    const auto sample = sampleFloorLight(position, 1.19f, false);
    if (!sample || !sample->isInterior) return std::nullopt;
    return sample->mocv;
}

DirectionalLightRig WMORenderer::resolveDoodadLightRig(
        const pipeline::WMOModel& wmoModel,
        uint32_t doodadIdx,
        const glm::mat4& wmoWorldMatrix,
        const glm::vec3& sampleWorldPos) {
    const glm::vec3 moddColor = doodadIdx < wmoModel.doodads.size()
        ? glm::clamp(glm::vec3(wmoModel.doodads[doodadIdx].color), 0.0f, 1.0f)
        : glm::vec3(1.0f);
    DirectionalLightRig rig = indoorDoodadRig(moddColor);
    const auto lightRefs = pipeline::wmoDoodadLightRefs(wmoModel, doodadIdx);
    if (!lightRefs.empty() && !wmoModel.lights.empty()) {
        mergeWmoLightRefs(rig, wmoModel.lights, lightRefs, wmoWorldMatrix, sampleWorldPos);
    }
    return rig;
}

std::optional<WMORenderer::SealedInterior> WMORenderer::findSealedInterior(const glm::vec3& position) const {
    const auto location = getInteriorLocation(position);
    if (!location) return std::nullopt;
    const auto instance = std::find_if(instances.begin(), instances.end(),
        [&](const auto& value) { return value.id == location->instanceId; });
    assert(instance != instances.end());
    const auto& model = loadedModels.at(instance->modelId);
    constexpr uint32_t kOutdoor = 0x8;
    std::vector<uint32_t> reached{location->groupIndex};
    std::vector<bool> seen(model.groups.size(), false);
    seen[location->groupIndex] = true;
    for (size_t next = 0; next < reached.size(); ++next) {
        const uint32_t group = reached[next];
        if (model.groups[group].groupFlags & kOutdoor) return std::nullopt;
        if (group >= model.groupPortalRefs.size()) continue;
        const auto [start, count] = model.groupPortalRefs[group];
        for (uint32_t r = start; r < static_cast<uint32_t>(start) + count && r < model.portalRefs.size(); ++r) {
            const uint32_t other = model.portalRefs[r].groupIndex;
            if (other < seen.size() && !seen[other]) {
                seen[other] = true;
                reached.push_back(other);
            }
        }
    }
    SealedInterior sealed;
    sealed.instanceId = instance->id;
    sealed.groups = reached;
    sealed.worldMin = glm::vec3(std::numeric_limits<float>::max());
    sealed.worldMax = glm::vec3(std::numeric_limits<float>::lowest());
    for (uint32_t group : reached) {
        assert(group < instance->worldGroupBounds.size());
        sealed.worldMin = glm::min(sealed.worldMin, instance->worldGroupBounds[group].first);
        sealed.worldMax = glm::max(sealed.worldMax, instance->worldGroupBounds[group].second);
    }
    return sealed;
}

std::optional<uint32_t> WMORenderer::findHiddenExteriorInterior(const Camera& camera) const {
    const glm::vec3 camPos = camera.getPosition();
    const auto location = getInteriorLocation(camPos, /*indoorOnly=*/true);
    if (!location) return std::nullopt;
    const auto instance = std::find_if(instances.begin(), instances.end(),
        [&](const auto& value) { return value.id == location->instanceId; });
    assert(instance != instances.end());
    const auto modelIt = loadedModels.find(instance->modelId);
    if (modelIt == loadedModels.end()) return std::nullopt;
    const auto& model = modelIt->second;
    if (location->groupIndex >= model.groups.size()) return std::nullopt;
    if (wmoGroupSeesExterior(model.groups[location->groupIndex].groupFlags)) return std::nullopt;

    std::vector<PortalGroupInfo> groups;
    groups.reserve(model.groups.size());
    for (size_t gi = 0; gi < model.groups.size(); ++gi) {
        PortalGroupInfo info{};
        info.groupFlags = model.groups[gi].groupFlags;
        if (gi < model.groupPortalRefs.size()) {
            info.portalStart = model.groupPortalRefs[gi].first;
            info.portalCount = model.groupPortalRefs[gi].second;
        }
        groups.push_back(info);
    }

    std::vector<PortalPlaneInfo> portals;
    portals.reserve(model.portals.size());
    for (const auto& p : model.portals) {
        portals.push_back({p.startVertex, p.vertexCount, p.normal, p.distance});
    }

    std::vector<PortalRefInfo> refs;
    refs.reserve(model.portalRefs.size());
    for (const auto& r : model.portalRefs) {
        refs.push_back({r.portalIndex, r.groupIndex, r.side});
    }

    Frustum frustum;
    const glm::mat4 viewProj = camera.getProjectionMatrix() * camera.getViewMatrix();
    frustum.extractFromMatrix(viewProj);
    const glm::vec3 localCam(instance->invModelMatrix * glm::vec4(camPos, 1.0f));

    if (isExteriorVisibleThroughPortals(groups, portals, model.portalVertices, refs,
                                        location->groupIndex, localCam, frustum,
                                        viewProj, instance->modelMatrix)) {
        return std::nullopt;
    }
    return instance->id;
}

std::optional<WMORenderer::InteriorLocation> WMORenderer::getInteriorLocation(const glm::vec3& position, bool indoorOnly) const {
    // The group a point is in is the one whose surface lies nearest below it,
    // among the groups whose box the vertical through the point crosses.
    //
    // Indoor classification (indoorOnly) used to be bounding-box containment,
    // smallest box first. Boxes overlap: Black Temple's "Den of Mortal
    // Delights" (indoor, 0x2000) has a box from z 65 to 444 that swallows the
    // outdoor ramp from Supremus up to the Grand Promenade (group 3 "maingate",
    // outdoor). Standing on that ramp counted as inside the Den, a sealed
    // interior, so terrain and sky were not drawn and the minimap went black,
    // where build 8606 draws the Illidari Training Grounds around it. The Lion's
    // Pride Inn's porch and roof lie in its kitchen's box, and are outdoors in
    // 8606 as well (IsIndoors() nil, terrain minimap).
    //
    // Indoor queries additionally need the point inside some group box of the
    // instance, so a point above a building is not inside it. The surface
    // need not belong to a group whose box holds the point: a camera high in
    // Ironforge's Great Forge is over a small room's floor whose box ends
    // below it, and is still indoors.
    gatherCandidates(position - glm::vec3(0.5f, 0.5f, 500.0f),
                     position + glm::vec3(0.5f), tl_candidateScratch);
    std::optional<InteriorLocation> result;
    bool resultIndoor = false;
    float bestDistance = std::numeric_limits<float>::max();
    bool bestBackface = true;
    for (size_t index : tl_candidateScratch) {
        const auto& instance = instances[index];
        const auto modelIt = loadedModels.find(instance.modelId);
        if (modelIt == loadedModels.end()) continue;
        const auto& model = modelIt->second;
        const glm::vec3 local(instance.invModelMatrix * glm::vec4(position, 1));
        // Indoor queries require the point to lie within the actual vertex
        // bounds of at least one indoor group (MOGP 0x2000). Using all groups
        // (including outdoor groups) or bloated authored MOGP bounds lets an
        // outdoor camera above an open stairwell (e.g. md_cryptonerm group 0
        // over group 1) or above an underground crypt (md_cryptonerm group 4,
        // MOGP zMax +7.37 vs vertex zMax -7.33) misclassify as indoor.
        //
        // Furthermore, open-air city districts such as Orgrimmar's Valley of
        // Strength (ogrimmar.wmo group 134, 0x0a841) and Stormwind's streets
        // (0x0a040) carry MOGP 0x2000 alongside exterior-lit 0x40 / show-sky
        // 0x100. Track whether `local` is inside any strictly enclosed indoor
        // group (`!wmoGroupSeesExterior`) so an outdoor camera over an open
        // stairwell (e.g. Wind Rider Tower group 108, zMax 30.811) cannot be
        // claimed by the enclosed room below it.
        bool inAnyIndoorBox = false;
        bool inAnyEnclosedIndoorBox = false;
        for (const auto& group : model.groups) {
            if (group.unreachable || group.antiportal) continue;
            if ((group.groupFlags & kWMOGroupIndoor) == 0) continue;
            if (glm::all(glm::greaterThanEqual(local, group.geometryMin)) &&
                glm::all(glm::lessThanEqual(local, group.geometryMax))) {
                inAnyIndoorBox = true;
                if (!wmoGroupSeesExterior(group.groupFlags)) {
                    inAnyEnclosedIndoorBox = true;
                }
            }
        }
        if (indoorOnly && !inAnyIndoorBox) {
            continue;
        }
        const glm::vec3 down(instance.invModelMatrix * glm::vec4(0, 0, -1, 0));
        for (size_t groupIndex = 0; groupIndex < model.groups.size(); ++groupIndex) {
            const auto& group = model.groups[groupIndex];
            const bool inGroupBox =
                glm::all(glm::greaterThanEqual(local, group.geometryMin)) &&
                glm::all(glm::lessThanEqual(local, group.geometryMax));
            if (!canDownwardHitClassifyWmoGroup(group.groupFlags, inGroupBox, inAnyEnclosedIndoorBox)) {
                continue;
            }
            if (!rayIntersectsAABB(local, down, group.geometryMin, group.geometryMax)) continue;
            if (!trianglesAlongRay(group, local, down, tl_triScratch)) continue;
            float distance = std::numeric_limits<float>::max();
            bool groupBackface = true;
            for (uint32_t triangle : tl_triScratch) {
                const auto& indices = group.collisionIndices;
                const auto& vertices = group.collisionVertices;
                const glm::vec3& v0 = vertices[indices[triangle]];
                const glm::vec3& v1 = vertices[indices[triangle + 1]];
                const glm::vec3& v2 = vertices[indices[triangle + 2]];
                const float t = rayTriangleIntersect(local, down, v0, v1, v2);
                if (t <= 0.0f) continue;
                const bool backface = isDownwardHitInteriorCeilingBackface(v0, v1, v2, down);
                if (preferDownwardSurfaceHit(t, backface, distance, groupBackface)) {
                    distance = t;
                    groupBackface = backface;
                }
            }
            if (distance == std::numeric_limits<float>::max()) continue;
            if (!preferDownwardSurfaceHit(distance, groupBackface, bestDistance, bestBackface)) continue;
            bestDistance = distance;
            bestBackface = groupBackface;
            resultIndoor = (group.groupFlags & kWMOGroupIndoor) != 0;
            result = InteriorLocation{model.rootId, instance.nameSet, group.groupId,
                static_cast<uint32_t>(groupIndex), instance.id, model.sourcePath,
                local, group.boundingBoxMin, group.boundingBoxMax, instance.modelMatrix,
                position.z - distance, group.groupFlags};
        }
    }
    // If the nearest surface below is the back-face of an interior-only room's
    // ceiling (normal pointing down into the room, away from position), the
    // point is outside above that room (e.g. on outdoor terrain over a crypt).
    if (result && bestBackface && wmoGroupIsIndoorOnly(result->groupFlags)) {
        return std::nullopt;
    }
    if (indoorOnly && !resultIndoor) return std::nullopt;
    return result;
}

std::vector<WMORenderer::InteriorMapGroup> WMORenderer::getInteriorMapGroups(const InteriorLocation& location) const {
    const auto instance = std::find_if(instances.begin(), instances.end(),
        [&](const auto& value) { return value.id == location.instanceId; });
    assert(instance != instances.end());
    const auto& model = loadedModels.at(instance->modelId);
    std::vector<InteriorMapGroup> groups;
    for (size_t index = 0; index < model.groups.size(); ++index) {
        const auto& group = model.groups[index];
        if (!(group.groupFlags & kWMOGroupIndoor)) continue;
        groups.push_back({group.authoredIndex, group.authoredMin, group.authoredMax});
    }
    // Height controls overlap order, not whether a room exists on the map.
    // Stairs can place the player above every corridor/cell ceiling nearby.
    // Draw distant vertical layers first so the occupied layer wins where
    // authored room textures overlap, without deleting nonoverlapping rooms.
    const auto verticalDistance = [&](const InteriorMapGroup& group) {
        return std::max({group.min.z - location.localPosition.z,
                         location.localPosition.z - group.max.z, 0.0f});
    };
    std::stable_sort(groups.begin(), groups.end(), [&](const auto& a, const auto& b) {
        return verticalDistance(a) > verticalDistance(b);
    });
    return groups;
}

float WMORenderer::raycastBoundingBoxes(const glm::vec3& origin, const glm::vec3& direction, float maxDistance) const {
    return raycastTriangles(origin, direction, maxDistance, false, false);
}

float WMORenderer::raycastNonTransportBoundingBoxes(const glm::vec3& origin, const glm::vec3& direction, float maxDistance) const {
    return raycastTriangles(origin, direction, maxDistance, false, true);
}

bool WMORenderer::isDisplacedByTransportHull(const glm::vec3& position) const {
    for (const auto& instance : instances) {
        if (!instance.isTransport || instance.hidden) continue;
        if (position.x < instance.worldBoundsMin.x - 0.5f || position.x > instance.worldBoundsMax.x + 0.5f ||
            position.y < instance.worldBoundsMin.y - 0.5f || position.y > instance.worldBoundsMax.y + 0.5f ||
            position.z < instance.worldBoundsMin.z - 0.5f || position.z > instance.worldBoundsMax.z + 0.5f) {
            continue;
        }

        const auto modelIt = loadedModels.find(instance.modelId);
        if (modelIt == loadedModels.end()) continue;
        const ModelData& model = modelIt->second;

        const glm::vec3 worldOrigin(position.x, position.y, instance.worldBoundsMax.z + 1.0f);
        const glm::vec3 worldDir(0.0f, 0.0f, -1.0f);
        const glm::vec3 localOrigin(instance.invModelMatrix * glm::vec4(worldOrigin, 1.0f));
        const glm::vec3 localDir = glm::normalize(
            glm::vec3(instance.invModelMatrix * glm::vec4(worldDir, 0.0f)));

        bool hasHit = false;
        float bestHitZ = -std::numeric_limits<float>::infinity();
        float bestSignedWorldNormalZ = -1.0f;

        for (size_t gi = 0; gi < model.groups.size(); ++gi) {
            if (gi < instance.worldGroupBounds.size()) {
                const auto& [gMin, gMax] = instance.worldGroupBounds[gi];
                if (position.x < gMin.x - 0.1f || position.x > gMax.x + 0.1f ||
                    position.y < gMin.y - 0.1f || position.y > gMax.y + 0.1f ||
                    position.z + 0.5f < gMin.z) {
                    continue;
                }
            }

            const auto& group = model.groups[gi];
            if (!rayIntersectsAABB(localOrigin, localDir,
                                   group.boundingBoxMin, group.boundingBoxMax)) {
                continue;
            }
            if (!trianglesAlongRay(group, localOrigin, localDir, tl_triScratch)) continue;

            const auto& verts = group.collisionVertices;
            const auto& indices = group.collisionIndices;
            for (uint32_t triStart : tl_triScratch) {
                const uint32_t triIdx = triStart / 3;
                if (triIdx >= group.triNormals.size()) continue;
                const glm::vec3 localNormal = group.triNormals[triIdx];
                if (glm::dot(localNormal, localNormal) < 0.5f) continue;

                const glm::vec3& v0 = verts[indices[triStart]];
                const glm::vec3& v1 = verts[indices[triStart + 1]];
                const glm::vec3& v2 = verts[indices[triStart + 2]];

                float t = rayTriangleIntersect(localOrigin, localDir, v0, v1, v2);
                if (t <= 0.0f) {
                    t = rayTriangleIntersect(localOrigin, localDir, v0, v2, v1);
                }
                if (t <= 0.0f) continue;

                const glm::vec3 hitLocal = localOrigin + localDir * t;
                const glm::vec3 hitWorld(instance.modelMatrix * glm::vec4(hitLocal, 1.0f));
                if (hitWorld.z > position.z + 0.5f) continue;
                if (!hasHit || hitWorld.z > bestHitZ) {
                    hasHit = true;
                    bestHitZ = hitWorld.z;
                    const glm::vec3 worldNormal = glm::normalize(
                        glm::vec3(instance.modelMatrix * glm::vec4(localNormal, 0.0f)));
                    bestSignedWorldNormalZ = worldNormal.z;
                }
            }
        }

        if (hasHit && bestSignedWorldNormalZ >= 0.5f) {
            return true;
        }
    }
    return false;
}

float WMORenderer::raycastCamera(const glm::vec3& origin, const glm::vec3& direction, float maxDistance) const {
    return raycastTriangles(origin, direction, maxDistance, true, false);
}

float WMORenderer::raycastTriangles(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
                                    bool camera, bool skipTransports) const {
    QueryTimer timer(&queryTimeMs, &queryCallCount);
    float closestHit = maxDistance;
    // The camera is a solid sphere and every solid surface stops it: floors and
    // ceilings as much as walls. This used to consider the wall list alone,
    // which is pre-filtered to |normal.z| <= 0.65, and then narrowed that to
    // 0.20 again on the way past - so a storey's floor, a vaulted ceiling and
    // anything more than a few degrees off vertical simply was not there, and
    // pitching the camera down through the floor of an upper room or up through
    // a roof met nothing at all.
    //
    // Nothing replaces the normal test. The floor underfoot is only crossed by
    // this ray when the camera is actually being pushed down through it, which
    // is the case where it has to stop.
    //
    // Ignore whatever the pivot is standing in, though: the ray starts inside
    // the player, so a surface within a hand's width would otherwise jam the
    // camera at the minimum.
    constexpr float MIN_HIT_DISTANCE = 0.20f;
    // Only surfaces the ray is very nearly parallel to are dropped. This was
    // 0.25, which is a 75-degree cone: a wall approached at a shallow angle -
    // exactly when the camera is sliding along it and about to pass through -
    // failed the test and was not collided with at all.
    constexpr float MIN_SURFACE_ALIGNMENT = 0.05f;

    glm::vec3 rayEnd = origin + direction * maxDistance;
    glm::vec3 queryMin = glm::min(origin, rayEnd) - glm::vec3(1.0f);
    glm::vec3 queryMax = glm::max(origin, rayEnd) + glm::vec3(1.0f);
    gatherCandidates(queryMin, queryMax, tl_candidateScratch);

    for (size_t idx : tl_candidateScratch) {
        const auto& instance = instances[idx];
        if (skipTransports && instance.isTransport) continue;
        if (outsideCollisionFocus(instance)) continue;

        glm::vec3 center = (instance.worldBoundsMin + instance.worldBoundsMax) * 0.5f;
        glm::vec3 halfExtent = instance.worldBoundsMax - center;
        float radiusSq = glm::dot(halfExtent, halfExtent);
        glm::vec3 toCenter = center - origin;
        float distSq = glm::dot(toCenter, toCenter);
        float maxR = maxDistance + std::sqrt(radiusSq) + 1.0f;
        if (distSq > maxR * maxR) {
            continue;
        }

        glm::vec3 worldMin = instance.worldBoundsMin - glm::vec3(0.5f);
        glm::vec3 worldMax = instance.worldBoundsMax + glm::vec3(0.5f);
        if (!rayIntersectsAABB(origin, direction, worldMin, worldMax)) {
            continue;
        }

        auto it = loadedModels.find(instance.modelId);
        if (it == loadedModels.end()) continue;

        const ModelData& model = it->second;

        // Use cached inverse matrix
        glm::vec3 localOrigin = glm::vec3(instance.invModelMatrix * glm::vec4(origin, 1.0f));
        glm::vec3 localDir = glm::normalize(glm::vec3(instance.invModelMatrix * glm::vec4(direction, 0.0f)));

        for (size_t gi = 0; gi < model.groups.size(); ++gi) {
            // World-space group cull - skip groups whose world AABB doesn't intersect the ray
            if (gi < instance.worldGroupBounds.size()) {
                const auto& [gMin, gMax] = instance.worldGroupBounds[gi];
                if (!rayIntersectsAABB(origin, direction, gMin, gMax)) {
                    continue;
                }
            }

            const auto& group = model.groups[gi];
            // Local-space AABB cull
            if (!rayIntersectsAABB(localOrigin, localDir, group.boundingBoxMin, group.boundingBoxMax)) {
                continue;
            }

            // Narrow-phase: triangle raycast using spatial grid (wall-only).
            const auto& verts = group.collisionVertices;
            const auto& indices = group.collisionIndices;

            // Compute local-space ray endpoint and query grid for XY range
            glm::vec3 localEnd = localOrigin + localDir * (closestHit / glm::length(
                glm::vec3(instance.modelMatrix * glm::vec4(localDir, 0.0f))));
            float rMinX = std::min(localOrigin.x, localEnd.x) - 1.0f;
            float rMinY = std::min(localOrigin.y, localEnd.y) - 1.0f;
            float rMaxX = std::max(localOrigin.x, localEnd.x) + 1.0f;
            float rMaxY = std::max(localOrigin.y, localEnd.y) + 1.0f;
            group.getTrianglesInRange(rMinX, rMinY, rMaxX, rMaxY, tl_triScratch);

            for (uint32_t triStart : tl_triScratch) {
                const uint32_t triIdx = triStart / 3;
                if (triIdx < group.triMopyFlags.size()) {
                    const uint8_t mopy = group.triMopyFlags[triIdx];
                    if (camera ? !wmoTriangleStopsCamera(mopy)
                               : !wmoTriangleBlocksMovement(mopy)) {
                        continue;
                    }
                }
                const glm::vec3& v0 = verts[indices[triStart]];
                const glm::vec3& v1 = verts[indices[triStart + 1]];
                const glm::vec3& v2 = verts[indices[triStart + 2]];
                glm::vec3 triNormal = group.triNormals[triStart / 3];
                if (glm::dot(triNormal, triNormal) < 0.5f) continue;  // degenerate
                // Ignore near-grazing intersections that tend to come from ramps/arches
                // and cause camera pull-in even when no meaningful wall is behind the player.
                if (std::abs(glm::dot(triNormal, localDir)) < MIN_SURFACE_ALIGNMENT) {
                    continue;
                }

                float t = rayTriangleIntersect(localOrigin, localDir, v0, v1, v2);
                if (t <= 0.0f) {
                    // Two-sided collision.
                    t = rayTriangleIntersect(localOrigin, localDir, v0, v2, v1);
                }
                if (t <= 0.0f) continue;

                glm::vec3 localHit = localOrigin + localDir * t;
                glm::vec3 worldHit = glm::vec3(instance.modelMatrix * glm::vec4(localHit, 1.0f));
                // Deliberately no height band around the pivot here.
                //
                // There was one - hits more than 0.90 below or 0.80 above the
                // pivot were dropped - and it is what let the camera through
                // walls. The camera orbits and pitches, so the far end of this
                // ray is normally metres above or below the pivot: at any
                // appreciable pitch every hit near the camera fell outside that
                // 1.7-yard slice and the wall was not there at all. The band was
                // aimed at floor and ramp geometry underfoot, which the normal
                // test above already excludes, so it was rejecting only the
                // walls it was supposed to be finding.
                float worldDist = glm::length(worldHit - origin);
                if (worldDist < MIN_HIT_DISTANCE) continue;
                if (worldDist < closestHit && worldDist <= maxDistance) {
                    closestHit = worldDist;
                }
            }
        }
    }

    return closestHit;
}

float WMORenderer::raycastVisibleGeometry(const glm::vec3& origin, const glm::vec3& direction,
                                          float maxDistance) const {
    QueryTimer timer(&queryTimeMs, &queryCallCount);
    float closestHit = maxDistance;
    const glm::vec3 rayEnd = origin + direction * maxDistance;
    gatherCandidates(glm::min(origin, rayEnd) - glm::vec3(1.0f),
                     glm::max(origin, rayEnd) + glm::vec3(1.0f), tl_candidateScratch);
    for (size_t idx : tl_candidateScratch) {
        const auto& instance = instances[idx];
        if (outsideCollisionFocus(instance)) continue;
        if (!rayIntersectsAABB(origin, direction, instance.worldBoundsMin - glm::vec3(0.5f),
                               instance.worldBoundsMax + glm::vec3(0.5f))) continue;
        const auto it = loadedModels.find(instance.modelId);
        if (it == loadedModels.end()) continue;
        // Unnormalized: a local distance along it is the world distance.
        const glm::vec3 localOrigin = glm::vec3(instance.invModelMatrix * glm::vec4(origin, 1.0f));
        const glm::vec3 localDir = glm::vec3(instance.invModelMatrix * glm::vec4(direction, 0.0f));
        const auto& groups = it->second.groups;
        for (size_t gi = 0; gi < groups.size(); ++gi) {
            if (gi < instance.worldGroupBounds.size()) {
                const auto& [gMin, gMax] = instance.worldGroupBounds[gi];
                if (!rayIntersectsAABB(origin, direction, gMin, gMax)) continue;
            }
            const auto& group = groups[gi];
            if (!trianglesAlongRay(group, localOrigin, localDir, tl_triScratch)) continue;
            assert(group.triMaterials.size() * 3 == group.collisionIndices.size() &&
                   "WMO group without one MOPY material per triangle");
            closestHit = pickDrawnWmoTriangles(group.collisionVertices, group.collisionIndices,
                                               group.triMaterials, tl_triScratch,
                                               localOrigin, localDir, closestHit);
        }
    }
    return closestHit;
}

// Occlusion queries stubbed out in Vulkan (were disabled by default anyway)

void WMORenderer::recreatePipelines() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();

    // Destroy old main-pass pipelines (NOT shadow, NOT pipeline layout)
    destroy(device, opaquePipeline_);
    destroy(device, transparentPipeline_);
    destroy(device, wireframePipeline_);
    destroy(device, opaqueCulledPipeline_);
    destroy(device, transparentCulledPipeline_);

    // --- Load shaders ---
    VkShaderModule vertShader, fragShader;
    if (!vertShader.loadFromFile(device, "assets/shaders/wmo.vert.spv") ||
        !fragShader.loadFromFile(device, "assets/shaders/wmo.frag.spv")) {
        core::Logger::getInstance().error("WMORenderer::recreatePipelines: failed to load shaders");
        return;
    }

    buildMainPassPipelines(device, vertShader, fragShader);

    vertShader.destroy();
    fragShader.destroy();

    core::Logger::getInstance().info("WMORenderer: pipelines recreated");
}

void WMORenderer::collectGrassClearings(float minX, float minY, float maxX, float maxY,
                                        std::vector<pipeline::GrassClearingSource>& out) const {
    // How far past a wall the clearing runs, and how far past that the grass
    // takes to stand back up. A building presses harder on its surroundings
    // than a cart does, so these are wider than the M2 numbers.
    constexpr float kClearing = 1.5f;
    constexpr float kEase = 9.0f;
    const float reach = kClearing + kEase;

    for (const auto& inst : instances) {
        if (inst.worldBoundsMax.x + reach < minX || inst.worldBoundsMin.x - reach > maxX ||
            inst.worldBoundsMax.y + reach < minY || inst.worldBoundsMin.y - reach > maxY) {
            continue;
        }
        // Per-group boxes where they exist; the outer bounds only as the
        // fallback. The outer box of a large WMO swallows its courtyards and
        // half its village, and that is where grass belongs.
        if (!inst.worldGroupBounds.empty()) {
            for (const auto& [mn, mx] : inst.worldGroupBounds) {
                if (mx.x + reach < minX || mn.x - reach > maxX ||
                    mx.y + reach < minY || mn.y - reach > maxY) {
                    continue;
                }
                out.push_back({mn.x, mn.y, mx.x, mx.y, kClearing, kEase});
            }
        } else {
            out.push_back({inst.worldBoundsMin.x, inst.worldBoundsMin.y,
                           inst.worldBoundsMax.x, inst.worldBoundsMax.y, kClearing, kEase});
        }
    }
}

} // namespace rendering
} // namespace wowee

namespace wowee {
namespace rendering {

void WMORenderer::registerRtModel(uint32_t modelId, const ModelData& model) {
    if (!rtScene_ || rtModelMeshes_.count(modelId)) return;
    RtScene::MeshSource src;
    for (const auto& group : model.groups) {
        // Groups the renderer never draws would cast shadows the player
        // cannot see the source of.
        if (group.allUntextured || group.antiportal) continue;
        if (group.collisionVertices.empty()) continue;
        const uint32_t base = static_cast<uint32_t>(src.positions.size());
        src.positions.insert(src.positions.end(), group.collisionVertices.begin(),
                             group.collisionVertices.end());
        for (const auto& batch : group.batches) {
            const uint32_t mat = batch.materialId;
            const uint32_t blend = mat < model.materialBlendModes.size() ? model.materialBlendModes[mat] : 0;
            if (blend >= 2) continue;  // blended: glass, light cards, water sheets
            glm::vec3 albedo(0.5f);
            float opacity = 1.0f;
            if (mat < model.materialTextureIndices.size()) {
                const uint32_t ti = model.materialTextureIndices[mat];
                if (ti < model.textures.size() && model.textures[ti]) {
                    albedo = model.textures[ti]->averageColor();
                    if (blend == 1) opacity = model.textures[ti]->alphaCoverage();
                }
            }
            const float surface = packRtSurface(albedo, opacity);
            const uint32_t end = std::min<uint32_t>(batch.startIndex + batch.indexCount,
                                                    static_cast<uint32_t>(group.collisionIndices.size()));
            for (uint32_t i = batch.startIndex; i + 2 < end; i += 3) {
                src.indices.push_back(base + group.collisionIndices[i]);
                src.indices.push_back(base + group.collisionIndices[i + 1]);
                src.indices.push_back(base + group.collisionIndices[i + 2]);
                src.surfaces.push_back(surface);
            }
        }
    }
    const RtScene::MeshId mesh = rtScene_->addMesh(std::move(src));
    if (mesh != RtScene::kInvalid) rtModelMeshes_[modelId] = mesh;
}

void WMORenderer::releaseRtModel(uint32_t modelId) {
    if (!rtScene_) return;
    for (auto it = rtInstances_.begin(); it != rtInstances_.end();) {
        if (it->second.modelId == modelId) {
            rtScene_->removeInstance(it->second.rtId);
            it = rtInstances_.erase(it);
        } else {
            ++it;
        }
    }
    auto mesh = rtModelMeshes_.find(modelId);
    if (mesh == rtModelMeshes_.end()) return;
    rtScene_->removeMesh(mesh->second);
    rtModelMeshes_.erase(mesh);
}

void WMORenderer::syncRtScene() {
    if (!rtScene_ || !rtScene_->isActive()) return;
    const uint64_t gen = ++rtSyncGeneration_;
    for (const auto& inst : instances) {
        if (inst.hidden) continue;
        auto mesh = rtModelMeshes_.find(inst.modelId);
        if (mesh == rtModelMeshes_.end()) continue;
        auto rec = rtInstances_.find(inst.id);
        if (rec == rtInstances_.end()) {
            const uint32_t rtId = rtScene_->addInstance(mesh->second, inst.modelMatrix);
            if (rtId != RtScene::kInvalid) {
                rtInstances_.emplace(inst.id, RtInstanceRecord{rtId, inst.modelId, inst.modelMatrix, gen});
            }
            continue;
        }
        if (rec->second.matrix != inst.modelMatrix) {
            rtScene_->setInstanceTransform(rec->second.rtId, inst.modelMatrix);
            rec->second.matrix = inst.modelMatrix;
        }
        rec->second.seen = gen;
    }
    for (auto it = rtInstances_.begin(); it != rtInstances_.end();) {
        if (it->second.seen != gen) {
            rtScene_->removeInstance(it->second.rtId);
            it = rtInstances_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace rendering
} // namespace wowee
