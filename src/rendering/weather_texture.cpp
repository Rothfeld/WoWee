#include "rendering/weather_texture.hpp"

#include "core/logger.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/blp_loader.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_utils.hpp"

namespace wowee::rendering {

bool WeatherTexture::load(VkContext& ctx, pipeline::AssetManager& assets, const std::string& path) {
    VkDevice device = ctx.getDevice();
    pipeline::BLPImage image = assets.loadTexture(path);
    if (!image.isValid() || !texture_.upload(ctx, image.data.data(), image.width, image.height,
                                             VK_FORMAT_R8G8B8A8_UNORM, true)) {
        LOG_ERROR("WeatherTexture: failed to load ", path);
        return false;
    }
    // Trilinear, no anisotropic filtering: a drop is a sliver, and an
    // anisotropic footprint keeps its across-the-drop sampling at the finest
    // mip, so the bright centre texels fall between pixel centres and the
    // streak breaks up (_experiments/20261119T0930.weather).
    texture_.createSampler(device, VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 1.0f);
    if (!texture_.isValid()) {
        LOG_ERROR("WeatherTexture: ", path, " is not sampleable");
        return false;
    }

    VkDescriptorSetLayoutBinding sampler{};
    sampler.binding = 0;
    sampler.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sampler.descriptorCount = 1;
    sampler.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    layout_ = createDescriptorSetLayout(device, {sampler});
    if (!layout_) return false;
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = 1;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &size;
    if (vkCreateDescriptorPool(device, &pool, nullptr, &pool_) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = pool_;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &layout_;
    if (vkAllocateDescriptorSets(device, &alloc, &set_) != VK_SUCCESS) return false;
    VkDescriptorImageInfo imageInfo = texture_.descriptorInfo();
    VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set_;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    return true;
}

void WeatherTexture::destroy(VkContext& ctx) {
    VkDevice device = ctx.getDevice();
    texture_.destroy(device, ctx.getAllocator());
    rendering::destroy(device, pool_);
    rendering::destroy(device, layout_);
    set_ = VK_NULL_HANDLE;
}

} // namespace wowee::rendering
