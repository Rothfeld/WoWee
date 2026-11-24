#pragma once

#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>

namespace wowee::rendering {

// Build 8606's surface light is independent of the celestial sun. Its polar
// angle alternates between 127 and 110 degrees every six hours; azimuth is 225.
// The original uses a periodic cubic cosine approximation for these angles.
inline glm::vec3 legacySurfaceLightDirection(float hours) {
    const float quadrant = hours / 6.0f;
    const float whole = std::floor(quadrant);
    const float fraction = quadrant - whole;
    const float phase = (static_cast<int>(whole) & 1) ? 1.0f - fraction : fraction;
    const float polar = (127.0f - 17.0f * phase) / 180.0f;
    const auto cosine = [](float halfTurns) {
        const float whole = std::floor(halfTurns);
        const float t = halfTurns - whole;
        const float value = 1.0f - t * t * (6.0f - 4.0f * t);
        return (static_cast<int>(whole) & 1) ? -value : value;
    };
    const float horizontal = cosine(polar - 0.5f);
    return {horizontal * cosine(1.25f), horizontal * cosine(0.75f), cosine(polar)};
}

// Fixed world-space light direction (light travel vector) for WMO interiors in
// build 8606: azimuth 225 deg, z = -0.9 -> (-sqrt(0.095), -sqrt(0.095), -0.9).
inline constexpr glm::vec3 kLegacyIndoorLightDir{-0.3082207f, -0.3082207f, -0.9f};

struct DirectionalLightRig {
    glm::vec3 ambient{0.0f};
    glm::vec3 diffuse{0.0f};
    glm::vec3 dir{kLegacyIndoorLightDir};
};

inline glm::vec3 capChannelMax(const glm::vec3& c, float capVal) {
    const float m = std::max({c.r, c.g, c.b});
    return (m > capVal) ? c * (capVal / m) : c;
}

inline glm::vec3 floorChannelMax(const glm::vec3& c, float floorVal) {
    const float m = std::max({c.r, c.g, c.b});
    return (m > 0.0f && m < floorVal) ? c * (floorVal / m) : c;
}

// Build 8606 base indoor model rig from an RGB colour in [0, 1]:
// ambient caps max channel at 96/255; diffuse floors max channel at 168/255
// for units (MOCV) and at 112/255 for WMO doodads (MODD) and game objects.
inline DirectionalLightRig indoorUnitRig(const glm::vec3& mocv) {
    return {
        .ambient = capChannelMax(mocv, 96.0f / 255.0f),
        .diffuse = floorChannelMax(mocv, 168.0f / 255.0f),
        .dir = kLegacyIndoorLightDir,
    };
}

inline DirectionalLightRig indoorDoodadRig(const glm::vec3& color) {
    return {
        .ambient = capChannelMax(color, 96.0f / 255.0f),
        .diffuse = floorChannelMax(color, 112.0f / 255.0f),
        .dir = kLegacyIndoorLightDir,
    };
}

// Merge two directional+ambient light rigs into one single-directional rig
// (Rec.709 luminance-weighted dipole direction, 1/4 cancelled dipole transferred
// into ambient monopole). Matches build 8606's MOLT/MOCV light accumulator.
inline DirectionalLightRig mergeDirectionalRig(const DirectionalLightRig& l1,
                                               const DirectionalLightRig& l2) {
    constexpr glm::vec3 kLuma(0.2125f, 0.7154f, 0.0721f);
    const float w1 = glm::dot(l1.diffuse, kLuma);
    const float w2 = glm::dot(l2.diffuse, kLuma);
    const glm::vec3 weightedDir = w1 * l1.dir + w2 * l2.dir;
    const float lenSq = glm::dot(weightedDir, weightedDir);
    const glm::vec3 dir = lenSq > 1e-10f ? weightedDir * (1.0f / std::sqrt(lenSq)) : l1.dir;
    const glm::vec3 sumDiff = l1.diffuse + l2.diffuse;
    const glm::vec3 projDiff = l1.diffuse * std::max(glm::dot(l1.dir, dir), 0.0f) +
                               l2.diffuse * std::max(glm::dot(l2.dir, dir), 0.0f);
    const glm::vec3 dAmb = 0.25f * (sumDiff - projDiff);
    return {
        .ambient = l1.ambient + l2.ambient + dAmb,
        .diffuse = glm::max(projDiff - dAmb, glm::vec3(0.0f)),
        .dir = dir,
    };
}

inline DirectionalLightRig blendIndoorWithOutdoorRig(const DirectionalLightRig& indoor,
                                                     const glm::vec3& outdoorAmbient,
                                                     const glm::vec3& outdoorDiffuseBase,
                                                     const glm::vec3& outdoorDir,
                                                     float exteriorBlend,
                                                     bool isUnit,
                                                     float unitAmbGain = 1.0f) {
    const float a = std::clamp(exteriorBlend, 0.0f, 1.0f);
    const float indoorGain = isUnit ? std::max(unitAmbGain, 0.0f) : 1.0f;
    const DirectionalLightRig gainedIndoor{
        .ambient = indoor.ambient * indoorGain,
        .diffuse = indoor.diffuse * indoorGain,
        .dir = indoor.dir,
    };
    if (a <= 0.0f) return gainedIndoor;
    if (a >= 1.0f) {
        return {
            .ambient = outdoorAmbient,
            .diffuse = outdoorDiffuseBase * (isUnit ? 2.5f : 1.0f),
            .dir = outdoorDir,
        };
    }
    const DirectionalLightRig scaledIn{
        .ambient = gainedIndoor.ambient * (1.0f - a),
        .diffuse = gainedIndoor.diffuse * (1.0f - a),
        .dir = gainedIndoor.dir,
    };
    const DirectionalLightRig scaledOut{
        .ambient = outdoorAmbient * a,
        .diffuse = outdoorDiffuseBase * a,
        .dir = outdoorDir,
    };
    DirectionalLightRig merged = mergeDirectionalRig(scaledIn, scaledOut);
    if (isUnit) merged.diffuse *= (1.0f + 1.5f * a);
    return merged;
}

} // namespace wowee::rendering
