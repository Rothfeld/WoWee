#include "rendering/overlay_system.hpp"
#include "core/coordinates.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_utils.hpp"
#include "core/logger.hpp"
#include "pipeline/asset_manager.hpp"
#include <algorithm>
#include <stdexcept>
#include <cstring>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <vector>

namespace wowee {
namespace rendering {

OverlaySystem::OverlaySystem(VkContext* ctx)
    : vkCtx_(ctx) {}

OverlaySystem::~OverlaySystem() {
    cleanup();
}

void OverlaySystem::cleanup() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();
    destroy(device, selCirclePipeline_);
    destroy(device, selCirclePipelineLayout_);
    for (auto& buffer : selCircleVertexBuffers_) destroyBuffer(vkCtx_->getAllocator(), buffer);
    selCircleVertexCapacity_ = {};
    destroy(device, selCirclePool_);
    destroy(device, selCircleSetLayout_);
    selCircleSet_ = VK_NULL_HANDLE;
    selCircleTexture_.destroy(device, vkCtx_->getAllocator());
    destroy(device, shadowPipeline_);
    destroy(device, shadowPipelineLayout_);
    for (auto& buffer : shadowVertexBuffers_) destroyBuffer(vkCtx_->getAllocator(), buffer);
    shadowVertexCapacity_ = {};
    destroy(device, shadowPool_);
    destroy(device, shadowSetLayout_);
    shadowSet_ = VK_NULL_HANDLE;
    shadowTexture_.destroy(device, vkCtx_->getAllocator());
    destroy(device, overlayPipeline_);
    destroy(device, brightnessPipeline_);
    destroy(device, overlayPipelineLayout_);
}

void OverlaySystem::recreatePipelines() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();
    // Destroy only pipelines (keep geometry buffers)
    destroy(device, selCirclePipeline_);
    destroy(device, shadowPipeline_);
    destroy(device, overlayPipeline_);
    destroy(device, brightnessPipeline_);
}

void OverlaySystem::setSelectionCircle(const glm::vec3& pos, float radius, const glm::vec3& color) {
    selCirclePos_ = pos;
    selCircleRadius_ = radius;
    selCircleColor_ = color;
    selCircleVisible_ = true;
}

void OverlaySystem::setSelectionCirclePosition(const glm::vec3& pos) {
    selCirclePos_ = pos;
}

void OverlaySystem::clearSelectionCircle() {
    selCircleVisible_ = false;
}

namespace {

/// One combined-image-sampler set for `texture`, in its own pool.
void createTextureSet(VkDevice device, const VkTexture& texture, VkDescriptorSetLayout& layout,
                      VkDescriptorPool& pool, VkDescriptorSet& set, const char* what) {
    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                        1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    layout = createDescriptorSetLayout(device, {binding});
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &size;
    if (!layout || vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool) != VK_SUCCESS)
        throw std::runtime_error(std::string(what) + " descriptor pool failed to create");
    VkDescriptorSetAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = pool;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &layout;
    if (vkAllocateDescriptorSets(device, &alloc, &set) != VK_SUCCESS)
        throw std::runtime_error(std::string(what) + " descriptor allocation failed");
    auto info = texture.descriptorInfo();
    VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &info;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
}

/// Loads a BLP with a linear clamped sampler.
void loadClampedTexture(VkContext& ctx, pipeline::AssetManager& assets, VkTexture& texture, const char* path) {
    const auto image = assets.loadTexture(path);
    if (!image.isValid() || !texture.uploadBLP(ctx, image)) {
        LOG_ERROR("Cannot load ", path);
        throw std::runtime_error(std::string(path) + " failed to load");
    }
    if (!texture.createSampler(ctx.getDevice(), VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                               VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE))
        throw std::runtime_error(std::string(path) + " sampler failed to create");
}

}  // namespace

void OverlaySystem::loadSelectionTexture(pipeline::AssetManager& assets) {
    if (selCircleTexture_.isValid()) return;
    loadClampedTexture(*vkCtx_, assets, selCircleTexture_, "Textures\\UnitSelectTexture.blp");
    createTextureSet(vkCtx_->getDevice(), selCircleTexture_, selCircleSetLayout_, selCirclePool_,
                     selCircleSet_, "Selection circle");
    LOG_INFO("Selection circle uses Textures/UnitSelectTexture.blp");
}

void OverlaySystem::loadUnitShadowTexture(pipeline::AssetManager& assets) {
    if (shadowTexture_.isValid()) return;
    loadClampedTexture(*vkCtx_, assets, shadowTexture_, "Textures\\ShadowBlob.blp");
    createTextureSet(vkCtx_->getDevice(), shadowTexture_, shadowSetLayout_, shadowPool_,
                     shadowSet_, "Unit shadow");
    LOG_INFO("Unit shadows use Textures/ShadowBlob.blp");
}

namespace {

/// selection_circle.vert/.frag's push block.
struct SelectionPush {
    glm::mat4 mvp;
    glm::vec4 color;
    glm::vec4 square;          // centre x, y; half side; unused
    glm::vec4 axisAndHeights;  // the square's right axis x, y; lowest and highest z drawn
};
static_assert(sizeof(SelectionPush) == 112, "must match selection_circle's push block");

}  // namespace

void OverlaySystem::initSelectionCircle() {
    if (selCirclePipeline_ != VK_NULL_HANDLE) return;
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();

    // Load shaders
    VkShaderModule vertShader, fragShader;
    if (!vertShader.loadFromFile(device, "assets/shaders/selection_circle.vert.spv")) {
        LOG_ERROR("OverlaySystem: failed to load selection circle vertex shader");
        return;
    }
    if (!fragShader.loadFromFile(device, "assets/shaders/selection_circle.frag.spv")) {
        LOG_ERROR("OverlaySystem: failed to load selection circle fragment shader");
        vertShader.destroy();
        return;
    }

    // Texture, mat4 view-projection, reaction colour, then the ring's
    // square (centre, half side, its right axis) and the box's height range.
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset = 0;
    pcRange.size = sizeof(SelectionPush);
    if (!selCircleSet_) throw std::logic_error("Selection texture was not initialized");
    if (!selCirclePipelineLayout_)
        selCirclePipelineLayout_ = createPipelineLayout(device, {selCircleSetLayout_}, {pcRange});

    // World-space triangles of the surfaces under the ring, rebuilt each frame.
    VkVertexInputBindingDescription vertBind{0, sizeof(glm::vec3), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription position{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};

    // Build pipeline
    selCirclePipeline_ = PipelineBuilder()
        .setShaders(vertShader.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                    fragShader.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
        .setVertexInput({vertBind}, {position})
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setDepthTest(true, false, VK_COMPARE_OP_LESS_OR_EQUAL)
        // The ring lies on the very triangles the world drew, transformed
        // here on the CPU rather than by their own shaders: pulled forward by
        // the smallest depth step (and the triangle's slope) so it does not
        // fight them.
        .setDepthBias(-1.0f, -1.0f)
        .setColorBlendAttachment(PipelineBuilder::blendAdditive())
        .setMultisample(vkCtx_->getMsaaSamples())
        .setLayout(selCirclePipelineLayout_)
        .setRenderPass(vkCtx_->getImGuiRenderPass())
        .setDynamicStates(viewportAndScissorDynamic())
        .build(device, vkCtx_->getPipelineCache());

    vertShader.destroy();
    fragShader.destroy();

    if (!selCirclePipeline_) {
        LOG_ERROR("OverlaySystem: failed to build selection circle pipeline");
    }
}

void OverlaySystem::renderSelectionCircle(const glm::mat4& view, const glm::mat4& projection,
                                           VkCommandBuffer cmd,
                                           const HeightQuery2D& terrainHeight,
                                           const HeightQuery3D& wmoHeight,
                                           const HeightQuery3D& m2Height,
                                           const SurfaceTriangles& surfaces) {
    if (!selCircleVisible_) return;
    initSelectionCircle();
    if (selCirclePipeline_ == VK_NULL_HANDLE || cmd == VK_NULL_HANDLE) return;

    // The ring's size: a sphere of the selection radius around the unit's
    // position meeting the ground under it. Measured in the original 2.4.3
    // (GM Island hilltop, a flying creature tempspawned at known heights h
    // over the ground): its width is 2 * sqrt(r^2 - h^2) (Infernal display
    // 7950, r 6.45: 12.3/12.3/11.7/9.9 yd at h 0/2/3/4 against
    // 12.9/12.3/11.4/10.1), and it is gone once h reaches r (human r 0.84 by
    // 1 yd, Infernal 7029 r 2.52 by 2.5-3.5 yd).
    const glm::vec3 center = selCirclePos_;
    const float sphere = selCircleRadius_;
    // Surfaces inside the sphere's vertical extent; the ground is the one
    // nearest the unit's height.
    std::optional<float> ground;
    auto consider = [&](std::optional<float> h) {
        if (h && *h >= center.z - sphere && *h <= center.z + sphere &&
            (!ground || std::abs(*h - center.z) < std::abs(*ground - center.z))) ground = h;
    };
    if (terrainHeight) consider(terrainHeight(center.x, center.y));
    if (wmoHeight) consider(wmoHeight(center.x, center.y, center.z + sphere));
    if (m2Height) consider(m2Height(center.x, center.y, center.z + sphere));
    if (!ground) return;
    const float height = std::abs(center.z - *ground);
    if (height >= sphere) return;
    const float radius = std::sqrt(sphere * sphere - height * height);

    // Where it is drawn: the texture projected straight down over the square
    // centre +- radius onto every terrain and WMO triangle in the box that
    // reaches the selection radius above and below the unit - walls, step
    // risers and render-only detail floors included, doodads (M2) not.
    // Native 2.4.3 beside Northshire Abbey (Huhuran's display 15739 morphed
    // onto a tempspawn) paints the abbey wall and the stair sides in vertical
    // streaks and leaves the lantern pillar doodad and the cypress bare.
    // WoWee used to drape a 24x24 grid over sampled floor heights: in
    // Ahn'Qiraj (Huhuran's hall, WMO floor) every crease of the floor between
    // two grid vertices poked through the ring as a dark shard. Drawn on the
    // drawn triangles themselves there is nothing to poke through.
    // _experiments/20261118T174257Z.selection_ring_holes in the workspace.
    const glm::vec3 boxMin(center.x - radius, center.y - radius, center.z - sphere);
    const glm::vec3 boxMax(center.x + radius, center.y + radius, center.z + sphere);
    auto& triangles = selCircleTriangles_;
    triangles.clear();
    if (surfaces) surfaces(boxMin, boxMax, triangles);
    if (triangles.empty()) return;

    const uint32_t frame = vkCtx_->getCurrentFrame();
    auto& buffer = selCircleVertexBuffers_[frame];
    auto& capacity = selCircleVertexCapacity_[frame];
    if (capacity < triangles.size()) {
        // This frame's previous use of the buffer has completed: the frame
        // fence was waited on before recording, so it can be replaced.
        destroyBuffer(vkCtx_->getAllocator(), buffer);
        capacity = std::max<size_t>(triangles.size(), 4096) * 2;
        buffer = createBuffer(vkCtx_->getAllocator(), capacity * sizeof(glm::vec3),
                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        if (!buffer.buffer) throw std::runtime_error("Selection vertex allocation failed");
    }
    void* mapped = nullptr;
    if (vmaMapMemory(vkCtx_->getAllocator(), buffer.allocation, &mapped) != VK_SUCCESS)
        throw std::runtime_error("Selection vertex mapping failed");
    const VkDeviceSize bytes = triangles.size() * sizeof(glm::vec3);
    std::memcpy(mapped, triangles.data(), bytes);
    vmaFlushAllocation(vkCtx_->getAllocator(), buffer.allocation, 0, bytes);
    vmaUnmapMemory(vkCtx_->getAllocator(), buffer.allocation);

    // The texture's square turns with the camera's yaw, as before.
    glm::vec2 right(view[0][0], view[1][0]);
    right = glm::normalize(right);
    SelectionPush push{};
    push.mvp = projection * view;
    push.color = glm::vec4(selCircleColor_, 1.0f);
    push.square = glm::vec4(center.x, center.y, radius, 0.0f);
    push.axisAndHeights = glm::vec4(right, boxMin.z, boxMax.z);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, selCirclePipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, selCirclePipelineLayout_,
                            0, 1, &selCircleSet_, 0, nullptr);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &buffer.buffer, &offset);
    vkCmdPushConstants(cmd, selCirclePipelineLayout_,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, static_cast<uint32_t>(triangles.size()), 1, 0, 0);
}

namespace {

/// unit_shadow.vert/.frag's push block.
struct UnitShadowPush {
    glm::mat4 mvp;
    glm::vec4 centreAxis;  // unit position x, y; facing x, y
    glm::vec4 halfAndZ;    // half extent along facing, across; lowest and highest z drawn
    glm::vec4 camera;      // camera position; unused
    glm::vec4 fog;         // fog start, end; unused
};
static_assert(sizeof(UnitShadowPush) == 128, "must match unit_shadow's push block");

}  // namespace

void OverlaySystem::setUnitShadows(std::vector<UnitShadowBlob> blobs) {
    shadowBlobs_ = std::move(blobs);
}

void OverlaySystem::initUnitShadows() {
    if (shadowPipeline_ != VK_NULL_HANDLE || !vkCtx_) return;
    if (!shadowSet_) throw std::logic_error("Unit shadow texture was not initialized");
    VkDevice device = vkCtx_->getDevice();
    VkShaderModule vertShader, fragShader;
    if (!vertShader.loadFromFile(device, "assets/shaders/unit_shadow.vert.spv") ||
        !fragShader.loadFromFile(device, "assets/shaders/unit_shadow.frag.spv")) {
        vertShader.destroy();
        fragShader.destroy();
        throw std::runtime_error("Unit shadow shaders failed to load");
    }
    VkPushConstantRange pcRange{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                sizeof(UnitShadowPush)};
    if (!shadowPipelineLayout_)
        shadowPipelineLayout_ = createPipelineLayout(device, {shadowSetLayout_}, {pcRange});
    VkVertexInputBindingDescription vertBind{0, sizeof(glm::vec3), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription position{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
    // Multiplied onto the ground: dst * src.
    VkPipelineColorBlendAttachmentState modulate = PipelineBuilder::blendAlpha();
    modulate.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
    modulate.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    modulate.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    modulate.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    shadowPipeline_ = PipelineBuilder()
        .setShaders(vertShader.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                    fragShader.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
        .setVertexInput({vertBind}, {position})
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setDepthTest(true, false, VK_COMPARE_OP_LESS_OR_EQUAL)
        // Drawn on the world's own triangles, as the selection circle.
        .setDepthBias(-1.0f, -1.0f)
        .setColorBlendAttachment(modulate)
        .setMultisample(vkCtx_->getMsaaSamples())
        .setLayout(shadowPipelineLayout_)
        .setRenderPass(vkCtx_->getImGuiRenderPass())
        .setDynamicStates(viewportAndScissorDynamic())
        .build(device, vkCtx_->getPipelineCache());
    vertShader.destroy();
    fragShader.destroy();
    if (!shadowPipeline_) throw std::runtime_error("Unit shadow pipeline failed to build");
}

void OverlaySystem::renderUnitShadows(const glm::mat4& view, const glm::mat4& projection,
                                      const glm::vec3& cameraPos, float fogStart, float fogEnd,
                                      VkCommandBuffer cmd,
                                      const HeightQuery2D& terrainHeight,
                                      const HeightQuery3D& wmoHeight,
                                      const HeightQuery3D& m2Height,
                                      const SurfaceTriangles& surfaces) {
    if (shadowBlobs_.empty() || cmd == VK_NULL_HANDLE || !shadowTexture_.isValid()) return;
    initUnitShadows();

    struct Draw { UnitShadowPush push; uint32_t first; uint32_t count; };
    std::vector<Draw> draws;
    auto& triangles = shadowTriangles_;
    triangles.clear();
    for (const auto& blob : shadowBlobs_) {
        const glm::vec3 p = blob.position;
        // Nothing to draw on unless some floor under the unit lies within
        // its reach; the triangles are then clipped to that height range.
        std::optional<float> ground;
        auto consider = [&](std::optional<float> h) {
            if (h && *h <= p.z + blob.reach && *h >= p.z - blob.reach && (!ground || *h > *ground)) ground = h;
        };
        if (terrainHeight) consider(terrainHeight(p.x, p.y));
        if (wmoHeight) consider(wmoHeight(p.x, p.y, p.z + blob.reach));
        if (m2Height) consider(m2Height(p.x, p.y, p.z + blob.reach));
        if (!ground) continue;
        const glm::vec2 axis = blob.axisX, across(-axis.y, axis.x);
        const glm::vec2 reach = glm::abs(axis) * blob.halfExtent.x + glm::abs(across) * blob.halfExtent.y;
        const glm::vec3 boxMin(p.x - reach.x, p.y - reach.y, p.z - blob.reach);
        const glm::vec3 boxMax(p.x + reach.x, p.y + reach.y, p.z + blob.reach);
        const size_t first = triangles.size();
        if (surfaces) surfaces(boxMin, boxMax, triangles);
        if (triangles.size() == first) continue;
        Draw draw{};
        draw.push.mvp = projection * view;
        draw.push.centreAxis = glm::vec4(p.x, p.y, axis.x, axis.y);
        draw.push.halfAndZ = glm::vec4(blob.halfExtent, boxMin.z, boxMax.z);
        draw.push.camera = glm::vec4(cameraPos, 0.0f);
        draw.push.fog = glm::vec4(fogStart, fogEnd, 0.0f, 0.0f);
        draw.first = static_cast<uint32_t>(first);
        draw.count = static_cast<uint32_t>(triangles.size() - first);
        draws.push_back(draw);
    }
    if (draws.empty()) return;

    const uint32_t frame = vkCtx_->getCurrentFrame();
    auto& buffer = shadowVertexBuffers_[frame];
    auto& capacity = shadowVertexCapacity_[frame];
    if (capacity < triangles.size()) {
        // This frame's previous use of the buffer has completed (frame fence).
        destroyBuffer(vkCtx_->getAllocator(), buffer);
        capacity = std::max<size_t>(triangles.size(), 16384) * 2;
        buffer = createBuffer(vkCtx_->getAllocator(), capacity * sizeof(glm::vec3),
                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        if (!buffer.buffer) throw std::runtime_error("Unit shadow vertex allocation failed");
    }
    void* mapped = nullptr;
    if (vmaMapMemory(vkCtx_->getAllocator(), buffer.allocation, &mapped) != VK_SUCCESS)
        throw std::runtime_error("Unit shadow vertex mapping failed");
    const VkDeviceSize bytes = triangles.size() * sizeof(glm::vec3);
    std::memcpy(mapped, triangles.data(), bytes);
    vmaFlushAllocation(vkCtx_->getAllocator(), buffer.allocation, 0, bytes);
    vmaUnmapMemory(vkCtx_->getAllocator(), buffer.allocation);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipelineLayout_,
                            0, 1, &shadowSet_, 0, nullptr);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &buffer.buffer, &offset);
    for (const auto& draw : draws) {
        vkCmdPushConstants(cmd, shadowPipelineLayout_,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(draw.push), &draw.push);
        vkCmdDraw(cmd, draw.count, 1, draw.first, 0);
    }
}

void OverlaySystem::initOverlayPipeline() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();

    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pc.offset = 0;
    pc.size = 112;  // mat4 invViewProj + vec4 colour + vec4 plane + vec4 params

    VkPipelineLayoutCreateInfo plCI{};
    plCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plCI.pushConstantRangeCount = 1;
    plCI.pPushConstantRanges = &pc;
    vkCreatePipelineLayout(device, &plCI, nullptr, &overlayPipelineLayout_);

    VkShaderModule vertMod, fragMod;
    if (!vertMod.loadFromFile(device, "assets/shaders/postprocess.vert.spv") ||
        !fragMod.loadFromFile(device, "assets/shaders/overlay.frag.spv")) {
        LOG_ERROR("OverlaySystem: failed to load overlay shaders");
        vertMod.destroy(); fragMod.destroy();
        return;
    }

    overlayPipeline_ = PipelineBuilder()
        .setShaders(vertMod.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                    fragMod.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
        .setVertexInput({}, {})
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setNoDepthTest()
        .setColorBlendAttachment(PipelineBuilder::blendAlpha())
        .setMultisample(vkCtx_->getMsaaSamples())
        .setLayout(overlayPipelineLayout_)
        .setRenderPass(vkCtx_->getImGuiRenderPass())
        .setDynamicStates(viewportAndScissorDynamic())
        .build(device, vkCtx_->getPipelineCache());

    vertMod.destroy(); fragMod.destroy();

    if (overlayPipeline_) LOG_INFO("OverlaySystem: overlay pipeline initialized");
}

namespace {

/// overlay.frag's push block. One definition, because there are two entry
/// points into that shader and only one of them was keeping up with it.
struct OverlayPush {
    glm::mat4 invViewProj;
    glm::vec4 color;
    glm::vec4 plane;
    glm::vec4 params;
};
static_assert(sizeof(OverlayPush) == 112, "must match overlay.frag's push block");

OverlayPush makeOverlayPush(const glm::vec4& color, const glm::mat4& invViewProj,
                            float waterZ, float softness, float rippleAmp,
                            float time, bool hasSeam) {
    OverlayPush p{};
    p.invViewProj = invViewProj;
    p.color = color;
    p.plane = glm::vec4(0.0f, 0.0f, 0.0f, waterZ);
    p.params = glm::vec4(softness, time, rippleAmp, hasSeam ? 1.0f : 0.0f);
    return p;
}

}  // namespace

void OverlaySystem::renderOverlay(const glm::vec4& color, VkCommandBuffer cmd) {
    // Whole screen: no seam, so the shader skips the surface test entirely and
    // the matrix and height below are never read.
    renderWaterline(color, glm::mat4(1.0f), 0.0f, 0.0f, 0.0f, 0.0f, false, cmd);
}

void OverlaySystem::renderWaterline(const glm::vec4& color, const glm::mat4& invViewProj,
                                    float waterZ, float softness, float rippleAmp,
                                    float time, bool hasSeam, VkCommandBuffer cmd) {
    if (!overlayPipeline_) initOverlayPipeline();
    if (!overlayPipeline_ || cmd == VK_NULL_HANDLE) return;
    OverlayPush push = makeOverlayPush(color, invViewProj, waterZ, softness, rippleAmp,
                                       time, hasSeam);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, overlayPipeline_);
    vkCmdPushConstants(cmd, overlayPipelineLayout_,
                       VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

void OverlaySystem::initBrightnessPipeline() {
    if (brightnessPipeline_ != VK_NULL_HANDLE) return;
    if (!vkCtx_) return;
    // Reuse the overlay pipeline's layout (same fragment push constant); only
    // the blend state differs.
    if (!overlayPipeline_) initOverlayPipeline();
    if (!overlayPipelineLayout_) return;
    VkDevice device = vkCtx_->getDevice();

    VkShaderModule vertMod, fragMod;
    if (!vertMod.loadFromFile(device, "assets/shaders/postprocess.vert.spv") ||
        !fragMod.loadFromFile(device, "assets/shaders/overlay.frag.spv")) {
        LOG_ERROR("OverlaySystem: failed to load brightness shaders");
        vertMod.destroy(); fragMod.destroy();
        return;
    }

    // result.rgb = src.rgb * dst.rgb + dst.rgb * 1 = dst.rgb * (1 + src.rgb).
    // Pushing src.rgb = (scale - 1) yields dst.rgb * scale - a true luminance
    // multiply. Alpha is left untouched (dst passes through).
    VkPipelineColorBlendAttachmentState mul{};
    mul.blendEnable = VK_TRUE;
    mul.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
    mul.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    mul.colorBlendOp = VK_BLEND_OP_ADD;
    mul.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    mul.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    mul.alphaBlendOp = VK_BLEND_OP_ADD;
    mul.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    brightnessPipeline_ = PipelineBuilder()
        .setShaders(vertMod.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                    fragMod.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
        .setVertexInput({}, {})
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setNoDepthTest()
        .setColorBlendAttachment(mul)
        .setMultisample(vkCtx_->getMsaaSamples())
        .setLayout(overlayPipelineLayout_)
        .setRenderPass(vkCtx_->getImGuiRenderPass())
        .setDynamicStates(viewportAndScissorDynamic())
        .build(device, vkCtx_->getPipelineCache());

    vertMod.destroy(); fragMod.destroy();
    if (brightnessPipeline_) LOG_INFO("OverlaySystem: brightness pipeline initialized");
}

void OverlaySystem::renderBrightnessScale(float scale, VkCommandBuffer cmd) {
    if (scale <= 1.0f) return; // darkening handled by the black overlay path
    if (!brightnessPipeline_) initBrightnessPipeline();
    if (!brightnessPipeline_ || cmd == VK_NULL_HANDLE) return;
    // overlay.frag outputs the pushed color; the blend multiplies it by dst.
    //
    // The whole block, not just the colour. This pushed sixteen bytes back when
    // the block started with one, and kept doing it after a mat4 was put in
    // front: the colour landed in the matrix's first row and everything the
    // shader actually reads was whatever the last draw had left there. It runs
    // whenever brightness is above neutral, so the screen flashed green or red
    // by the frame.
    const glm::vec4 tint(scale - 1.0f, scale - 1.0f, scale - 1.0f, 1.0f);
    OverlayPush push = makeOverlayPush(tint, glm::mat4(1.0f), 0.0f, 0.0f, 0.0f, 0.0f, false);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, brightnessPipeline_);
    vkCmdPushConstants(cmd, overlayPipelineLayout_,
                       VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

} // namespace rendering
} // namespace wowee
