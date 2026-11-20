#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <glm/glm.hpp>

namespace wowee {
namespace pipeline { struct M2Model; }

namespace ui {

/// Evaluated 2D sprite quad produced by AutoCastShineModel for one frame.
struct AutoCastShineSprite {
    float x = 0.0f;         // Center X in screen pixels
    float y = 0.0f;         // Center Y in screen pixels
    float halfSize = 0.0f;  // Half-width/half-height in screen pixels
    float rotation = 0.0f;  // Radians
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;
};

/// Data-driven evaluator for TBC 2.4.3 Interface\Buttons\UI-AutoCastButton.m2.
///
/// The shipped M2 has zero mesh vertices: it consists of 4 animated bones
/// tracing a square perimeter in a 2000 ms loop, each carrying a particle
/// emitter that spawns additive Interface\Buttons\GlowStar.blp sprites along
/// the perimeter. Keeping this separate from Vulkan/ImGui allows deterministic
/// unit tests against the real shipped M2 file.
struct AutoCastShineModel {
    struct BoneTrack {
        glm::vec3 pivot{0.0f};
        std::vector<uint32_t> timestampsMs;
        std::vector<glm::vec3> translations;
    };

    struct Emitter {
        uint16_t boneIndex = 0;
        uint8_t blendingType = 4;  // 4 = additive
        float lifespan = 1.0f;
        float emissionRate = 300.0f;
        float midpoint = 0.5f;
        float spin = 0.0f;
        std::vector<float> colorTimes;
        std::vector<glm::vec3> colorValues;
        std::vector<float> alphaTimes;
        std::vector<float> alphaValues;
        std::vector<float> scaleTimes;
        std::vector<float> scaleValues;
    };

    uint32_t durationMs = 0;
    std::string texturePath;
    std::vector<BoneTrack> bones;
    std::vector<Emitter> emitters;
    glm::vec2 boundsMin{0.0f};
    glm::vec2 boundsMax{0.0f};

    [[nodiscard]] bool isValid() const {
        return durationMs > 0 && !texturePath.empty() && !bones.empty() && !emitters.empty() &&
               (boundsMax.x - boundsMin.x) > 1e-6f && (boundsMax.y - boundsMin.y) > 1e-6f;
    }

    /// Build from a parsed M2Model (e.g. Interface\Buttons\UI-AutoCastButton.m2).
    static AutoCastShineModel fromM2(const pipeline::M2Model& model);

    /// Evaluate bone position (pivot + interpolated translation) at timeMs (wrapped mod durationMs).
    [[nodiscard]] glm::vec3 evaluateBonePosition(size_t boneIndex, float timeMs) const;

    /// Map bone (X, Y) position at timeMs to normalized UI rect coordinates [0, 1] x [0, 1]
    /// (u = left->right, v = top->bottom, clockwise perimeter).
    [[nodiscard]] glm::vec2 evaluateNormalizedPoint(size_t boneIndex, float timeMs) const;

    /// Evaluate all active GlowStar particle sprites for a button rect at timeSeconds.
    void evaluateSprites(double timeSeconds, float x0, float y0, float x1, float y1,
                         float modelScale, float widgetAlpha,
                         std::vector<AutoCastShineSprite>& out) const;
};

} // namespace ui
} // namespace wowee
