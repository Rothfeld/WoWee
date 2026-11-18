#include "rendering/overlay_system.hpp"
#include "core/coordinates.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_utils.hpp"
#include "core/logger.hpp"
#include "pipeline/asset_manager.hpp"
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
    destroy(device, selCirclePool_);
    destroy(device, selCircleSetLayout_);
    selCircleSet_ = VK_NULL_HANDLE;
    selCircleTexture_.destroy(device, vkCtx_->getAllocator());
    destroy(vkCtx_->getAllocator(), selCircleIdxBuf_, selCircleIdxAlloc_);
    destroy(device, overlayPipeline_);
    destroy(device, brightnessPipeline_);
    destroy(device, overlayPipelineLayout_);
}

void OverlaySystem::recreatePipelines() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();
    // Destroy only pipelines (keep geometry buffers)
    destroy(device, selCirclePipeline_);
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

void OverlaySystem::loadSelectionTexture(pipeline::AssetManager& assets) {
    if (selCircleTexture_.isValid()) return;
    const auto image = assets.loadTexture("Textures\\UnitSelectTexture.blp");
    if (!image.isValid() || !selCircleTexture_.uploadBLP(*vkCtx_, image)) {
        LOG_ERROR("Cannot load Textures/UnitSelectTexture.blp");
        throw std::runtime_error("Selection circle texture failed to load");
    }
    VkDevice device = vkCtx_->getDevice();
    if (!selCircleTexture_.createSampler(device, VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                                        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE))
        throw std::runtime_error("Selection circle sampler failed to create");
    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                        1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    selCircleSetLayout_ = createDescriptorSetLayout(device, {binding});
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = 1;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &size;
    if (!selCircleSetLayout_ || vkCreateDescriptorPool(device, &pool, nullptr, &selCirclePool_) != VK_SUCCESS)
        throw std::runtime_error("Selection circle descriptor pool failed to create");
    VkDescriptorSetAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = selCirclePool_;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &selCircleSetLayout_;
    if (vkAllocateDescriptorSets(device, &alloc, &selCircleSet_) != VK_SUCCESS)
        throw std::runtime_error("Selection circle descriptor allocation failed");
    auto info = selCircleTexture_.descriptorInfo();
    VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = selCircleSet_;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &info;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    LOG_INFO("Selection circle uses Textures/UnitSelectTexture.blp");
}

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

    // Texture plus mat4 view-projection and reaction color.
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset = 0;
    pcRange.size = 80;
    if (!selCircleSet_) throw std::logic_error("Selection texture was not initialized");
    if (!selCirclePipelineLayout_)
        selCirclePipelineLayout_ = createPipelineLayout(device, {selCircleSetLayout_}, {pcRange});

    // Ground-conforming grid; each frame owns its mutable vertices.
    VkVertexInputBindingDescription vertBind{0, 20, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription position{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
    VkVertexInputAttributeDescription uv{1, 0, VK_FORMAT_R32G32_SFLOAT, 12};
    constexpr int cells = kSelectionCells;
    if (selCircleIdxBuf_ == VK_NULL_HANDLE) {
        std::vector<uint16_t> indices;
        for (int y = 0; y < cells; ++y) {
            for (int x = 0; x < cells; ++x) {
                const uint16_t a = y * (cells + 1) + x;
                indices.insert(indices.end(), {a, uint16_t(a + 1), uint16_t(a + cells + 1),
                    uint16_t(a + 1), uint16_t(a + cells + 2), uint16_t(a + cells + 1)});
            }
        }
        selCircleVertCount_ = static_cast<int>(indices.size());
        auto ibuf = uploadBuffer(*vkCtx_, indices.data(), indices.size() * sizeof(uint16_t),
                                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        selCircleIdxBuf_ = ibuf.buffer;
        selCircleIdxAlloc_ = ibuf.allocation;
        for (auto& buffer : selCircleVertexBuffers_) {
            buffer = createBuffer(vkCtx_->getAllocator(), (cells + 1) * (cells + 1) * 20,
                                  VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
            if (!buffer.buffer) throw std::runtime_error("Selection vertex allocation failed");
        }
    }

    // Build pipeline
    selCirclePipeline_ = PipelineBuilder()
        .setShaders(vertShader.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                    fragShader.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
        .setVertexInput({vertBind}, {position, uv})
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setDepthTest(true, false, VK_COMPARE_OP_LESS_OR_EQUAL)
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
                                           const HeightQuery3D& m2Height) {
    if (!selCircleVisible_) return;
    initSelectionCircle();
    if (selCirclePipeline_ == VK_NULL_HANDLE || cmd == VK_NULL_HANDLE) return;

    // The ring is where a sphere of the selection radius around the unit's
    // position meets the ground below it, projected straight down onto
    // terrain, WMO and M2 floors so it wraps slopes. Measured in the original
    // 2.4.3 (GM Island hilltop, a flying creature tempspawned at known
    // heights h over the ground): the ring stays on the ground at every
    // height, its width is 2 * sqrt(r^2 - h^2) (Infernal display 7950, r 6.45:
    // 12.3/12.3/11.7/9.9 yd at h 0/2/3/4 against 12.9/12.3/11.4/10.1), and it
    // is gone once h reaches r (human r 0.84 by 1 yd, Infernal 7029 r 2.52 by
    // 2.5-3.5 yd). It drew at the unit's feet before, flying along with it.
    const glm::vec3 center = selCirclePos_;
    const float sphere = selCircleRadius_;
    // Surfaces inside the sphere's vertical extent; the ground is the one
    // nearest the unit's height.
    auto surfaceNear = [&](float x, float y, float target) {
        std::optional<float> best;
        auto consider = [&](std::optional<float> h) {
            if (h && *h >= center.z - sphere && *h <= center.z + sphere &&
                (!best || std::abs(*h - target) < std::abs(*best - target))) best = h;
        };
        if (terrainHeight) consider(terrainHeight(x, y));
        if (wmoHeight) consider(wmoHeight(x, y, center.z + sphere));
        if (m2Height) consider(m2Height(x, y, center.z + sphere));
        return best;
    };
    const std::optional<float> ground = surfaceNear(center.x, center.y, center.z);
    if (!ground) return;
    const float height = std::abs(center.z - *ground);
    if (height >= sphere) return;
    const float radius = std::sqrt(sphere * sphere - height * height);

    constexpr int cells = kSelectionCells;
    auto& vertices = selCircleVertices_;
    glm::vec2 right(view[0][0], view[1][0]);
    right = glm::normalize(right);
    const glm::vec2 forward(-right.y, right.x);
    if (selCircleMeshAge_++ >= 30 || selCircleMeshPos_ != selCirclePos_ ||
        selCircleMeshRadius_ != selCircleRadius_ || selCircleMeshRight_ != right) {
        selCircleMeshAge_ = 0;
        selCircleMeshPos_ = selCirclePos_;
        selCircleMeshRadius_ = selCircleRadius_;
        selCircleMeshRight_ = right;
        for (int y = 0; y <= cells; ++y) {
            for (int x = 0; x <= cells; ++x) {
                const float u = float(x) / cells;
                const float v = float(y) / cells;
                const glm::vec2 xy = glm::vec2(center) + radius *
                    ((2.0f * u - 1.0f) * right + (1.0f - 2.0f * v) * forward);
                // Each vertex on the surface nearest the ground under the
                // unit: a slope keeps the ring on it, a floor above or below
                // (bridge, roof) does not take it over. Over a gap the ring
                // stays at the ground's height.
                const float z = surfaceNear(xy.x, xy.y, *ground).value_or(*ground);
                const size_t offset = (y * (cells + 1) + x) * 5;
                vertices[offset] = xy.x;
                vertices[offset + 1] = xy.y;
                vertices[offset + 2] = z + 0.025f;
                vertices[offset + 3] = u;
                vertices[offset + 4] = v;
            }
        }
    }
    auto& buffer = selCircleVertexBuffers_[vkCtx_->getCurrentFrame()];
    void* mapped = nullptr;
    if (vmaMapMemory(vkCtx_->getAllocator(), buffer.allocation, &mapped) != VK_SUCCESS)
        throw std::runtime_error("Selection vertex mapping failed");
    std::memcpy(mapped, vertices.data(), sizeof(vertices));
    vmaFlushAllocation(vkCtx_->getAllocator(), buffer.allocation, 0, sizeof(vertices));
    vmaUnmapMemory(vkCtx_->getAllocator(), buffer.allocation);
    glm::mat4 mvp = projection * view;
    glm::vec4 color4(selCircleColor_, 1.0f);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, selCirclePipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, selCirclePipelineLayout_,
                            0, 1, &selCircleSet_, 0, nullptr);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &buffer.buffer, &offset);
    vkCmdBindIndexBuffer(cmd, selCircleIdxBuf_, 0, VK_INDEX_TYPE_UINT16);
    vkCmdPushConstants(cmd, selCirclePipelineLayout_,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        0, 64, &mvp[0][0]);
    vkCmdPushConstants(cmd, selCirclePipelineLayout_,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        64, 16, &color4[0]);
    vkCmdDrawIndexed(cmd, static_cast<uint32_t>(selCircleVertCount_), 1, 0, 0, 0);
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
