#include "rendering/sand_storm.hpp"

#include "core/logger.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/blp_loader.hpp"
#include "rendering/camera.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_utils.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <random>

namespace wowee::rendering {
namespace {

// Measurements on 2.4.3 behind these numbers are in
// _experiments/20261118T171000Z.aq20_sandstorm (workspace root): heavy
// sandstorm (`.wchange 3 1`) in Ruins of Ahn'Qiraj and Silithus.

// Grain and mist motion. Grains and mist puffs come from the camera's
// horizontal forward direction toward it: the grain flow field expands from
// a point on the horizon straight ahead, at any camera yaw, and running does
// not change it, so the effect moves with the camera. The expansion rate is
// what can be measured - speed over distance, 0.6 per second for the typical
// grain 350 px from that point (3.5 px per 60 Hz frame). Speed and distance
// only appear as that ratio on screen; this speed with the spawn depths below
// reproduces it. The box sides are chosen to cover the view at those depths
// (grains show from the bottom of the screen to its top); the original's own
// box cannot be read off the screen.
constexpr float kSpeed = 12.0f;             // yards per second
constexpr float kGrainNear = -16.0f;        // spawn/pass-behind range along horizontal forward, yards
constexpr float kGrainFar = 40.0f;
constexpr float kGrainHalfWidth = 40.0f;    // spawn box across and up/down
constexpr float kGrainBelow = 30.0f;
constexpr float kGrainAbove = 25.0f;
// Live grains at full intensity. The original builds them in packets of
// 6144 (Wow.exe names the type Packet<Sand::Grain,0x1800>). Its sky shows
// 20-80 detected grains per 10^4 px at heavy grade; one packet in this spawn
// box puts 10-17 on the same sky. Four packets' worth live at once is fitted
// to that density; how many packets the original keeps is not known.
constexpr size_t kGrainsPerPacket = 6144;
constexpr float kGrainsAtFull = 4.0f * kGrainsPerPacket;
// sand.bls: size = clamp(distance * c.x + c.y, 0, 1) * c.z. Two pixels up close
// to one (the device minimum) at kGrainFar: 89% of detected grains cover one
// pixel, 7% two, 3% four.
constexpr float kGrainSizePx = 2.0f;

// Mist puffs: WeatherMistGrainy01 quads in the same 3D camera-surrounding slab
// [-28, 110] x [-65, 65] x [-32, 32] as rain/snow mist so downward and horizon
// views both see full sandstorm haze and puff billowing (~70 puffs at full).
constexpr float kPuffNear = -28.0f;
constexpr float kPuffFar = 110.0f;
constexpr float kPuffSize = 36.0f;
constexpr float kPuffHalfWidth = 65.0f;
constexpr float kPuffBelow = 32.0f;
constexpr float kPuffAbove = 32.0f;
// Optical depth 0.0145/yd = puffs per yd^3 * quad area * mean texture alpha
// (46.7/255 for WeatherMistGrainy01), which sets the count across the slab.
constexpr float kMeanTextureAlpha = 46.7f / 255.0f;
constexpr float kOpticalDepthPerYard = 0.0105f;
constexpr float kPuffSlab = (2.0f * kPuffHalfWidth) * (kPuffBelow + kPuffAbove) * (kPuffFar - kPuffNear);
constexpr float kPuffsAtFull =
    kOpticalDepthPerYard * kPuffSlab / (kPuffSize * kPuffSize * kMeanTextureAlpha);

// From clear to heavy the haze arrives 3-4 s after the packet and is complete
// by about 7 s; grains follow at 8-9 s.
constexpr float kRampPerSecond = 1.0f / 7.0f;
// sand.bls fades every grain in over its first 0.2 s and out over its last
// (sand_grain.vert.glsl). Puffs fade smoothly at birth and death.
constexpr float kPuffFade = 0.35f;

constexpr size_t kMaxGrains = 4 * kGrainsPerPacket;

constexpr WeatherMist::Shape kMist{
    .speed = kSpeed, .nearDepth = kPuffNear, .farDepth = kPuffFar, .size = kPuffSize,
    .halfWidth = kPuffHalfWidth, .below = kPuffBelow, .above = kPuffAbove, .fade = kPuffFade,
    .puffsAtFull = kPuffsAtFull};

struct GrainPush {
    glm::vec4 timeAndSize;  // x = now, yzw = sand.bls c_pointSize
};

std::mt19937& rng() {
    static std::mt19937 gen(std::random_device{}());
    return gen;
}
float uniform(float lo, float hi) {
    return std::uniform_real_distribution<float>(lo, hi)(rng());
}

} // namespace

SandStorm::~SandStorm() {
    shutdown();
}

bool SandStorm::initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout,
                           VkDescriptorSetLayout sceneSetLayout,
                           pipeline::AssetManager* assetManager) {
    assert(ctx && assetManager && sceneSetLayout);
    if (vkCtx_) return true;
    vkCtx_ = ctx;
    perFrameLayout_ = perFrameLayout;
    // Named in Wow.exe next to Shaders\Vertex\sand.bls; Weather.dbc's sand rows
    // leave EffectTexture empty.
    if (!mist_.initialize(ctx, perFrameLayout, sceneSetLayout, assetManager, "Textures\\Weather\\WeatherMistGrainy01.blp") ||
        !createPipelines() || !createBuffers()) {
        LOG_ERROR("SandStorm: initialization failed");
        shutdown();
        return false;
    }
    grains_.reserve(kMaxGrains);
    LOG_INFO("SandStorm: initialized, ", kPuffsAtFull, " mist puffs and ", kGrainsAtFull,
             " grains at full intensity");
    return true;
}

void SandStorm::shutdown() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();
    VmaAllocator allocator = vkCtx_->getAllocator();
    vkDeviceWaitIdle(device);
    for (auto& f : frames_) {
        destroy(allocator, f.grains, f.grainsAlloc);
        f = FrameBuffers{};
    }
    mist_.shutdown();
    destroy(device, grainPipeline_);
    destroy(device, grainLayout_);
    grains_.clear();
    vkCtx_ = nullptr;
}

bool SandStorm::createPipelines() {
    VkDevice device = vkCtx_->getDevice();
    auto grainShaders = loadShaderPair(device, "assets/shaders/sand_grain.vert.spv",
                                       "assets/shaders/sand_grain.frag.spv", "sand grain");
    if (!grainShaders) return false;

    if (!grainLayout_) {
        VkPushConstantRange push{};
        push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        push.size = sizeof(GrainPush);
        grainLayout_ = createPipelineLayout(device, {perFrameLayout_}, {push});
    }
    if (!grainLayout_) return false;

    VkVertexInputBindingDescription grainBinding{0, sizeof(Grain), VK_VERTEX_INPUT_RATE_VERTEX};
    std::vector<VkVertexInputAttributeDescription> grainAttrs{
        {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Grain, start)},
        {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Grain, velocity)},
    };
    grainPipeline_ = PipelineBuilder()
        .setShaders(grainShaders.vertStage, grainShaders.fragStage)
        .setVertexInput({grainBinding}, grainAttrs)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_POINT_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setDepthTest(true, false, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setColorBlendAttachment(PipelineBuilder::blendAlpha())
        .setMultisample(vkCtx_->getMsaaSamples())
        .setLayout(grainLayout_)
        .setRenderPass(vkCtx_->getImGuiRenderPass())
        .setDynamicStates(viewportAndScissorDynamic())
        .build(device, vkCtx_->getPipelineCache());

    grainShaders.vert.destroy();
    grainShaders.frag.destroy();
    return grainPipeline_ != VK_NULL_HANDLE;
}

void SandStorm::recreatePipelines() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();
    destroy(device, grainPipeline_);
    if (!createPipelines()) LOG_ERROR("SandStorm: pipeline recreation failed");
    mist_.recreatePipeline();
}

bool SandStorm::createBuffers() {
    for (auto& f : frames_) {
        AllocatedBuffer g = createBuffer(vkCtx_->getAllocator(), kMaxGrains * sizeof(Grain),
                                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        f.grains = g.buffer;
        f.grainsAlloc = g.allocation;
        f.grainsMapped = g.info.pMappedData;
        if (!f.grains || !f.grainsMapped) return false;
    }
    return true;
}

void SandStorm::spawnGrain(const glm::vec3& forward, const glm::vec3& right) {
    const float depth = uniform(kGrainNear, kGrainFar);
    const glm::vec3 start = forward * depth + right * uniform(-kGrainHalfWidth, kGrainHalfWidth) +
                            glm::vec3(0.0f, 0.0f, uniform(-kGrainBelow, kGrainAbove));
    // A grain travels all the way past kGrainNear so downward camera views stay populated.
    const float life = std::max((depth - kGrainNear) / kSpeed, 0.25f);
    grains_.push_back({start, time_, -forward * kSpeed, time_ + life});
}

void SandStorm::update(const Camera& camera, float deltaTime, float targetIntensity) {
    if (!vkCtx_) return;
    time_ += deltaTime;
    const float step = kRampPerSecond * deltaTime;
    intensity_ += glm::clamp(targetIntensity - intensity_, -step, step);

    std::erase_if(grains_, [this](const Grain& g) { return g.death <= time_; });

    glm::vec3 forward = camera.getForward();
    forward.z = 0.0f;
    if (glm::dot(forward, forward) < 1e-6f) forward = camera.getUp();  // looking straight down or up
    forward.z = 0.0f;
    forward = glm::normalize(forward);
    const glm::vec3 right(forward.y, -forward.x, 0.0f);

    const float amount = intensity_ * densityScale_;
    // Grains: a steady stream whose live count is proportional to intensity.
    const float meanLife = 0.5f * (kGrainFar - kGrainNear) / kSpeed;
    grainBudget_ += amount * kGrainsAtFull / meanLife * deltaTime;
    while (grainBudget_ >= 1.0f && grains_.size() < kMaxGrains) {
        spawnGrain(forward, right);
        grainBudget_ -= 1.0f;
    }
    grainBudget_ = std::min(grainBudget_, 1.0f);

    mist_.update(camera, deltaTime, amount, kMist);
}

void SandStorm::render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet,
                       bool includeMist, const WeatherMist::DepthContext& mistDepth) {
    if (!vkCtx_) return;
    if (includeMist) mist_.render(cmd, perFrameSet, mistDepth);
    FrameBuffers& f = frames_[vkCtx_->getCurrentFrame()];
    VkDeviceSize offset = 0;
    if (!grains_.empty()) {
        std::memcpy(f.grainsMapped, grains_.data(), grains_.size() * sizeof(Grain));
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, grainPipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, grainLayout_, 0, 1, &perFrameSet, 0, nullptr);
        GrainPush push{};
        push.timeAndSize = glm::vec4(time_, -1.0f / kGrainFar, 1.0f, kGrainSizePx);
        vkCmdPushConstants(cmd, grainLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
        vkCmdBindVertexBuffers(cmd, 0, 1, &f.grains, &offset);
        vkCmdDraw(cmd, static_cast<uint32_t>(grains_.size()), 1, 0, 0);
    }
}

void SandStorm::renderMist(VkCommandBuffer cmd, VkDescriptorSet perFrameSet,
                           const WeatherMist::DepthContext& mistDepth) {
    if (!vkCtx_) return;
    mist_.render(cmd, perFrameSet, mistDepth);
}

} // namespace wowee::rendering
