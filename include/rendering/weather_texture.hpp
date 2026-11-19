#pragma once

#include <string>

#include <vulkan/vulkan.h>

#include "rendering/vk_texture.hpp"

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering {

class VkContext;

/// One weather texture (Textures\Weather\*.blp) and the descriptor set that
/// binds it at set 1, binding 0 for the fragment stage. Rain drops, splashes,
/// snow flakes and mist puffs each draw with one.
class WeatherTexture {
public:
    WeatherTexture() = default;
    ~WeatherTexture() = default;
    WeatherTexture(const WeatherTexture&) = delete;
    WeatherTexture& operator=(const WeatherTexture&) = delete;

    /// Clamped at the edges (sprites and atlases), mip-mapped.
    [[nodiscard]] bool load(VkContext& ctx, pipeline::AssetManager& assets, const std::string& path);
    void destroy(VkContext& ctx);

    [[nodiscard]] VkDescriptorSetLayout layout() const { return layout_; }
    [[nodiscard]] VkDescriptorSet set() const { return set_; }

private:
    VkTexture texture_;
    VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
};

} // namespace rendering
} // namespace wowee
