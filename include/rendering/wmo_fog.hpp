#pragma once

#include "pipeline/wmo_loader.hpp"
#include <algorithm>
#include <array>
#include <optional>
#include <span>

namespace wowee::rendering {
struct WMOFogSample {
    glm::vec3 color;
    float start, end;
    float weight = 1.0f;
};

/// WMO compilers emit pure blue (0, 0, 255) (1364/1565 TBC MFOG records) or a
/// degenerate ~1-inch end distance (undercity.wmo MFOG[1]: 0.027778 yd) when
/// no underwater fog was authored for a record. Treat those stubs as unauthored
/// so underwater interiors fall back to Light.dbc's LightParamsIDUnderwater
/// instead of turning the screen solid blue.
inline bool wmoFogHasAuthoredUnderwater(const pipeline::WMOFog& fog) {
    if (fog.endDist2 <= 0.05f) return false;
    const bool pureBlueStub = fog.color2.r <= 0.002f &&
                              fog.color2.g <= 0.002f &&
                              fog.color2.b >= 0.995f;
    return !pureBlueStub;
}

inline std::optional<WMOFogSample> sampleWMOFog(
    std::span<const pipeline::WMOFog> fogs, const std::array<uint8_t, 4>& indices,
    const glm::vec3& position, bool underwater,
    std::optional<WMOFogSample> outdoorFallback = std::nullopt) {
    if (fogs.empty() || indices[0] >= fogs.size()) return std::nullopt;
    const auto sample = [underwater](const pipeline::WMOFog& fog) {
        const float end = underwater ? fog.endDist2 : fog.endDist;
        return WMOFogSample{glm::vec3(underwater ? fog.color2 : fog.color1),
            end * (underwater ? fog.startFactor2 : fog.startFactor), end, 1.0f};
    };
    const auto& base = fogs[0];
    const bool hasBase = (base.largeRadius > 0.0f || (base.flags & 1) != 0) &&
                         (!underwater || wmoFogHasAuthoredUnderwater(base));
    // Record zero provides the WMO default when authored (largeRadius > 0 or
    // infinite-radius flag 0x1). When no global WMO fog was authored, the WMO
    // compiler writes a zero-radius white stub in record zero (37 TBC WMOs -
    // wailingcaverns.wmo, md_mushroomcave*, md_icecave*, mini-hive*,
    // stormwind.wmo, darnassis.wmo, kl_diremaul.wmo - still carry authored
    // local spheres in records 1..N). Group refs select nearby local spheres;
    // without an authored record zero, spheres blend against outdoor Light.dbc
    // fog across their falloff shell and yield nullopt outside their radii.
    // Repeated indices are padding, not additional weight.
    float total = 0.0f;
    WMOFogSample local{glm::vec3(0.0f), 0.0f, 0.0f, 0.0f};
    for (size_t i = 0; i < indices.size(); ++i) {
        if (indices[i] == 0 || indices[i] >= fogs.size()) continue;
        bool duplicate = false;
        for (size_t j = 0; j < i; ++j) duplicate |= indices[j] == indices[i];
        if (duplicate) continue;
        const auto& fog = fogs[indices[i]];
        if (underwater && !wmoFogHasAuthoredUnderwater(fog)) continue;
        const float distance = glm::distance(position, fog.position);
        float weight = 0.0f;
        if (fog.flags & 1) weight = 1.0f;
        else if (distance <= fog.smallRadius) weight = 1.0f;
        else if (fog.largeRadius > fog.smallRadius)
            weight = glm::clamp((fog.largeRadius - distance) /
                                (fog.largeRadius - fog.smallRadius), 0.0f, 1.0f);
        const auto value = sample(fog);
        local.color += value.color * weight;
        local.start += value.start * weight;
        local.end += value.end * weight;
        total += weight;
    }
    if (!hasBase && total <= 0.0f) return std::nullopt;
    const float blend = std::min(total, 1.0f);
    if (hasBase) {
        auto result = sample(base);
        if (total > 0.0f) {
            result.color = glm::mix(result.color, local.color / total, blend);
            result.start = glm::mix(result.start, local.start / total, blend);
            result.end = glm::mix(result.end, local.end / total, blend);
        }
        return result;
    }
    WMOFogSample sphere{local.color / total, local.start / total, local.end / total, blend};
    if (outdoorFallback && blend < 1.0f) {
        return WMOFogSample{
            glm::mix(outdoorFallback->color, sphere.color, blend),
            glm::mix(outdoorFallback->start, sphere.start, blend),
            glm::mix(outdoorFallback->end, sphere.end, blend),
            1.0f,
        };
    }
    return sphere;
}
}
