#include "rendering/renderer_bloom.hpp"
#include "rendering/renderer_bloom_math.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_utils.hpp"
#include "core/logger.hpp"
#include "core/profiler.hpp"
#include <glm/glm.hpp>
#include <algorithm>

namespace wowee::rendering {

namespace {

constexpr VkFormat kCopyFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat kBloomFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr uint32_t kBlurGroup = 8;

struct BlurPush {
    glm::vec2 axisStepUV{0.0f};
    float invSigmaNorm = 1.0f;
    float pad = 0.0f;
};

struct CompositePush {
    float glow = 0.0f;
};

}  // namespace

RendererBloom::~RendererBloom() {
    shutdown();
}

bool RendererBloom::initialize(VkContext* ctx) {
    if (!ctx) return false;
    ctx_ = ctx;
    VkDevice device = ctx_->getDevice();

    const auto has = [&](VkFormat format, VkFormatFeatureFlags needed) {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(ctx_->getPhysicalDevice(), format, &props);
        return (props.optimalTilingFeatures & needed) == needed;
    };
    if (!has(ctx_->getSwapchainFormat(), VK_FORMAT_FEATURE_BLIT_SRC_BIT) ||
        !has(kCopyFormat, VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
                          VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ||
        !has(kBloomFormat, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
                           VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) {
        LOG_WARNING("RendererBloom: required blit/storage image features missing - ffxGlow unavailable");
        return false;
    }

    VkSamplerCreateInfo sampCI{};
    sampCI.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampCI.magFilter = VK_FILTER_LINEAR;
    sampCI.minFilter = VK_FILTER_LINEAR;
    sampCI.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ = ctx_->getOrCreateSampler(sampCI);
    if (sampler_ == VK_NULL_HANDLE) return false;

    VkDescriptorSetLayoutBinding blurBindings[2]{};
    blurBindings[0] = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                       .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    blurBindings[1] = {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                       .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    blurSetLayout_ = createDescriptorSetLayout(device, {blurBindings[0], blurBindings[1]});

    VkDescriptorSetLayoutBinding compBinding{
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    compositeSetLayout_ = createDescriptorSetLayout(device, {compBinding});
    if (blurSetLayout_ == VK_NULL_HANDLE || compositeSetLayout_ == VK_NULL_HANDLE) {
        LOG_ERROR("RendererBloom: failed to create descriptor set layouts");
        return false;
    }

    const VkPushConstantRange blurPush{.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                                       .offset = 0, .size = sizeof(BlurPush)};
    const VkPushConstantRange compPush{.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
                                       .offset = 0, .size = sizeof(CompositePush)};
    blurLayout_ = createPipelineLayout(device, {blurSetLayout_}, {blurPush});
    compositeLayout_ = createPipelineLayout(device, {compositeSetLayout_}, {compPush});
    if (blurLayout_ == VK_NULL_HANDLE || compositeLayout_ == VK_NULL_HANDLE) {
        LOG_ERROR("RendererBloom: failed to create pipeline layouts");
        return false;
    }

    {
        VkShaderModule module;
        if (!module.loadFromFile(device, "assets/shaders/bloom_blur.comp.spv")) {
            LOG_ERROR("RendererBloom: failed to load bloom_blur.comp.spv");
            return false;
        }
        VkComputePipelineCreateInfo cpCI{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpCI.stage = module.stageInfo(VK_SHADER_STAGE_COMPUTE_BIT);
        cpCI.layout = blurLayout_;
        const bool ok = vkCreateComputePipelines(device, ctx_->getPipelineCache(), 1, &cpCI,
                                                 nullptr, &blurPipeline_) == VK_SUCCESS;
        module.destroy();
        if (!ok) {
            LOG_ERROR("RendererBloom: failed to create blur compute pipeline");
            return false;
        }
    }

    VkDescriptorPoolSize sizes[2] = {
        {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = MAX_FRAMES * 3},
        {.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = MAX_FRAMES * 2},
    };
    VkDescriptorPoolCreateInfo poolCI{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolCI.maxSets = MAX_FRAMES * 3;
    poolCI.poolSizeCount = 2;
    poolCI.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(device, &poolCI, nullptr, &descPool_) != VK_SUCCESS) {
        LOG_ERROR("RendererBloom: failed to create descriptor pool");
        return false;
    }

    for (uint32_t i = 0; i < MAX_FRAMES; ++i) {
        VkDescriptorSetAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = descPool_;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &blurSetLayout_;
        if (vkAllocateDescriptorSets(device, &alloc, &blurHSets_[i]) != VK_SUCCESS) return false;
        if (vkAllocateDescriptorSets(device, &alloc, &blurVSets_[i]) != VK_SUCCESS) return false;
        alloc.pSetLayouts = &compositeSetLayout_;
        if (vkAllocateDescriptorSets(device, &alloc, &compositeSets_[i]) != VK_SUCCESS) return false;
    }

    if (!ensureCompositePipeline()) return false;

    usable_ = true;
    return true;
}

bool RendererBloom::ensureCompositePipeline() {
    const VkFormat format = ctx_->getSwapchainFormat();
    if (compositePipeline_ != VK_NULL_HANDLE && format == compositeFormat_) return true;
    VkDevice device = ctx_->getDevice();
    if (compositePipeline_ != VK_NULL_HANDLE) vkDeviceWaitIdle(device);
    destroy(device, compositePipeline_);

    ShaderPair shaders = loadShaderPair(device, "assets/shaders/postprocess.vert.spv",
                                        "assets/shaders/bloom_composite.frag.spv", "bloom");
    if (!shaders) return false;

    // Pure additive blend (`ffxglow.bls`: result.xyz = scene.xyz + glow * blur^2,
    // clamped to [0, 1] by the UNORM color target). Alpha is left untouched.
    VkPipelineColorBlendAttachmentState add{};
    add.blendEnable = VK_TRUE;
    add.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    add.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    add.colorBlendOp = VK_BLEND_OP_ADD;
    add.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    add.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    add.alphaBlendOp = VK_BLEND_OP_ADD;
    add.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;

    compositePipeline_ = PipelineBuilder()
        .setShaders(shaders.vertStage, shaders.fragStage)
        .setVertexInput({}, {})
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setNoDepthTest()
        .setColorBlendAttachment(add)
        .setMultisample(VK_SAMPLE_COUNT_1_BIT)
        .setLayout(compositeLayout_)
        .setRenderPass(ctx_->getOverlayRenderPass())
        .setDynamicStates({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR})
        .build(device, ctx_->getPipelineCache());
    if (compositePipeline_ == VK_NULL_HANDLE) {
        LOG_ERROR("RendererBloom: failed to create composite pipeline");
        return false;
    }
    compositeFormat_ = format;
    return true;
}

bool RendererBloom::createTarget(Target& t, VkExtent2D extent, VkFormat format, VkImageUsageFlags usage) {
    VkImageCreateInfo imgCI{};
    imgCI.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgCI.imageType = VK_IMAGE_TYPE_2D;
    imgCI.format = format;
    imgCI.extent = {.width = extent.width, .height = extent.height, .depth = 1};
    imgCI.mipLevels = 1;
    imgCI.arrayLayers = 1;
    imgCI.samples = VK_SAMPLE_COUNT_1_BIT;
    imgCI.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgCI.usage = usage;
    imgCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo allocCI{};
    allocCI.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    if (vmaCreateImage(ctx_->getAllocator(), &imgCI, &allocCI, &t.image, &t.alloc, nullptr) != VK_SUCCESS) {
        return false;
    }
    VkImageViewCreateInfo viewCI{};
    viewCI.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewCI.image = t.image;
    viewCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewCI.format = format;
    viewCI.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0,
                               .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
    if (vkCreateImageView(ctx_->getDevice(), &viewCI, nullptr, &t.view) != VK_SUCCESS) {
        destroyTarget(t);
        return false;
    }
    return true;
}

void RendererBloom::destroyTarget(Target& t) {
    if (!ctx_) return;
    if (t.view) {
        vkDestroyImageView(ctx_->getDevice(), t.view, nullptr);
        t.view = VK_NULL_HANDLE;
    }
    if (t.image) {
        vmaDestroyImage(ctx_->getAllocator(), t.image, t.alloc);
        t.image = VK_NULL_HANDLE;
        t.alloc = VK_NULL_HANDLE;
    }
}

void RendererBloom::destroyTargets() {
    for (uint32_t i = 0; i < MAX_FRAMES; ++i) {
        destroyTarget(halfCopy_[i]);
        destroyTarget(quarterCopy_[i]);
        destroyTarget(blurTemp_[i]);
        destroyTarget(bloom_[i]);
        strength_[i] = 0.0f;
        drawThisFrame_[i] = false;
    }
    halfExtent_ = {.width = 0, .height = 0};
    quarterExtent_ = {.width = 0, .height = 0};
    sourceExtent_ = {.width = 0, .height = 0};
}

bool RendererBloom::ensureTargets(VkExtent2D extent) {
    if (extent.width == sourceExtent_.width && extent.height == sourceExtent_.height &&
        bloom_[0].image != VK_NULL_HANDLE) {
        return true;
    }
    vkDeviceWaitIdle(ctx_->getDevice());
    destroyTargets();

    const BloomTargetExtents dims = computeBloomTargetExtents(extent.width, extent.height);
    halfExtent_ = {.width = dims.halfWidth, .height = dims.halfHeight};
    quarterExtent_ = {.width = dims.quarterWidth, .height = dims.quarterHeight};

    for (uint32_t i = 0; i < MAX_FRAMES; ++i) {
        if (!createTarget(halfCopy_[i], halfExtent_, kCopyFormat,
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ||
            !createTarget(quarterCopy_[i], quarterExtent_, kCopyFormat,
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT) ||
            !createTarget(blurTemp_[i], quarterExtent_, kBloomFormat,
                          VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT) ||
            !createTarget(bloom_[i], quarterExtent_, kBloomFormat,
                          VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) {
            LOG_WARNING("RendererBloom: failed to allocate ", quarterExtent_.width, "x",
                        quarterExtent_.height, " bloom targets - ffxGlow disabled");
            destroyTargets();
            usable_ = false;
            return false;
        }
    }

    ctx_->immediateSubmit([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier2 b[MAX_FRAMES * 2]{};
        for (uint32_t i = 0; i < MAX_FRAMES; ++i) {
            for (uint32_t k = 0; k < 2; ++k) {
                auto& bar = b[i * 2 + k];
                bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                bar.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
                bar.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                bar.dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
                bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                bar.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                bar.image = (k == 0) ? blurTemp_[i].image : bloom_[i].image;
                bar.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0,
                                        .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
            }
        }
        VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = MAX_FRAMES * 2;
        dep.pImageMemoryBarriers = b;
        cmdPipelineBarrier2(cmd, dep);
    });

    for (uint32_t i = 0; i < MAX_FRAMES; ++i) {
        VkDescriptorImageInfo quarterIn{.sampler = sampler_, .imageView = quarterCopy_[i].view,
                                        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo tempOut{.sampler = VK_NULL_HANDLE, .imageView = blurTemp_[i].view,
                                      .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo tempIn{.sampler = sampler_, .imageView = blurTemp_[i].view,
                                     .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo bloomOut{.sampler = VK_NULL_HANDLE, .imageView = bloom_[i].view,
                                       .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo bloomIn{.sampler = sampler_, .imageView = bloom_[i].view,
                                      .imageLayout = VK_IMAGE_LAYOUT_GENERAL};

        VkWriteDescriptorSet w[5]{};
        for (auto& write : w) {
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.descriptorCount = 1;
        }
        w[0].dstSet = blurHSets_[i];
        w[0].dstBinding = 0;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[0].pImageInfo = &quarterIn;

        w[1].dstSet = blurHSets_[i];
        w[1].dstBinding = 1;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[1].pImageInfo = &tempOut;

        w[2].dstSet = blurVSets_[i];
        w[2].dstBinding = 0;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[2].pImageInfo = &tempIn;

        w[3].dstSet = blurVSets_[i];
        w[3].dstBinding = 1;
        w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[3].pImageInfo = &bloomOut;

        w[4].dstSet = compositeSets_[i];
        w[4].dstBinding = 0;
        w[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[4].pImageInfo = &bloomIn;

        vkUpdateDescriptorSets(ctx_->getDevice(), 5, w, 0, nullptr);
    }

    sourceExtent_ = extent;
    LOG_INFO("RendererBloom: ", quarterExtent_.width, "x", quarterExtent_.height,
             " targets for ", extent.width, "x", extent.height, " swapchain");
    return true;
}

bool RendererBloom::record(VkCommandBuffer cmd, uint32_t frame, VkImage swapchainImage,
                           VkExtent2D extent, float strength) {
    ZoneScopedN("RendererBloom::record");
    if (frame >= MAX_FRAMES) return false;
    drawThisFrame_[frame] = false;
    strength_[frame] = 0.0f;
    if (!usable_ || strength <= 0.0f || cmd == VK_NULL_HANDLE || swapchainImage == VK_NULL_HANDLE ||
        extent.width < 4 || extent.height < 4) {
        return false;
    }
    if (!ensureTargets(extent) || !ensureCompositePipeline()) return false;

    const auto barrier = [&](VkImage image, VkImageLayout from, VkImageLayout to,
                             VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                             VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
        VkImageMemoryBarrier2 b{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = srcStage;
        b.srcAccessMask = srcAccess;
        b.dstStageMask = dstStage;
        b.dstAccessMask = dstAccess;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0,
                              .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
        VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        cmdPipelineBarrier2(cmd, dep);
    };

    // 1. Two-stage 2x2 hardware linear blit (exact 4x4 box filter, matching 2x ffxbox4.bls).
    barrier(swapchainImage, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    barrier(halfCopy_[frame].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

    VkImageBlit blit1{};
    blit1.srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1};
    blit1.dstSubresource = blit1.srcSubresource;
    blit1.srcOffsets[1] = {.x = static_cast<int32_t>(extent.width), .y = static_cast<int32_t>(extent.height), .z = 1};
    blit1.dstOffsets[1] = {.x = static_cast<int32_t>(halfExtent_.width), .y = static_cast<int32_t>(halfExtent_.height), .z = 1};
    vkCmdBlitImage(cmd, swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   halfCopy_[frame].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit1, VK_FILTER_LINEAR);

    // Return swapchainImage to PRESENT_SRC_KHR for SunShafts and the overlay pass.
    barrier(swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_2_TRANSFER_READ_BIT);

    barrier(halfCopy_[frame].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    barrier(quarterCopy_[frame].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

    VkImageBlit blit2 = blit1;
    blit2.srcOffsets[1] = {.x = static_cast<int32_t>(halfExtent_.width), .y = static_cast<int32_t>(halfExtent_.height), .z = 1};
    blit2.dstOffsets[1] = {.x = static_cast<int32_t>(quarterExtent_.width), .y = static_cast<int32_t>(quarterExtent_.height), .z = 1};
    vkCmdBlitImage(cmd, halfCopy_[frame].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   quarterCopy_[frame].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit2, VK_FILTER_LINEAR);

    barrier(quarterCopy_[frame].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    barrier(blurTemp_[frame].image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);

    // 2. Separable 9-tap Gaussian blur (H then V) at quarter resolution.
    const float sigmaTexels = computeQuarterBlurSigmaTexels(quarterExtent_.height);
    const float tapStepTexels = computeQuarterBlurTapStepTexels(sigmaTexels);
    const float invSigmaNorm = tapStepTexels / std::max(1e-4f, sigmaTexels);
    const uint32_t groupsX = (quarterExtent_.width + kBlurGroup - 1) / kBlurGroup;
    const uint32_t groupsY = (quarterExtent_.height + kBlurGroup - 1) / kBlurGroup;

    // Horizontal sigma matches vertical sigma in physical screen pixels so halos stay circular.
    const float aspectFix = (static_cast<float>(extent.height) / static_cast<float>(extent.width)) *
                            (static_cast<float>(quarterExtent_.width) / static_cast<float>(quarterExtent_.height));

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, blurPipeline_);

    BlurPush pushH{};
    pushH.axisStepUV = glm::vec2((tapStepTexels * aspectFix) / static_cast<float>(quarterExtent_.width), 0.0f);
    pushH.invSigmaNorm = invSigmaNorm;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, blurLayout_, 0, 1, &blurHSets_[frame], 0, nullptr);
    vkCmdPushConstants(cmd, blurLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pushH), &pushH);
    vkCmdDispatch(cmd, groupsX, groupsY, 1);

    barrier(blurTemp_[frame].image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    barrier(bloom_[frame].image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);

    BlurPush pushV{};
    pushV.axisStepUV = glm::vec2(0.0f, tapStepTexels / static_cast<float>(quarterExtent_.height));
    pushV.invSigmaNorm = invSigmaNorm;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, blurLayout_, 0, 1, &blurVSets_[frame], 0, nullptr);
    vkCmdPushConstants(cmd, blurLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pushV), &pushV);
    vkCmdDispatch(cmd, groupsX, groupsY, 1);

    barrier(bloom_[frame].image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);

    strength_[frame] = std::clamp(strength, 0.0f, 1.0f);
    drawThisFrame_[frame] = true;
    return true;
}

void RendererBloom::composite(VkCommandBuffer cmd, uint32_t frame) {
    if (frame >= MAX_FRAMES || !drawThisFrame_[frame] || compositePipeline_ == VK_NULL_HANDLE) return;
    CompositePush push{.glow = strength_[frame]};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compositePipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compositeLayout_, 0, 1,
                            &compositeSets_[frame], 0, nullptr);
    vkCmdPushConstants(cmd, compositeLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

void RendererBloom::shutdown() {
    if (!ctx_) return;
    VkDevice device = ctx_->getDevice();
    vkDeviceWaitIdle(device);
    destroyTargets();
    destroy(device, descPool_);
    for (uint32_t i = 0; i < MAX_FRAMES; ++i) {
        blurHSets_[i] = VK_NULL_HANDLE;
        blurVSets_[i] = VK_NULL_HANDLE;
        compositeSets_[i] = VK_NULL_HANDLE;
    }
    destroy(device, blurPipeline_);
    destroy(device, compositePipeline_);
    destroy(device, blurLayout_);
    destroy(device, compositeLayout_);
    destroy(device, blurSetLayout_);
    destroy(device, compositeSetLayout_);
    sampler_ = VK_NULL_HANDLE;
    compositeFormat_ = VK_FORMAT_UNDEFINED;
    usable_ = false;
    ctx_ = nullptr;
}

}  // namespace wowee::rendering
