#include "rendering/weather_mist.hpp"
#include "rendering/weather_mist_math.hpp"

#include "core/logger.hpp"
#include "rendering/camera.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_utils.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <random>

namespace wowee::rendering {
namespace {

struct MistPush {
    glm::vec4 depthParams; // x = near, y = far, z = maxFadeRange (0 = disabled), w = depthFadeSlope
    glm::vec4 screenSize;  // xy = render target extent in pixels, z = minFadeRange, w = unused
};

float wrapSigned(float offset, float halfSpan) {
    const float span = 2.0f * halfSpan;
    return offset - span * std::floor((offset + halfSpan) / span);
}

std::mt19937& rng() {
    static std::mt19937 gen(std::random_device{}());
    return gen;
}
float uniform(float lo, float hi) {
    return std::uniform_real_distribution<float>(lo, hi)(rng());
}

} // namespace

WeatherMist::~WeatherMist() {
    shutdown();
}

bool WeatherMist::initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout,
                             VkDescriptorSetLayout sceneSetLayout,
                             pipeline::AssetManager* assets, const std::string& texturePath) {
    assert(ctx && assets && sceneSetLayout);
    if (vkCtx_) return true;
    vkCtx_ = ctx;
    perFrameLayout_ = perFrameLayout;
    sceneSetLayout_ = sceneSetLayout;
    if (!texture_.load(*ctx, *assets, texturePath) || !createPipeline()) {
        LOG_ERROR("WeatherMist: initialization failed for ", texturePath);
        shutdown();
        return false;
    }
    for (auto& f : frames_) {
        AllocatedBuffer b = createBuffer(ctx->getAllocator(), kMaxPuffs * 6 * sizeof(MistVertex),
                                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        f.buffer = b.buffer;
        f.alloc = b.allocation;
        f.mapped = b.info.pMappedData;
        if (!f.buffer || !f.mapped) {
            LOG_ERROR("WeatherMist: vertex buffer allocation failed");
            shutdown();
            return false;
        }
    }
    puffs_.reserve(kMaxPuffs);
    return true;
}

void WeatherMist::shutdown() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();
    vkDeviceWaitIdle(device);
    for (auto& f : frames_) {
        destroy(vkCtx_->getAllocator(), f.buffer, f.alloc);
        f = FrameBuffer{};
    }
    texture_.destroy(*vkCtx_);
    destroy(device, pipeline_);
    destroy(device, layout_);
    puffs_.clear();
    vertices_.clear();
    vkCtx_ = nullptr;
}

bool WeatherMist::createPipeline() {
    VkDevice device = vkCtx_->getDevice();
    auto shaders = loadShaderPair(device, "assets/shaders/weather_mist.vert.spv",
                                  "assets/shaders/weather_mist.frag.spv", "weather mist");
    if (!shaders) return false;
    if (!layout_) {
        VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(MistPush)};
        layout_ = createPipelineLayout(device, {perFrameLayout_, texture_.layout(), sceneSetLayout_}, {push});
    }
    if (!layout_) return false;
    VkVertexInputBindingDescription binding{0, sizeof(MistVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    std::vector<VkVertexInputAttributeDescription> attrs{
        {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(MistVertex, position)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(MistVertex, uv)},
    };
    const VkRenderPass rp = targetPass_ ? targetPass_ : vkCtx_->getImGuiRenderPass();
    const VkSampleCountFlagBits samples = targetPass_ ? targetSamples_ : vkCtx_->getMsaaSamples();
    pipeline_ = PipelineBuilder()
        .setShaders(shaders.vertStage, shaders.fragStage)
        .setVertexInput({binding}, attrs)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setDepthTest(true, false, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setColorBlendAttachment(PipelineBuilder::blendAlpha())
        .setMultisample(samples)
        .setLayout(layout_)
        .setRenderPass(rp)
        .setDynamicStates(viewportAndScissorDynamic())
        .build(device, vkCtx_->getPipelineCache());
    shaders.vert.destroy();
    shaders.frag.destroy();
    return pipeline_ != VK_NULL_HANDLE;
}

void WeatherMist::recreatePipeline() {
    if (!vkCtx_) return;
    destroy(vkCtx_->getDevice(), pipeline_);
    if (!createPipeline()) LOG_ERROR("WeatherMist: pipeline recreation failed");
}

void WeatherMist::clear() {
    puffs_.clear();
    vertices_.clear();
    budget_ = 0.0f;
}

void WeatherMist::update(const Camera& camera, float deltaTime, float amount, const Shape& shape) {
    if (!vkCtx_) return;
    assert(shape.farDepth > shape.nearDepth && shape.speed > 0.0f && shape.halfWidth > 0.0f);
    softFadeRange_ = mistSoftFadeRange(shape.size);

    glm::vec3 forward = camera.getForward();
    forward.z = 0.0f;
    if (glm::dot(forward, forward) < 1e-6f) forward = camera.getUp();  // looking straight down or up
    forward.z = 0.0f;
    forward = glm::normalize(forward);
    const glm::vec3 right(forward.y, -forward.x, 0.0f);

    const float spanAlong = shape.farDepth - shape.nearDepth;
    const float pathTime = spanAlong / shape.speed;
    const float targetCount = std::max(amount, 0.0f) * shape.puffsAtFull;

    // Pre-populate the 3D volume around the camera when weather begins (matching
    // rain and snow box population) so foreground and downward-pitched views have
    // immediate mist coverage instead of waiting ~10 s for far puffs to walk in.
    if (puffs_.empty() && targetCount >= 1.0f) {
        const size_t initialCount = std::min<size_t>(
            kMaxPuffs, static_cast<size_t>(std::lround(targetCount)));
        for (size_t i = 0; i < initialCount; ++i) {
            const float along = uniform(shape.nearDepth, shape.farDepth);
            const glm::vec3 pos = forward * along +
                                  right * uniform(-shape.halfWidth, shape.halfWidth) +
                                  glm::vec3(0.0f, 0.0f, uniform(-shape.below, shape.above));
            const float age = std::clamp((shape.farDepth - along) / shape.speed, 0.0f, pathTime);
            puffs_.push_back({pos, age, pathTime, shape.size, shape.fade});
        }
    }

    for (Puff& p : puffs_) {
        p.age += deltaTime;
        p.position -= forward * (shape.speed * deltaTime);
        // Keep lateral & upwind coordinates inside the camera-centered slab when
        // the player yaws the camera, without rigidly locking puffs to screen space.
        const float along = glm::dot(p.position, forward);
        const float across = glm::dot(p.position, right);
        if (std::abs(across) > shape.halfWidth || along > shape.farDepth) {
            const float wrappedAcross = wrapSigned(across, shape.halfWidth);
            const float wrappedAlong = (along > shape.farDepth)
                ? (shape.nearDepth + std::fmod(along - shape.nearDepth, spanAlong))
                : along;
            p.position = forward * wrappedAlong + right * wrappedAcross +
                         glm::vec3(0.0f, 0.0f, p.position.z);
        }
    }
    std::erase_if(puffs_, [&](const Puff& p) {
        return p.age >= p.life || glm::dot(p.position, forward) <= shape.nearDepth;
    });

    budget_ += targetCount / pathTime * deltaTime;
    while (budget_ >= 1.0f && puffs_.size() < kMaxPuffs) {
        const glm::vec3 pos = forward * shape.farDepth +
                              right * uniform(-shape.halfWidth, shape.halfWidth) +
                              glm::vec3(0.0f, 0.0f, uniform(-shape.below, shape.above));
        puffs_.push_back({pos, 0.0f, pathTime, shape.size, shape.fade});
        budget_ -= 1.0f;
    }
    budget_ = std::min(budget_, 1.0f);

    // Camera-facing quads, sorted far to near along planar view depth.
    const glm::vec3 camForward = camera.getForward();
    const glm::vec3 camRight = camera.getRight();
    const glm::vec3 camUp = camera.getUp();
    std::sort(puffs_.begin(), puffs_.end(), [&](const Puff& a, const Puff& b) {
        return glm::dot(a.position, camForward) > glm::dot(b.position, camForward);
    });
    vertices_.clear();
    for (const Puff& p : puffs_) {
        const float viewDepth = glm::dot(p.position, camForward);
        if (viewDepth <= 0.5f) continue;
        const float lifeAlpha = glm::clamp(p.age / p.fade, 0.0f, 1.0f) *
                                glm::clamp((p.life - p.age) / p.fade, 0.0f, 1.0f);
        const float alpha = lifeAlpha * mistNearCameraFade(glm::length(p.position));
        if (alpha <= 0.0f) continue;
        const glm::vec3 c = p.position;
        const glm::vec3 r = camRight * (0.5f * p.size);
        const glm::vec3 u = camUp * (0.5f * p.size);
        const MistVertex v0{c - r - u, alpha, {0, 1}}, v1{c + r - u, alpha, {1, 1}},
                         v2{c + r + u, alpha, {1, 0}}, v3{c - r + u, alpha, {0, 0}};
        vertices_.insert(vertices_.end(), {v0, v1, v2, v0, v2, v3});
    }
}

void WeatherMist::render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const DepthContext& depthCtx) {
    if (!vkCtx_ || vertices_.empty() || !pipeline_ || depthCtx.sceneSet == VK_NULL_HANDLE) return;
    FrameBuffer& f = frames_[vkCtx_->getCurrentFrame()];
    std::memcpy(f.mapped, vertices_.data(), vertices_.size() * sizeof(MistVertex));
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    const VkDescriptorSet sets[] = {perFrameSet, texture_.set(), depthCtx.sceneSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 3, sets, 0, nullptr);
    const float maxFadeRange = (depthCtx.enabled && softFadeRange_ > 0.0f) ? softFadeRange_ : 0.0f;
    MistPush push{
        glm::vec4(depthCtx.nearPlane, depthCtx.farPlane, maxFadeRange, kMistDepthFadeSlope),
        glm::vec4(depthCtx.screenWidth, depthCtx.screenHeight, kMistMinSoftFadeRange, 0.0f),
    };
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &f.buffer, &offset);
    vkCmdDraw(cmd, static_cast<uint32_t>(vertices_.size()), 1, 0, 0);
}

} // namespace wowee::rendering
