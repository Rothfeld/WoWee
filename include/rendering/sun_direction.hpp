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
/// Rises in the east (-Y) at 06:00, reaches 60 deg elevation toward north (+X)
/// at 12:00 noon, tracks Native TBC 2.4.3's extended afternoon/dusk arc from
/// 14:00 (~49 deg) through 17:31 (~26 deg) to sunset at 21:15 (+Y, matching
/// LightIntBand's 21:00 sunset keyframe before 22:00 moonrise), and passes
/// below the horizon overnight.
inline glm::vec3 sunLightDirectionForDayFraction(float dayFraction) {
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kTwoPi = 6.28318530717958647692f;
    constexpr float kSinTilt = 0.5f;        // sin(30 deg)
    constexpr float kCosTilt = 0.8660254f;  // cos(30 deg)
    float frac = std::fmod(dayFraction, 1.0f);
    if (frac < 0.0f) frac += 1.0f;
    const float hours = frac * 24.0f;
    float angle = frac * kTwoPi;
    if (hours > 14.0f && hours <= 21.25f) {
        // 14:00 (7*pi/6 = 210 deg) -> 21:15 (3*pi/2 = 270 deg)
        const float t = (hours - 14.0f) / (21.25f - 14.0f);
        angle = (7.0f * kPi / 6.0f) + t * (kPi / 3.0f);
    } else if (hours > 21.25f) {
        // 21:15 (3*pi/2 = 270 deg) -> 24:00 (2*pi = 360 deg)
        const float t = (hours - 21.25f) / (24.0f - 21.25f);
        angle = (1.5f * kPi) + t * (0.5f * kPi);
    }
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    return glm::vec3(c * kSinTilt, s, c * kCosTilt);
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
