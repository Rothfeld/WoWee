#pragma once

/// Where the sun is, from the direction its light travels.
///
/// The lighting system gives a directional vector - the way the light goes -
/// so the sun is the other way. Below the horizon it stays below: this used to
/// be written inline in SkySystem::getSunPosition, which mirrored a sun under
/// the ground back up into the sky (`sunDir = dir` rather than `-dir`).
///
/// Only the lens flare asked where the sun was then, so all the mirror achieved
/// was to invent one for the flare to draw around, at a position no sun was at.
/// The flare's own height attenuation then read the mirrored height as a sun
/// climbing the sky and let it through unweakened. The sun shafts ask too now,
/// and would have streamed from the same invented sun.

#include <cmath>
#include <glm/glm.hpp>

namespace wowee::rendering {

/// Light travel direction (pointing FROM the sun) for a given day fraction in
/// [0, 1) in engine render space (+X = North, +Y = West, -Y = East, +Z = Up).
///
/// Calibrated to Native TBC 2.4.3 multi-epoch outdoor captures:
/// - 06:00 sunrise on the SE horizon (`az = -135 deg`),
/// - 12:00 culmination (`88 deg` elevation tilted slightly SW),
/// - 13:57..14:05 early afternoon (`~66..68 deg` elevation in NW, matching
///   Goldshire & Kharanos paired captures),
/// - 17:31 late afternoon (`~27.9 deg` elevation at `az ~ +46 deg` NW, peeking
///   above Karazhan's north-west battlements beside the `292z` main spire),
/// - 18:17 early evening (`~19.1 deg` elevation at `az ~ +45.7 deg` NW, behind
///   Karazhan's main spire and outside the due-west `+94.5 deg` rotunda dome view),
/// - 20:00 sunset on the NW horizon (`az = +45 deg`, matching `starFieldTimeVisibility`
///   nightfall so `20:12` Shimmering Flats skies have the sun below the horizon).
inline glm::vec3 sunLightDirectionForDayFraction(float dayFraction) {
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kHalfPi = 1.57079632679489661923f;
    constexpr float kInvSqrt2 = 0.70710678118654752440f;
    constexpr float kMaxElevSin = 0.99939083f;  // sin(88 deg)
    constexpr float kMaxElevCos = 0.03489950f;  // cos(88 deg)

    float frac = std::fmod(dayFraction, 1.0f);
    if (frac < 0.0f) frac += 1.0f;
    const float hours = frac * 24.0f;

    // theta = 0 at 06:00 sunrise, pi/2 at 12:00 culmination, pi at 20:00 sunset,
    // and (pi..2*pi) below the horizon overnight across 20:00..06:00 (10 hours).
    float theta = 0.0f;
    if (hours >= 6.0f && hours <= 12.0f) {
        theta = ((hours - 6.0f) / 6.0f) * kHalfPi;
    } else if (hours > 12.0f && hours <= 20.0f) {
        theta = kHalfPi + ((hours - 12.0f) / 8.0f) * kHalfPi;
    } else if (hours > 20.0f) {
        theta = kPi + ((hours - 20.0f) / 10.0f) * kPi;
    } else {
        theta = kPi + ((hours + 4.0f) / 10.0f) * kPi;
    }

    const float horizForward = -std::cos(theta);            // -1 (SE) -> +1 (NW)
    const float horizSide = std::sin(theta) * kMaxElevCos;  // slight SW noon tilt
    const float z = std::sin(theta) * kMaxElevSin;

    const glm::vec3 toSun(
        (horizForward - horizSide) * kInvSqrt2,
        (horizForward + horizSide) * kInvSqrt2,
        z);
    return -glm::normalize(toSun);
}

/// Daytime sun disc time-of-day intensity in [0, 1] across the 06:00 sunrise ..
/// 20:00 sunset window: smooth dawn fade-in (05:30..06:15), full strength
/// through afternoon/early evening (06:15..19:15, including 17:31 and 18:17),
/// smooth dusk fade-out to 20:03 sunset, and 0.0 overnight.
inline float sunDiscTimeIntensity(float timeOfDay) {
    float h = std::fmod(timeOfDay, 24.0f);
    if (h < 0.0f) h += 24.0f;
    if (h >= 5.5f && h < 6.25f) {
        return (h - 5.5f) / 0.75f;
    }
    if (h >= 6.25f && h < 19.25f) {
        return 1.0f;
    }
    if (h >= 19.25f && h < 20.05f) {
        return 1.0f - (h - 19.25f) / 0.80f;
    }
    return 0.0f;
}

/// World-space vertex position for a celestial billboard quad tangent to the
/// celestial sphere at `center`. Keeping the quad perpendicular to `center`
/// prevents elevation foreshortening into a horizontal oval while keeping +Y
/// aligned toward zenith.
inline glm::vec3 celestialBillboardWorldPos(const glm::vec3& center,
                                            const glm::vec2& size,
                                            const glm::vec2& localOffset) {
    const float lenSq = glm::dot(center, center);
    if (lenSq < 1e-8f) return center;
    const glm::vec3 toBody = center * glm::inversesqrt(lenSq);
    const glm::vec3 refUp = std::abs(toBody.z) < 0.99f
        ? glm::vec3(0.0f, 0.0f, 1.0f)
        : glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 right = glm::normalize(glm::cross(toBody, refUp));
    const glm::vec3 up = glm::cross(right, toBody);
    return center + right * (localOffset.x * size.x) + up * (localOffset.y * size.y);
}

/// The unit direction from the eye toward the sun.
///
/// A zero or degenerate directional means no sun has been given yet; straight
/// down is what the rest of the sky code falls back to, so the sun reads as
/// being straight up and everything that gates on height turns it off.
inline glm::vec3 sunDirectionFromLightDir(const glm::vec3& directionalDir) {
    const float lenSq = glm::dot(directionalDir, directionalDir);
    if (lenSq < 1e-8f) return glm::vec3(0.0f, 0.0f, 1.0f);
    return -directionalDir * glm::inversesqrt(lenSq);
}

/// Where the sun falls on the screen.
struct SunOnScreen {
    /// False when the sun is behind the eye, where it has no place on screen.
    bool inFront = false;
    /// Framebuffer uv, (0,0) at the top left - the way every full-screen pass
    /// samples. Not clamped: a sun just past the edge still streams rays in.
    glm::vec2 uv{0.0f};
};

/// The sun's place on screen, for the view and projection the frame was drawn
/// with. A direction, so it is projected with w = 0: the sun is infinitely far,
/// and moving the camera does not move it.
inline SunOnScreen sunScreenPosition(const glm::mat4& view, const glm::mat4& projection,
                                     const glm::vec3& sunDir) {
    const glm::vec4 clip = projection * view * glm::vec4(sunDir, 0.0f);
    if (clip.w <= 1e-4f) return {};
    return {.inFront = true, .uv = glm::vec2(clip) / clip.w * 0.5f + 0.5f};
}

}  // namespace wowee::rendering
