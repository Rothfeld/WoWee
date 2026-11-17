#include "rendering/footprint_renderer.hpp"
#include <cassert>
#include "pipeline/m2_loader.hpp"

#include "core/logger.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/blp_loader.hpp"
#include "pipeline/dbc_loader.hpp"
#include "rendering/camera.hpp"
#include "rendering/frustum.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/renderer.hpp"
#include "rendering/terrain_manager.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_utils.hpp"
#include "rendering/wmo_renderer.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <unordered_map>

#include <glm/gtc/matrix_transform.hpp>

namespace wowee::rendering {
namespace {

constexpr float kLifetime = 12.0f;
constexpr float kFadeStart = 1.5f;
constexpr size_t kMaxPrints = 512;

struct FootprintPushConstants {
    glm::mat4 model{1.0f};
    glm::vec4 tint{0.075f, 0.065f, 0.05f, 1.0f};
};

} // namespace

FootprintRenderer::~FootprintRenderer() {
    shutdown();
}

bool FootprintRenderer::initialize(Renderer* owner, VkContext* ctx,
                                   VkDescriptorSetLayout perFrameLayout,
                                   pipeline::AssetManager* assetManager) {
    if (!owner || !ctx || !assetManager) {
        LOG_WARNING("FootprintRenderer: missing renderer, Vulkan context, or asset manager");
        return false;
    }
    if (vkCtx_) return true;

    owner_ = owner;
    vkCtx_ = ctx;
    perFrameLayout_ = perFrameLayout;
    if (!createDescriptorResources() || !createPipeline() || !createQuad() ||
        !loadFootprintData(assetManager)) {
        LOG_WARNING("FootprintRenderer: initialization incomplete");
        shutdown();
        return false;
    }
    LOG_INFO("FootprintRenderer: loaded ", profilesByModel_.size(),
             " model profiles and ", kTextureCount, " original footprint masks");
    return true;
}

void FootprintRenderer::shutdown() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();
    VmaAllocator allocator = vkCtx_->getAllocator();
    vkDeviceWaitIdle(device);

    for (auto& texture : textures_) texture.destroy(device, allocator);
    // Through the helpers, which clear each handle as they destroy it, so the
    // block of assignments that used to follow is gone.
    destroy(allocator, quadVB_, quadVBAlloc_);
    destroy(device, pipeline_);
    destroy(device, pipelineLayout_);
    destroy(device, descriptorPool_);
    destroy(device, materialSetLayout_);

    textureSets_.fill(VK_NULL_HANDLE);
    terrainFlags_.clear();
    groundEffectTerrain_.clear();
    profilesByModel_.clear();
    prints_.clear();
    owner_ = nullptr;
    vkCtx_ = nullptr;
    perFrameLayout_ = VK_NULL_HANDLE;
}

bool FootprintRenderer::createDescriptorResources() {
    VkDevice device = vkCtx_->getDevice();
    VkDescriptorSetLayoutBinding sampler{};
    sampler.binding = 0;
    sampler.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sampler.descriptorCount = 1;
    sampler.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    materialSetLayout_ = createDescriptorSetLayout(device, {sampler});
    if (!materialSetLayout_) return false;

    VkDescriptorPoolSize size{};
    size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    size.descriptorCount = kTextureCount;
    VkDescriptorPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = kTextureCount;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &size;
    if (vkCreateDescriptorPool(device, &pool, nullptr, &descriptorPool_) != VK_SUCCESS) return false;

    std::array<VkDescriptorSetLayout, kTextureCount> layouts{};
    layouts.fill(materialSetLayout_);
    VkDescriptorSetAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = descriptorPool_;
    alloc.descriptorSetCount = kTextureCount;
    alloc.pSetLayouts = layouts.data();
    return vkAllocateDescriptorSets(device, &alloc, textureSets_.data()) == VK_SUCCESS;
}

bool FootprintRenderer::createPipeline() {
    VkDevice device = vkCtx_->getDevice();
    VkShaderModule vert;
    VkShaderModule frag;
    if (!vert.loadFromFile(device, "assets/shaders/footprint.vert.spv") ||
        !frag.loadFromFile(device, "assets/shaders/footprint.frag.spv")) {
        vert.destroy();
        frag.destroy();
        return false;
    }

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push.size = sizeof(FootprintPushConstants);
    if (!pipelineLayout_) {
        pipelineLayout_ = createPipelineLayout(device, {perFrameLayout_, materialSetLayout_}, {push});
    }

    VkVertexInputBindingDescription binding{.binding = 0, .stride = 5 * sizeof(float), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription pos{.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0};
    VkVertexInputAttributeDescription uv{.location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 3 * sizeof(float)};
    pipeline_ = PipelineBuilder()
        .setShaders(vert.stageInfo(VK_SHADER_STAGE_VERTEX_BIT), frag.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
        .setVertexInput({binding}, {pos, uv})
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setDepthTest(true, false, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setColorBlendAttachment(PipelineBuilder::blendAlpha())
        .setMultisample(vkCtx_->getMsaaSamples())
        .setLayout(pipelineLayout_)
        .setRenderPass(vkCtx_->getImGuiRenderPass())
        .setDynamicStates(viewportAndScissorDynamic())
        .build(device, vkCtx_->getPipelineCache());
    vert.destroy();
    frag.destroy();
    return pipeline_ != VK_NULL_HANDLE;
}

void FootprintRenderer::recreatePipelines() {
    if (!vkCtx_) return;
    if (pipeline_) {
        vkDestroyPipeline(vkCtx_->getDevice(), pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
    if (!createPipeline()) LOG_WARNING("FootprintRenderer: pipeline recreation failed");
}

bool FootprintRenderer::createQuad() {
    const float vertices[] = {
        -0.5f, -0.5f, 0.0f, 0.0f, 1.0f,
         0.5f, -0.5f, 0.0f, 1.0f, 1.0f,
         0.5f,  0.5f, 0.0f, 1.0f, 0.0f,
        -0.5f, -0.5f, 0.0f, 0.0f, 1.0f,
         0.5f,  0.5f, 0.0f, 1.0f, 0.0f,
        -0.5f,  0.5f, 0.0f, 0.0f, 0.0f
    };
    AllocatedBuffer buffer = uploadBuffer(*vkCtx_, vertices, sizeof(vertices),
                                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    quadVB_ = buffer.buffer;
    quadVBAlloc_ = buffer.allocation;
    return quadVB_ != VK_NULL_HANDLE;
}

bool FootprintRenderer::loadFootprintData(pipeline::AssetManager* assets) {
    const auto terrain = assets->loadDBC("TerrainType.dbc");
    const auto effects = assets->loadDBC("GroundEffectTexture.dbc");
    if (!terrain || !effects) {
        LOG_ERROR("FootprintRenderer: required surface DBCs unavailable");
        return false;
    }
    assert(terrain->getFieldCount() == 6);
    const uint32_t effectFields = effects->getFieldCount();
    assert(effectFields == 7 || effectFields == 11);
    for (uint32_t row = 0; row < terrain->getRecordCount(); ++row)
        terrainFlags_[terrain->getUInt32(row, 0)] = terrain->getUInt32(row, 5);
    for (uint32_t row = 0; row < effects->getRecordCount(); ++row)
        groundEffectTerrain_[effects->getUInt32(row, 0)] = effects->getUInt32(row, effectFields - 1);

    // FootprintTextures IDs are sparse; every row is a mask.
    std::unordered_map<uint32_t, uint8_t> indexById;
    auto textureDbc = assets->loadDBC("FootprintTextures.dbc");
    if (!textureDbc || !textureDbc->isLoaded()) return false;
    if (textureDbc->getRecordCount() != kTextureCount) {
        LOG_ERROR("FootprintRenderer: FootprintTextures.dbc has ", textureDbc->getRecordCount(),
                  " rows, expected ", kTextureCount);
        return false;
    }

    VkDevice device = vkCtx_->getDevice();
    for (size_t i = 0; i < kTextureCount; ++i) {
        const auto row = static_cast<uint32_t>(i);
        std::string path = textureDbc->getString(row, 1);
        if (path.find('.') == std::string::npos) path += ".blp";
        pipeline::BLPImage image = assets->loadTexture(path);
        if (!image.isValid() || !textures_[i].upload(*vkCtx_, image.data.data(), image.width, image.height,
                                                     VK_FORMAT_R8G8B8A8_UNORM, true)) {
            LOG_WARNING("FootprintRenderer: failed to load ", path);
            return false;
        }
        textures_[i].createSampler(device, VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                                   VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
        // createSampler answers nothing and the image view is created without
        // being checked either, so "uploaded" is not the same as "sampleable".
        // Writing an unsampleable texture into a live descriptor is undefined
        // behaviour that costs the device rather than the footprints. See #123.
        if (!textures_[i].isValid()) {
            LOG_WARNING("FootprintRenderer: ", path, " is not sampleable");
            return false;
        }
        VkDescriptorImageInfo imageInfo = textures_[i].descriptorInfo();
        VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = textureSets_[i];
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        indexById[textureDbc->getUInt32(row, 0)] = static_cast<uint8_t>(i);
    }

    auto modelDbc = assets->loadDBC("CreatureModelData.dbc");
    if (!modelDbc || !modelDbc->isLoaded() || modelDbc->getFieldCount() <= 8) return false;
    for (uint32_t row = 0; row < modelDbc->getRecordCount(); ++row) {
        auto textureIt = indexById.find(modelDbc->getUInt32(row, 6));
        if (textureIt == indexById.end()) continue;
        // Full decal dimensions, calibrated against matched-camera TBC captures.
        // The mask includes transparent padding; these are not shoe dimensions.
        const float length = modelDbc->getFloat(row, 7) / 24.0f;
        const float width = modelDbc->getFloat(row, 8) / 24.0f;
        // Rows with no size (Creature\Tiger\Tigercub.mdx) leave no print.
        if (length <= 0.0f || width <= 0.0f) continue;
        profilesByModel_[modelDbc->getUInt32(row, 0)] =
            Profile{.textureIndex = textureIt->second, .length = length, .width = width};
    }
    return !profilesByModel_.empty();
}

bool FootprintRenderer::hasProfile(uint32_t creatureModelDataId) const {
    return profilesByModel_.contains(creatureModelDataId);
}

float FootprintRenderer::resolveFloorHeight(const glm::vec3& position) const {
    std::optional<float> floor;
    auto consider = [&](std::optional<float> height) {
        if (!height || *height < position.z - 1.25f || *height > position.z + 0.85f) return;
        floor = floor ? std::max(*floor, *height) : *height;
    };
    if (auto* terrain = owner_->getTerrainManager())
        consider(terrain->getHeightAt(position.x, position.y));
    if (auto* wmo = owner_->getWMORenderer())
        consider(wmo->getFloorHeight(position.x, position.y, position.z + 3.0f));
    if (auto* m2 = owner_->getM2Renderer())
        consider(m2->getFloorHeight(position.x, position.y, position.z + 2.0f));
    return floor.value_or(position.z);
}

bool FootprintRenderer::surfaceAllowsFootprints(const glm::vec3& position) const {
    std::optional<float> support;
    std::optional<uint32_t> terrainType;
    const auto reachable = [&](std::optional<float> z) {
        return z && std::abs(*z - position.z) <= 0.85f;
    };
    if (auto* terrain = owner_->getTerrainManager(); terrain && !terrain->isHoleAt(position.x, position.y)) {
        const auto height = terrain->getHeightAt(position.x, position.y);
        if (reachable(height)) {
            support = height;
            uint32_t effectId = 0;
            if (terrain->getDominantTextureAt(position.x, position.y, &effectId)) {
                const auto entry = groundEffectTerrain_.find(effectId);
                if (entry != groundEffectTerrain_.end()) terrainType = entry->second;
            }
        }
    }
    if (auto* wmo = owner_->getWMORenderer()) {
        std::optional<uint32_t> materialType;
        const auto height = wmo->getFloorHeight(position.x, position.y, position.z + 0.85f,
            nullptr, position.z, &materialType);
        if (reachable(height) && (!support || *height >= *support - 0.01f)) {
            support = height;
            terrainType = materialType;
        }
    }
    if (auto* m2 = owner_->getM2Renderer()) {
        const auto height = m2->getFloorHeight(position.x, position.y, position.z + 0.85f);
        // Model surfaces carry no TerrainType footprint permission.
        if (reachable(height) && (!support || *height > *support + 0.01f)) return false;
    }
    if (!support || !terrainType) return false;
    const auto flags = terrainFlags_.find(*terrainType);
    return flags != terrainFlags_.end() && (flags->second & 1u) != 0;
}

void FootprintRenderer::spawn(uint32_t creatureModelDataId, const glm::vec3& footPosition,
                              float yawRadians, bool leftFoot, float scale) {
    if (!vkCtx_ || !surfaceAllowsFootprints(footPosition)) return;
    Profile profile = profilesByModel_.at(creatureModelDataId);
    assert(std::isfinite(scale) && scale > 0.0f);
    profile.length *= scale;
    profile.width *= scale;
    glm::vec3 position = footPosition;
    position.z = resolveFloorHeight(position) + 0.035f;

    // Fit the decal plane to the supporting surface, rather than cutting half
    // the mask into the ground on slopes.
    const float radius = std::max(profile.width, profile.length) * 0.5f;
    const glm::vec3 dx(radius, 0.0f, 0.0f), dy(0.0f, radius, 0.0f);
    const glm::vec2 slope(
        (resolveFloorHeight(position + dx) - resolveFloorHeight(position - dx)) / (2.0f * radius),
        (resolveFloorHeight(position + dy) - resolveFloorHeight(position - dy)) / (2.0f * radius));

    if (prints_.size() >= kMaxPrints) prints_.erase(prints_.begin());
    // A negative width mirrors the one-foot mask laterally for the opposite foot.
    prints_.push_back({.position = position, .slope = slope, .yaw = yawRadians, .length = profile.length,
                       .signedWidth = leftFoot ? -profile.width : profile.width, .age = 0.0f,
                       .textureIndex = profile.textureIndex});
}

void FootprintRenderer::update(float deltaTime) {
    for (Print& print : prints_) print.age += deltaTime;
    prints_.erase(std::remove_if(prints_.begin(), prints_.end(),
        [](const Print& print) { return print.age >= kLifetime; }), prints_.end());
}

void FootprintRenderer::clear() {
    prints_.clear();
}

void FootprintRenderer::render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const Camera& camera) {
    if (prints_.empty() || !pipeline_ || !quadVB_) return;
    const glm::vec3 cameraPos = camera.getPosition();
    Frustum frustum;
    frustum.extractFromMatrix(camera.getViewProjectionMatrix());

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_,
                            0, 1, &perFrameSet, 0, nullptr);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &quadVB_, &offset);

    for (const Print& print : prints_) {
        const glm::vec3 delta = cameraPos - print.position;
        if (glm::dot(delta, delta) > 10000.0f ||
            !frustum.intersectsSphere(print.position, std::max(print.length, std::abs(print.signedWidth))))
            continue;

        float alpha = 1.0f;
        if (print.age > kFadeStart) {
            float t = glm::clamp((print.age - kFadeStart) / (kLifetime - kFadeStart), 0.0f, 1.0f);
            t = t * t * (3.0f - 2.0f * t);
            alpha = 1.0f - t;
        }
        if (alpha <= 0.001f || print.textureIndex >= kTextureCount) continue;

        glm::mat4 model = glm::translate(glm::mat4(1.0f), print.position);
        // The shipped masks point toward +V/local +Y; rotate that axis onto
        // the model's +X forward direction and mirror across local X.
        model = glm::rotate(model, print.yaw - 1.57079632679f, glm::vec3(0.0f, 0.0f, 1.0f));
        model = glm::scale(model, glm::vec3(print.signedWidth, print.length, 1.0f));
        model[0].z = glm::dot(print.slope, glm::vec2(model[0]));
        model[1].z = glm::dot(print.slope, glm::vec2(model[1]));
        FootprintPushConstants push;
        push.model = model;
        push.tint.a = alpha;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_,
                                1, 1, &textureSets_[print.textureIndex], 0, nullptr);
        vkCmdPushConstants(cmd, pipelineLayout_,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(push), &push);
        vkCmdDraw(cmd, 6, 1, 0, 0);
    }
}

} // namespace wowee::rendering
