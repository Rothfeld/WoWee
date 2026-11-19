#include "rendering/weather_mist.hpp"

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
                             pipeline::AssetManager* assets, const std::string& texturePath) {
    assert(ctx && assets);
    if (vkCtx_) return true;
    vkCtx_ = ctx;
    perFrameLayout_ = perFrameLayout;
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
    if (!layout_) layout_ = createPipelineLayout(device, {perFrameLayout_, texture_.layout()}, {});
    if (!layout_) return false;
    VkVertexInputBindingDescription binding{0, sizeof(MistVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    std::vector<VkVertexInputAttributeDescription> attrs{
        {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(MistVertex, position)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(MistVertex, uv)},
    };
    pipeline_ = PipelineBuilder()
        .setShaders(shaders.vertStage, shaders.fragStage)
        .setVertexInput({binding}, attrs)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setDepthTest(true, false, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setColorBlendAttachment(PipelineBuilder::blendAlpha())
        .setMultisample(vkCtx_->getMsaaSamples())
        .setLayout(layout_)
        .setRenderPass(vkCtx_->getImGuiRenderPass())
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
    time_ += deltaTime;
    std::erase_if(puffs_, [this](const Puff& p) { return p.death <= time_; });

    glm::vec3 forward = camera.getForward();
    forward.z = 0.0f;
    if (glm::dot(forward, forward) < 1e-6f) forward = camera.getUp();  // looking straight down or up
    forward.z = 0.0f;
    forward = glm::normalize(forward);
    const glm::vec3 right(forward.y, -forward.x, 0.0f);

    // Born at the far end at the rate that keeps amount * the full count on
    // the path, so a weather that starts fills in from the distance.
    const float pathTime = (shape.farDepth - shape.nearDepth) / shape.speed;
    budget_ += amount * shape.puffsAtFull / pathTime * deltaTime;
    while (budget_ >= 1.0f && puffs_.size() < kMaxPuffs) {
        const glm::vec3 start = forward * shape.farDepth +
                                right * uniform(-shape.halfWidth, shape.halfWidth) +
                                glm::vec3(0.0f, 0.0f, uniform(-shape.below, shape.above));
        puffs_.push_back({start, time_, -forward * shape.speed, time_ + pathTime, shape.size, shape.fade});
        budget_ -= 1.0f;
    }
    budget_ = std::min(budget_, 1.0f);

    // Camera-facing quads, far to near.
    const glm::vec3 camRight = camera.getRight();
    const glm::vec3 camUp = camera.getUp();
    std::sort(puffs_.begin(), puffs_.end(), [this](const Puff& a, const Puff& b) {
        const glm::vec3 pa = a.start + a.velocity * (time_ - a.birth);
        const glm::vec3 pb = b.start + b.velocity * (time_ - b.birth);
        return glm::dot(pa, pa) > glm::dot(pb, pb);
    });
    vertices_.clear();
    for (const Puff& p : puffs_) {
        const float age = time_ - p.birth;
        const float life = p.death - p.birth;
        const float alpha = glm::clamp(age / p.fade, 0.0f, 1.0f) *
                            glm::clamp((life - age) / p.fade, 0.0f, 1.0f);
        if (alpha <= 0.0f) continue;
        const glm::vec3 c = p.start + p.velocity * age;
        const glm::vec3 r = camRight * (0.5f * p.size);
        const glm::vec3 u = camUp * (0.5f * p.size);
        const MistVertex v0{c - r - u, alpha, {0, 1}}, v1{c + r - u, alpha, {1, 1}},
                         v2{c + r + u, alpha, {1, 0}}, v3{c - r + u, alpha, {0, 0}};
        vertices_.insert(vertices_.end(), {v0, v1, v2, v0, v2, v3});
    }
}

void WeatherMist::render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) {
    if (!vkCtx_ || vertices_.empty() || !pipeline_) return;
    FrameBuffer& f = frames_[vkCtx_->getCurrentFrame()];
    std::memcpy(f.mapped, vertices_.data(), vertices_.size() * sizeof(MistVertex));
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    const VkDescriptorSet sets[] = {perFrameSet, texture_.set()};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 2, sets, 0, nullptr);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &f.buffer, &offset);
    vkCmdDraw(cmd, static_cast<uint32_t>(vertices_.size()), 1, 0, 0);
}

} // namespace wowee::rendering
