#pragma once

#include <cstdint>
#include <vector>
#include <glm/glm.hpp>

namespace wowee {

namespace audio { enum class FootstepSurface : uint8_t; }

namespace rendering {

class Renderer;

// ============================================================================
// FootstepDriver - extracted from AnimationController
//
// Owns animation-driven footstep event detection, surface resolution,
// and player/mount footstep tracking state.
// ============================================================================
class FootstepDriver {
public:
    FootstepDriver() = default;

    /// Process footstep events for this frame (called from Renderer::update).
    void update(float deltaTime, Renderer* renderer,
                bool mounted, uint32_t mountInstanceId, bool taxiFlight,
                bool isFootstepState);

    /// Detect if a footstep event should trigger based on animation phase crossing.
    /// When the model has authored $FSD footfall keyframes, pass them as
    /// eventTimesMs (ms from sequence start) for exact foot-strike timing;
    /// nullptr falls back to the generic biped step pattern.
    bool shouldTriggerFootstepEvent(uint32_t animationId, float animationTimeMs,
                                    float animationDurationMs,
                                    const std::vector<uint32_t>* eventTimesMs = nullptr);

    /// Resolve the surface type under the character for footstep sound selection.
    audio::FootstepSurface resolveFootstepSurface(Renderer* renderer) const;

private:
    /// One footstep of the character or its mount: FootstepTerrainLookup
    /// sound for that unit on the TerrainType under it.
    void playStep(Renderer* renderer, uint32_t instanceId);
    [[nodiscard]] bool isWading(Renderer* renderer) const;
    /// Water-step side effects shared by on-foot and mounted steps
    /// (splash sound layer + visual foot splash).
    void playWaterStepExtras(Renderer* renderer) const;


    // Footstep surface cache (avoid expensive queries every step)
    mutable audio::FootstepSurface cachedFootstepSurface_{};
    mutable glm::vec3 cachedFootstepPosition_{0.0f, 0.0f, 0.0f};
    mutable float cachedFootstepUpdateTimer_{999.0f};

    // The mount's footfall event tracking, the same scan as the player's.
    struct EventTracker {
        uint32_t lastAnimationId = 0;
        float lastNormTime = 0.0f;
        bool initialized = false;
        void reset() { initialized = false; }
        bool shouldTriggerFootstepEvent(uint32_t animationId, float animationTimeMs,
                                        float animationDurationMs, const std::vector<uint32_t>* eventTimesMs);
    };
    EventTracker playerFootsteps_;
    EventTracker mountFootsteps_;
};

} // namespace rendering
} // namespace wowee
