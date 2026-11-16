#pragma once

#include "pipeline/wmo_loader.hpp"
#include <array>
#include <optional>
#include <span>

namespace wowee::rendering {
struct WMOFogSample {
    glm::vec3 color;
    float start, end;
};

inline std::optional<WMOFogSample> sampleWMOFog(
    std::span<const pipeline::WMOFog> fogs, const std::array<uint8_t, 4>& indices,
    const glm::vec3& position, bool underwater) {
    if (indices[0] >= fogs.size()) return std::nullopt;
    const auto sample = [underwater](const pipeline::WMOFog& fog) {
        const float end = underwater ? fog.endDist2 : fog.endDist;
        return WMOFogSample{glm::vec3(underwater ? fog.color2 : fog.color1),
            end * (underwater ? fog.startFactor2 : fog.startFactor), end};
    };
    const auto& base = fogs[0];
    if (base.largeRadius <= 0 && !(base.flags & 1)) return std::nullopt;
    auto result = sample(base);
    // Record zero provides the WMO default. Group refs select nearby local
    // spheres, including refs whose radius does not reach the camera.
    // Repeated indices are padding, not additional weight.
    float total = 0;
    WMOFogSample local{glm::vec3(0), 0, 0};
    for (size_t i = 0; i < indices.size(); ++i) {
        if (indices[i] == 0 || indices[i] >= fogs.size()) continue;
        bool duplicate = false;
        for (size_t j = 0; j < i; ++j) duplicate |= indices[j] == indices[i];
        if (duplicate) continue;
        const auto& fog = fogs[indices[i]];
        const float distance = glm::distance(position, fog.position);
        float weight = 0;
        if (fog.flags & 1) weight = 1;
        else if (distance <= fog.smallRadius) weight = 1;
        else if (fog.largeRadius > fog.smallRadius)
            weight = glm::clamp((fog.largeRadius - distance) /
                                (fog.largeRadius - fog.smallRadius), 0.0f, 1.0f);
        const auto value = sample(fog);
        local.color += value.color * weight;
        local.start += value.start * weight;
        local.end += value.end * weight;
        total += weight;
    }
    if (total > 0) {
        const float blend = std::min(total, 1.0f);
        result.color = glm::mix(result.color, local.color / total, blend);
        result.start = glm::mix(result.start, local.start / total, blend);
        result.end = glm::mix(result.end, local.end / total, blend);
    }
    return result;
}
}
