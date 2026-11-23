#pragma once

#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>

namespace wowee::rendering {

/// Rec.709 luminance of a linear/sRGB sky color in [0, 1].
inline float skyColorLuminance(const glm::vec3& c) {
    return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
}

/// Smooth Hermite interpolation matching GLSL smoothstep(edge0, edge1, x).
inline float skySmoothstep(float edge0, float edge1, float x) {
    if (edge0 == edge1) return x < edge0 ? 0.0f : 1.0f;
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/// Time-of-day star visibility in [0, 1].
/// Daytime (06:00-18:00): 0.0 (stars never bleed into noon sky).
/// Dusk (18:00-20:00): smooth fade in.
/// Night (20:00-04:00): 1.0.
/// Dawn (04:00-06:00): smooth fade out.
inline float starFieldTimeVisibility(float timeOfDayHours) {
    float h = std::fmod(timeOfDayHours, 24.0f);
    if (h < 0.0f) h += 24.0f;
    if (h >= 20.0f || h < 4.0f) return 1.0f;
    if (h >= 18.0f) return skySmoothstep(18.0f, 20.0f, h);
    if (h < 6.0f) return 1.0f - skySmoothstep(4.0f, 6.0f, h);
    return 0.0f;
}

/// Overcast cloud-deck cover in [0, 1] from effective DBC cloud density.
/// Clear-night Azeroth zones (Elwynn, Orgrimmar, Westfall, Barrens) author
/// LightFloatBand[3] <= 0.50 -> 0.0 (stars and moon stay unattenuated).
/// Permanent-overcast zones (Karazhan 386 / Deadwind Pass 19 / Duskwood 57 at
/// 0.70, Plaguelands at 0.68, Storm at 0.95) ramp to 1.0 so thick overcast
/// cloud cover hides stars and moon just like storm weather.
inline float overcastCloudCover(float effectiveCloudDensity) {
    return skySmoothstep(0.55f, 0.68f, effectiveCloudDensity);
}

/// Atmospheric attenuation for stars in [0, 1].
/// Attenuates under heavy weather/overcast, low DBC celestialGlowThrough
/// (LightFloatBand[2]), and bright zenith skies, without crushing clear-night
/// Azeroth skies where LightFloatBand[3] (cloud weight) is 0.5 and
/// LightFloatBand[4] is 0.95.
inline float starFieldAtmosphereVisibility(float weatherIntensity,
                                           float celestialGlowThrough,
                                           float skyTopLuma) {
    const float weatherClear = 1.0f - skySmoothstep(0.15f, 0.70f, weatherIntensity);
    const float glowThrough = std::clamp(celestialGlowThrough, 0.0f, 1.0f);
    // Tanaris midnight zenith luma is ~0.105; Barrens/1kNeedles/Westfall is 0.0.
    // Keep full visibility up to 0.14 and fade out cleanly by 0.30.
    const float darkSky = 1.0f - skySmoothstep(0.14f, 0.30f, skyTopLuma);
    return weatherClear * glowThrough * darkSky;
}

/// Combined starfield intensity for a frame.
inline float starFieldEffectiveIntensity(float timeOfDayHours,
                                         float weatherIntensity,
                                         float celestialGlowThrough,
                                         const glm::vec3& skyTopColor) {
    return starFieldTimeVisibility(timeOfDayHours) *
           starFieldAtmosphereVisibility(weatherIntensity, celestialGlowThrough,
                                         skyColorLuminance(skyTopColor));
}

/// Whether procedural DBC-tinted clouds should render on the sky dome.
/// Outland and custom skybox M2s supply their own clouds/nebulae, so clouds
/// are disabled when an original skybox M2 is active. Zones with zero DBC
/// cloud density and clear weather (e.g. Tanaris, FloatBand[3] = 0.0) also
/// stay cloud-free.
inline bool shouldRenderSkyClouds(bool useOriginalSkybox,
                                  float cloudDensity,
                                  float weatherIntensity) {
    if (useOriginalSkybox) return false;
    const float effective = cloudDensity + (weatherIntensity > 0.05f ? weatherIntensity * 0.4f : 0.0f);
    return effective > 0.01f;
}

/// Unit direction vector toward the primary moon (White Lady) in WoW Z-up world
/// space (+X = North, +Y = West, +Z = Up).
///
/// Calibrated to Native TBC 2.4.3 night captures (Tanaris 22:15 rising right on
/// the NNW horizon at azimuth ~0.665 rad, climbing across the night sky through
/// midnight/early morning and setting around 06:00). Below the horizon during
/// daytime.
inline glm::vec3 moonDirectionForTimeOfDay(float timeOfDayHours) {
    constexpr float kPi = 3.14159265358979323846f;
    float h = std::fmod(timeOfDayHours, 24.0f);
    if (h < 0.0f) h += 24.0f;

    // Map the 24-hour cycle so moonrise at 22:00 -> arcAngle = 0, culmination
    // at 02:00 -> pi/2, moonset at 06:00 -> pi, and the remaining 16 daytime
    // hours (06:00..22:00) sweep continuously below the horizon (pi..2*pi).
    // At 22:15 (0.25h after rise), elevation is ~2.0 deg at azimuth ~0.665 rad,
    // matching Native TBC 2.4.3 Tanaris moonrise behind the desert ridge.
    const float hoursSinceRise = std::fmod(h - 22.0f + 24.0f, 24.0f);
    const float arcAngle = (hoursSinceRise <= 8.0f)
        ? (hoursSinceRise / 8.0f) * kPi
        : kPi + ((hoursSinceRise - 8.0f) / 16.0f) * kPi;
    const float sinArc = std::sin(arcAngle);
    const float cosArc = std::cos(arcAngle);

    // Rise azimuth ~0.665 rad (NNW, matching Tanaris capture), sweeping across
    // the sky to set azimuth ~0.665 + pi (SSE) with 45 deg peak elevation.
    constexpr float kRiseAz = 0.66522f;
    constexpr float kMaxElevSin = 0.70710678f;  // sin(45 deg)
    constexpr float kMaxElevCos = 0.70710678f;  // cos(45 deg)

    const float z = sinArc * kMaxElevSin;
    const float horizForward = cosArc;
    const float horizSide = sinArc * kMaxElevCos;

    const float cAz = std::cos(kRiseAz);
    const float sAz = std::sin(kRiseAz);
    // Local basis: forward = (cAz, sAz), side = (-sAz, cAz)
    const float x = horizForward * cAz - horizSide * sAz;
    const float y = horizForward * sAz + horizSide * cAz;
    return glm::normalize(glm::vec3(x, y, z));
}

/// Billboard quad scale at `distance` for the White Lady moon so its disc
/// (UV radius = 0.35, 70% of quad) subtends ~10.4 deg angular diameter,
/// matching Native TBC 2.4.3's 162px moon disc at 1280x720 (44.1 deg vFOV).
inline float moonBillboardScaleAtDistance(float distance) {
    // 2 * distance * tan(5.2 deg) / 0.70 ≈ distance * 0.260
    return distance * 0.260f;
}

/// Moon visibility in [0, 1] accounting for time-of-day, horizon fade, sky
/// darkness, weather overcast, and DBC celestialGlowThrough.
inline float moonNightVisibility(float timeOfDayHours,
                                 const glm::vec3& skyTopColor,
                                 float weatherIntensity,
                                 float celestialGlowThrough) {
    const glm::vec3 dir = moonDirectionForTimeOfDay(timeOfDayHours);
    if (dir.z < -0.06f) return 0.0f;

    const float horizonFade = skySmoothstep(-0.06f, 0.03f, dir.z);
    const float timeVis = starFieldTimeVisibility(timeOfDayHours);
    const float atmoVis = starFieldAtmosphereVisibility(
        weatherIntensity, celestialGlowThrough, skyColorLuminance(skyTopColor));
    return horizonFade * timeVis * atmoVis;
}

}  // namespace wowee::rendering
