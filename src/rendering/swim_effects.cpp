#include "rendering/swim_effects.hpp"
#include "rendering/camera.hpp"
#include "rendering/camera_controller.hpp"
#include "rendering/water_renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_frame_data.hpp"
#include "rendering/vk_utils.hpp"
#include "core/logger.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <random>
#include <cmath>
#include <cstring>

namespace wowee {
namespace rendering {

static std::mt19937& rng() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    return gen;
}

static float randFloat(float lo, float hi) {
    std::uniform_real_distribution<float> dist(lo, hi);
    return dist(rng());
}

SwimEffects::SwimEffects() = default;
SwimEffects::~SwimEffects() { shutdown(); }

/// The ripple pipeline: the rings that spread where a swimmer breaks the
/// surface.
///
/// initialize() and recreatePipelines() described these three identically,
/// so each was stated twice in one file.
void SwimEffects::buildRipplePipeline(
        VkDevice device, const VkPipelineShaderStageCreateInfo& vertStage,
        const VkPipelineShaderStageCreateInfo& fragStage,
        const VkVertexInputBindingDescription& binding,
        const std::vector<VkVertexInputAttributeDescription>& attrs) {
    const std::vector<VkDynamicState> dynamicStates = viewportAndScissorDynamic();

    ripplePipeline = PipelineBuilder()
        .setShaders(vertStage, fragStage)
        .setVertexInput({binding}, attrs)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_POINT_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setDepthTest(true, false, VK_COMPARE_OP_LESS)
        .setColorBlendAttachment(PipelineBuilder::blendAlpha())
        .setMultisample(targetSamples_)
        .setLayout(ripplePipelineLayout)
        .setRenderPass(targetPass_)
        .setDynamicStates(dynamicStates)
        .build(device, vkCtx->getPipelineCache());
}

/// The bubble pipeline: what rises from a swimmer under the surface.
void SwimEffects::buildBubblePipeline(
        VkDevice device, const VkPipelineShaderStageCreateInfo& vertStage,
        const VkPipelineShaderStageCreateInfo& fragStage,
        const VkVertexInputBindingDescription& binding,
        const std::vector<VkVertexInputAttributeDescription>& attrs) {
    const std::vector<VkDynamicState> dynamicStates = viewportAndScissorDynamic();

    bubblePipeline = PipelineBuilder()
        .setShaders(vertStage, fragStage)
        .setVertexInput({binding}, attrs)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_POINT_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setDepthTest(true, false, VK_COMPARE_OP_LESS)
        .setColorBlendAttachment(PipelineBuilder::blendAlpha())
        .setMultisample(targetSamples_)
        .setLayout(bubblePipelineLayout)
        .setRenderPass(targetPass_)
        .setDynamicStates(dynamicStates)
        .build(device, vkCtx->getPipelineCache());
}

bool SwimEffects::initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout) {
    LOG_INFO("Initializing swim effects");

    vkCtx = ctx;
    VkDevice device = vkCtx->getDevice();

    // Fall back to drawing inside the scene pass when no target has been
    // selected, which is the arrangement that predates water having its own.
    if (targetPass_ == VK_NULL_HANDLE) {
        targetPass_ = vkCtx->getImGuiRenderPass();
        targetSamples_ = vkCtx->getMsaaSamples();
    }

    // ---- Vertex input: pos(vec3) + size(float) + alpha(float) = 5 floats, stride = 20 bytes ----
    VkVertexInputBindingDescription binding = tightVertexBinding(5 * sizeof(float));
    std::vector<VkVertexInputAttributeDescription> attrs = positionPlusTwoFloatsAttrs();


    // ---- Ripple pipeline ----
    {
        auto shaders = loadShaderPair(device, "assets/shaders/swim_ripple.vert.spv", "assets/shaders/swim_ripple.frag.spv", "swim_ripple");
        if (!shaders) return false;
        const auto& vertStage = shaders.vertStage;
        const auto& fragStage = shaders.fragStage;

        ripplePipelineLayout = createPipelineLayout(device, {perFrameLayout}, {});
        if (ripplePipelineLayout == VK_NULL_HANDLE) {
            LOG_ERROR("Failed to create ripple pipeline layout");
            return false;
        }

        buildRipplePipeline(device, vertStage, fragStage, binding, attrs);


        if (ripplePipeline == VK_NULL_HANDLE) {
            LOG_ERROR("Failed to create ripple pipeline");
            return false;
        }
    }

    // ---- Bubble pipeline ----
    {
        auto shaders = loadShaderPair(device, "assets/shaders/swim_bubble.vert.spv", "assets/shaders/swim_bubble.frag.spv", "swim_bubble");
        if (!shaders) return false;
        const auto& vertStage = shaders.vertStage;
        const auto& fragStage = shaders.fragStage;

        bubblePipelineLayout = createPipelineLayout(device, {perFrameLayout}, {});
        if (bubblePipelineLayout == VK_NULL_HANDLE) {
            LOG_ERROR("Failed to create bubble pipeline layout");
            return false;
        }

        buildBubblePipeline(device, vertStage, fragStage, binding, attrs);


        if (bubblePipeline == VK_NULL_HANDLE) {
            LOG_ERROR("Failed to create bubble pipeline");
            return false;
        }
    }

    // ---- Create dynamic mapped vertex buffers ----
    rippleDynamicVBSize = MAX_RIPPLE_PARTICLES * 5 * sizeof(float);
    {
        AllocatedBuffer buf = createBuffer(vkCtx->getAllocator(), rippleDynamicVBSize,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        rippleDynamicVB = buf.buffer;
        rippleDynamicVBAlloc = buf.allocation;
        rippleDynamicVBAllocInfo = buf.info;
        if (rippleDynamicVB == VK_NULL_HANDLE) {
            LOG_ERROR("Failed to create ripple dynamic vertex buffer");
            return false;
        }
    }

    bubbleDynamicVBSize = MAX_BUBBLE_PARTICLES * 5 * sizeof(float);
    {
        AllocatedBuffer buf = createBuffer(vkCtx->getAllocator(), bubbleDynamicVBSize,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        bubbleDynamicVB = buf.buffer;
        bubbleDynamicVBAlloc = buf.allocation;
        bubbleDynamicVBAllocInfo = buf.info;
        if (bubbleDynamicVB == VK_NULL_HANDLE) {
            LOG_ERROR("Failed to create bubble dynamic vertex buffer");
            return false;
        }
    }

    ripples.reserve(MAX_RIPPLE_PARTICLES);
    bubbles.reserve(MAX_BUBBLE_PARTICLES);
    rippleVertexData.reserve(MAX_RIPPLE_PARTICLES * 5);
    bubbleVertexData.reserve(MAX_BUBBLE_PARTICLES * 5);

    LOG_INFO("Swim effects initialized");
    return true;
}

void SwimEffects::shutdown() {
    if (vkCtx) {
        VkDevice device = vkCtx->getDevice();
        VmaAllocator allocator = vkCtx->getAllocator();

        destroy(device, ripplePipeline);
        destroy(device, ripplePipelineLayout);
        destroy(allocator, rippleDynamicVB, rippleDynamicVBAlloc);

        destroy(device, bubblePipeline);
        destroy(device, bubblePipelineLayout);
        destroy(allocator, bubbleDynamicVB, bubbleDynamicVBAlloc);
    }

    vkCtx = nullptr;
    ripples.clear();
    bubbles.clear();
}

void SwimEffects::recreatePipelines() {
    if (!vkCtx) return;
    VkDevice device = vkCtx->getDevice();

    // Fall back to drawing inside the scene pass when no target has been
    // selected, which is the arrangement that predates water having its own.
    if (targetPass_ == VK_NULL_HANDLE) {
        targetPass_ = vkCtx->getImGuiRenderPass();
        targetSamples_ = vkCtx->getMsaaSamples();
    }

    // Destroy old pipelines (NOT layouts)
    destroy(device, ripplePipeline);
    destroy(device, bubblePipeline);

    // Shared vertex input: pos(vec3) + size(float) + alpha(float) = 5 floats
    VkVertexInputBindingDescription binding = tightVertexBinding(5 * sizeof(float));
    std::vector<VkVertexInputAttributeDescription> attrs = positionPlusTwoFloatsAttrs();


    // ---- Rebuild ripple pipeline ----
    {
        VkShaderModule vertModule;
        (void)vertModule.loadFromFile(device, "assets/shaders/swim_ripple.vert.spv");
        VkShaderModule fragModule;
        (void)fragModule.loadFromFile(device, "assets/shaders/swim_ripple.frag.spv");

        VkPipelineShaderStageCreateInfo vertStage = vertModule.stageInfo(VK_SHADER_STAGE_VERTEX_BIT);
        VkPipelineShaderStageCreateInfo fragStage = fragModule.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT);

        buildRipplePipeline(device, vertStage, fragStage, binding, attrs);

    }

    // ---- Rebuild bubble pipeline ----
    {
        VkShaderModule vertModule;
        (void)vertModule.loadFromFile(device, "assets/shaders/swim_bubble.vert.spv");
        VkShaderModule fragModule;
        (void)fragModule.loadFromFile(device, "assets/shaders/swim_bubble.frag.spv");

        VkPipelineShaderStageCreateInfo vertStage = vertModule.stageInfo(VK_SHADER_STAGE_VERTEX_BIT);
        VkPipelineShaderStageCreateInfo fragStage = fragModule.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT);

        buildBubblePipeline(device, vertStage, fragStage, binding, attrs);

    }

}

void SwimEffects::spawnRipple(const glm::vec3& pos, const glm::vec3& moveDir, float waterH) {
    if (static_cast<int>(ripples.size()) >= MAX_RIPPLE_PARTICLES) return;

    Particle p;
    // Scatter splash droplets around the character at the water surface
    float ox = randFloat(-1.5f, 1.5f);
    float oy = randFloat(-1.5f, 1.5f);
    p.position = glm::vec3(pos.x + ox, pos.y + oy, waterH + 0.3f);

    // Spray outward + upward from movement direction
    float spread = randFloat(-1.0f, 1.0f);
    glm::vec3 perp(-moveDir.y, moveDir.x, 0.0f);
    glm::vec3 outDir = -moveDir + perp * spread;
    float speed = randFloat(1.5f, 4.0f);
    p.velocity = glm::vec3(outDir.x * speed, outDir.y * speed, randFloat(1.0f, 3.0f));

    p.lifetime = 0.0f;
    p.maxLifetime = randFloat(0.5f, 1.0f);
    p.size = randFloat(3.0f, 7.0f);
    p.alpha = randFloat(0.5f, 0.8f);

    ripples.push_back(p);
}

void SwimEffects::spawnFootSplash(const glm::vec3& footPos, float waterH) {
    // Small burst of splash droplets at foot position (for wading)
    constexpr int splashCount = 5;
    for (int i = 0; i < splashCount; ++i) {
        if (static_cast<int>(ripples.size()) >= MAX_RIPPLE_PARTICLES) break;
        Particle p;
        float ox = randFloat(-0.4f, 0.4f);
        float oy = randFloat(-0.4f, 0.4f);
        p.position = glm::vec3(footPos.x + ox, footPos.y + oy, waterH + 0.1f);
        // Small upward spray in random horizontal direction
        float angle = randFloat(0.0f, 6.2832f);
        float speed = randFloat(0.8f, 2.0f);
        p.velocity = glm::vec3(std::cos(angle) * speed, std::sin(angle) * speed, randFloat(1.0f, 2.5f));
        p.lifetime = 0.0f;
        p.maxLifetime = randFloat(0.3f, 0.6f);
        p.size = randFloat(2.0f, 4.0f);
        p.alpha = randFloat(0.4f, 0.7f);
        ripples.push_back(p);
    }
}

void SwimEffects::spawnBubble(const glm::vec3& pos, float /*waterH*/) {
    if (static_cast<int>(bubbles.size()) >= MAX_BUBBLE_PARTICLES) return;

    Particle p;
    float ox = randFloat(-3.0f, 3.0f);
    float oy = randFloat(-3.0f, 3.0f);
    float oz = randFloat(-2.0f, 0.0f);
    p.position = glm::vec3(pos.x + ox, pos.y + oy, pos.z + oz);

    p.velocity = glm::vec3(randFloat(-0.3f, 0.3f), randFloat(-0.3f, 0.3f), randFloat(4.0f, 8.0f));
    p.lifetime = 0.0f;
    p.maxLifetime = randFloat(2.0f, 3.5f);
    p.size = randFloat(6.0f, 12.0f);
    p.alpha = 0.6f;

    bubbles.push_back(p);
}

void SwimEffects::update(const Camera& camera, const CameraController& cc,
                         const WaterRenderer& water, float deltaTime) {
    glm::vec3 camPos = camera.getPosition();

    // Use character position for ripples in third-person mode
    glm::vec3 charPos = camPos;
    const glm::vec3* followTarget = cc.getFollowTarget();
    if (cc.isThirdPerson() && followTarget) {
        charPos = *followTarget;
    }

    // Check water at character position (for ripples) and camera position (for bubbles),
    // honoring roof/hull displacement so dry below-sea-level rooms and ship holds stay dry.
    auto charWaterH = water.getNearestWaterHeightAt(charPos.x, charPos.y, charPos.z, 400.0f);
    auto camWaterH = water.getNearestWaterHeightAt(camPos.x, camPos.y, camPos.z, 400.0f);

    bool swimming = cc.isSwimming();
    bool moving = cc.isMoving();

    // --- Ripple/splash spawning ---
    if (swimming && charWaterH) {
        float wh = *charWaterH;
        float spawnRate = moving ? 40.0f : 8.0f;
        rippleSpawnAccum += spawnRate * deltaTime;

        // Compute movement direction from camera yaw
        float yawRad = glm::radians(cc.getYaw());
        glm::vec3 moveDir(std::cos(yawRad), std::sin(yawRad), 0.0f);
        if (moveDir.x * moveDir.x + moveDir.y * moveDir.y > 1e-6f) {
            moveDir = glm::normalize(moveDir);
        }

        while (rippleSpawnAccum >= 1.0f) {
            spawnRipple(charPos, moveDir, wh);
            rippleSpawnAccum -= 1.0f;
        }
    } else {
        rippleSpawnAccum = 0.0f;
        // Don't clear ripples - foot splash particles are added while wading
        // (not swimming) and need to live out their lifetime.
    }

    // --- Wading spray ---
    // Feet in the water but not swimming: the shoreline transition. Ripples
    // above only spawn while swimming, so walking or riding through the
    // shallows threw nothing but the per-footstep burst, which reads as dry.
    // Rate rises with depth, since deeper water is displaced harder, and dies
    // off as the surface approaches the character's waist, by which point
    // swimming takes over.
    if (!swimming && moving && charWaterH) {
        const float depth = *charWaterH - charPos.z;
        constexpr float kWadeMinDepth = 0.03f;
        constexpr float kWadeMaxDepth = 1.60f;
        if (depth > kWadeMinDepth && depth < kWadeMaxDepth) {
            const float depthScale = glm::clamp(depth / 0.7f, 0.30f, 1.0f);
            // Its own accumulator: the swim branch above zeroes rippleSpawnAccum
            // on every frame it is not swimming, so a wading rate of 30/s could
            // only ever reach 0.5 in a frame and never crossed the threshold.
            wadeSpawnAccum += 30.0f * depthScale * deltaTime;

            // Forward from yaw is (cos, sin) - the same derivation the camera
            // controller uses to move the character.
            const float yawRad = glm::radians(cc.getYaw());
            const glm::vec2 travel(std::cos(yawRad), std::sin(yawRad));

            while (wadeSpawnAccum >= 1.0f) {
                wadeSpawnAccum -= 1.0f;
                if (static_cast<int>(ripples.size()) >= MAX_RIPPLE_PARTICLES) break;
                Particle p;
                // Around the legs, biased behind so the spray trails the stride.
                const float side = randFloat(-0.45f, 0.45f);
                const float back = randFloat(-0.55f, 0.15f);
                p.position = glm::vec3(charPos.x + travel.x * back - travel.y * side,
                                       charPos.y + travel.y * back + travel.x * side,
                                       *charWaterH + 0.05f);
                const float angle = randFloat(0.0f, 6.2832f);
                const float outward = randFloat(0.5f, 1.6f) * depthScale;
                p.velocity = glm::vec3(std::cos(angle) * outward - travel.x * 0.8f,
                                       std::sin(angle) * outward - travel.y * 0.8f,
                                       randFloat(0.9f, 2.2f) * depthScale);
                p.lifetime = 0.0f;
                p.maxLifetime = randFloat(0.25f, 0.55f);
                p.size = randFloat(1.6f, 3.4f) * depthScale;
                p.alpha = randFloat(0.35f, 0.65f) * depthScale;
                ripples.push_back(p);
            }
        } else {
            wadeSpawnAccum = 0.0f;
        }
    } else {
        wadeSpawnAccum = 0.0f;
    }

    // --- Bubble spawning ---
    // Require swimming state to prevent spurious bubbles on login/teleport
    // when camera may briefly appear below a water surface before grounding.
    bool underwater = swimming && camWaterH && camPos.z < *camWaterH;
    if (underwater) {
        float bubbleRate = 20.0f;
        bubbleSpawnAccum += bubbleRate * deltaTime;
        while (bubbleSpawnAccum >= 1.0f) {
            spawnBubble(camPos, *camWaterH);
            bubbleSpawnAccum -= 1.0f;
        }
    } else {
        bubbleSpawnAccum = 0.0f;
        bubbles.clear();
    }

    // --- Update ripples (splash droplets with gravity) ---
    for (int i = static_cast<int>(ripples.size()) - 1; i >= 0; --i) {
        auto& p = ripples[i];
        p.lifetime += deltaTime;
        if (p.lifetime >= p.maxLifetime) {
            ripples[i] = ripples.back();
            ripples.pop_back();
            continue;
        }
        // Apply gravity to splash droplets
        p.velocity.z -= 9.8f * deltaTime;
        p.position += p.velocity * deltaTime;

        // Kill if fallen back below water
        float surfaceZ = charWaterH ? *charWaterH : 0.0f;
        if (p.position.z < surfaceZ && p.lifetime > 0.1f) {
            ripples[i] = ripples.back();
            ripples.pop_back();
            continue;
        }

        float t = p.lifetime / p.maxLifetime;
        p.alpha = glm::mix(0.7f, 0.0f, t);
        p.size = glm::mix(5.0f, 2.0f, t);
    }

    // --- Update bubbles ---
    float bubbleCeilH = camWaterH ? *camWaterH : 0.0f;
    for (int i = static_cast<int>(bubbles.size()) - 1; i >= 0; --i) {
        auto& p = bubbles[i];
        p.lifetime += deltaTime;
        if (p.lifetime >= p.maxLifetime || p.position.z >= bubbleCeilH) {
            bubbles[i] = bubbles.back();
            bubbles.pop_back();
            continue;
        }
        // Wobble
        float wobbleX = std::sin(p.lifetime * 3.0f) * 0.5f;
        float wobbleY = std::cos(p.lifetime * 2.5f) * 0.5f;
        p.position += (p.velocity + glm::vec3(wobbleX, wobbleY, 0.0f)) * deltaTime;

        float t = p.lifetime / p.maxLifetime;
        if (t > 0.8f) {
            p.alpha = 0.6f * (1.0f - (t - 0.8f) / 0.2f);
        } else {
            p.alpha = 0.6f;
        }
    }

    // --- Build vertex data ---
    rippleVertexData.clear();
    rippleVertexData.reserve(ripples.size() * 5);
    for (const auto& p : ripples) {
        rippleVertexData.push_back(p.position.x);
        rippleVertexData.push_back(p.position.y);
        rippleVertexData.push_back(p.position.z);
        rippleVertexData.push_back(p.size);
        rippleVertexData.push_back(p.alpha);
    }

    bubbleVertexData.clear();
    bubbleVertexData.reserve(bubbles.size() * 5);
    for (const auto& p : bubbles) {
        bubbleVertexData.push_back(p.position.x);
        bubbleVertexData.push_back(p.position.y);
        bubbleVertexData.push_back(p.position.z);
        bubbleVertexData.push_back(p.size);
        bubbleVertexData.push_back(p.alpha);
    }

}

void SwimEffects::render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) {
    if (rippleVertexData.empty() && bubbleVertexData.empty()) return;

    VkDeviceSize offset = 0;

    // --- Render ripples (splash droplets above water surface) ---
    if (!rippleVertexData.empty() && ripplePipeline != VK_NULL_HANDLE) {
        VkDeviceSize uploadSize = rippleVertexData.size() * sizeof(float);
        if (rippleDynamicVBAllocInfo.pMappedData) {
            std::memcpy(rippleDynamicVBAllocInfo.pMappedData, rippleVertexData.data(), uploadSize);
        }

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ripplePipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ripplePipelineLayout,
            0, 1, &perFrameSet, 0, nullptr);
        vkCmdBindVertexBuffers(cmd, 0, 1, &rippleDynamicVB, &offset);
        vkCmdDraw(cmd, static_cast<uint32_t>(rippleVertexData.size() / 5), 1, 0, 0);
    }

    // --- Render bubbles ---
    if (!bubbleVertexData.empty() && bubblePipeline != VK_NULL_HANDLE) {
        VkDeviceSize uploadSize = bubbleVertexData.size() * sizeof(float);
        if (bubbleDynamicVBAllocInfo.pMappedData) {
            std::memcpy(bubbleDynamicVBAllocInfo.pMappedData, bubbleVertexData.data(), uploadSize);
        }

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bubblePipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bubblePipelineLayout,
            0, 1, &perFrameSet, 0, nullptr);
        vkCmdBindVertexBuffers(cmd, 0, 1, &bubbleDynamicVB, &offset);
        vkCmdDraw(cmd, static_cast<uint32_t>(bubbleVertexData.size() / 5), 1, 0, 0);
    }

}

} // namespace rendering
} // namespace wowee
