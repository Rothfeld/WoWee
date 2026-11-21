#pragma once

#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include <glm/glm.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace wowee {

namespace pipeline {
class AssetManager;
class DBCFile;
} // namespace pipeline

namespace rendering {

/// Authored CameraShakes.dbc record (TBC 2.4.3 build 8606, 8 columns).
///
/// Verified empirically against live Native TBC 2.4.3 CGCamera telemetry:
/// - amplitude is authored in inches (1/36 yard): ID=1 (2.0 -> 0.05556 yd),
///   ID=10 (3.0 -> 0.08333 yd), ID=82 (12.0 -> 0.33333 yd), ID=103..105 (4.0 -> 0.11111 yd).
/// - phase is the starting time offset tau_0 (seconds): tau = elapsed + phase,
///   running while tau < duration (so active lifetime is duration - phase).
/// - shakeType 0 (SINE) holds unit envelope over [phase, duration);
///   shakeType 1 (DAMPED_SINE) decays as exp(-coefficient * tau).
/// - axis: 0 = camera horizontal forward, 1 = camera horizontal left, 2 = world +Z
///   (axis 2 stays pure world +Z even when the camera is pitched).
struct CameraShakeEntry {
    uint32_t id = 0;
    uint32_t shakeType = 0;
    uint32_t axis = 2;
    float amplitude = 0.0f;
    float frequency = 0.0f;
    float duration = 0.0f;
    float phase = 0.0f;
    float coefficient = 0.0f;
};

inline constexpr float kCameraShakeAmplitudePerUnit = 1.0f / 36.0f;
inline constexpr float kCameraShakeFullRadius = 9.0f;
inline constexpr float kCameraShakeDecayLength = 25.4f;
inline constexpr float kCameraShakeMaxDistance = 100.0f;

/// Distance attenuation from 3D camera-to-source distance in yards.
/// Fitted to Native TBC 2.4.3 CGCamera telemetry across d = 1.86 .. 46.98 yd
/// (max residual 0.0025 across FelReaver ID=82, Kodo Death ID=10, and Kodo Run ID=1),
/// with full strength inside 9 yd and zero beyond 100 yd.
[[nodiscard]] inline float cameraShakeDistanceAttenuation(float distanceYards) {
    if (!std::isfinite(distanceYards) || distanceYards >= kCameraShakeMaxDistance) {
        return 0.0f;
    }
    if (distanceYards <= kCameraShakeFullRadius) {
        return 1.0f;
    }
    return std::exp(-(distanceYards - kCameraShakeFullRadius) / kCameraShakeDecayLength);
}

/// Evaluate one CameraShakes.dbc record's signed displacement (yards) at `elapsedSeconds`
/// since trigger, before distance attenuation and user shakeScale.
[[nodiscard]] inline float evaluateCameraShakeSample(const CameraShakeEntry& entry,
                                                     float elapsedSeconds) {
    if (!std::isfinite(elapsedSeconds) || elapsedSeconds < 0.0f) {
        return 0.0f;
    }
    if (entry.amplitude <= 0.0f || entry.frequency <= 0.0f || entry.duration <= 0.0f) {
        return 0.0f;
    }
    const float tau = elapsedSeconds + entry.phase;
    if (tau >= entry.duration) {
        return 0.0f;
    }
    const float envelope = (entry.shakeType == 1)
        ? std::exp(-entry.coefficient * tau)
        : 1.0f;
    return (entry.amplitude * kCameraShakeAmplitudePerUnit) *
           envelope *
           std::sin(core::coords::TWO_PI * entry.frequency * tau);
}

/// Map per-axis displacements (axis 0 = forward XY, 1 = left XY, 2 = world +Z)
/// into render/world coordinates using the camera's horizontal forward direction.
[[nodiscard]] inline glm::vec3 cameraShakeBasisOffset(const glm::vec3& axisDisp,
                                                      const glm::vec3& cameraForward) {
    glm::vec2 fwdXY(cameraForward.x, cameraForward.y);
    const float lenSq = glm::dot(fwdXY, fwdXY);
    if (lenSq > 1e-8f) {
        fwdXY *= glm::inversesqrt(lenSq);
    } else {
        fwdXY = glm::vec2(1.0f, 0.0f);
    }
    const glm::vec2 leftXY(-fwdXY.y, fwdXY.x);
    const glm::vec2 horiz = fwdXY * axisDisp.x + leftXY * axisDisp.y;
    return glm::vec3(horiz, axisDisp.z);
}

/// Runtime accumulator for active CameraShakes.dbc impulses.
///
/// Multiple active shakes coexist with independent clocks. On each axis (0, 1, 2),
/// the impulse with the largest |displacement| wins that frame (verified on rapid
/// Kodo stride pairs: peak stays 1x rather than doubling, and when the earlier
/// impulse expires the later impulse seamlessly takes over). Separate axes combine
/// simultaneously.
class CameraShakeState {
public:
    struct ActiveShake {
        CameraShakeEntry entry;
        float attenuation = 1.0f;
        float elapsed = 0.0f;
        bool fresh = true;
    };

    void trigger(const CameraShakeEntry& entry, float attenuation = 1.0f) {
        if (attenuation <= 0.0f || entry.amplitude <= 0.0f || entry.frequency <= 0.0f) {
            return;
        }
        if (entry.phase >= entry.duration) {
            return;
        }
        active_.push_back(ActiveShake{
            .entry = entry,
            .attenuation = attenuation,
            .elapsed = 0.0f,
            .fresh = true,
        });
    }

    void clear() { active_.clear(); }
    [[nodiscard]] bool empty() const { return active_.empty(); }
    [[nodiscard]] size_t size() const { return active_.size(); }

    /// Advance active shakes by `deltaTime` and return per-axis displacement (yards)
    /// before user `shakeScale` and camera-basis rotation.
    ///
    /// A shake triggered after the previous camera update evaluates its initial
    /// sample near `elapsed = 0` on the very next camera update (or `deltaTime` if
    /// minimal), matching Native TBC 2.4.3's first-frame peak on Kodo Death (ID=10).
    [[nodiscard]] glm::vec3 step(float deltaTime) {
        if (active_.empty()) {
            return glm::vec3(0.0f);
        }
        const float dt = std::max(0.0f, deltaTime);
        glm::vec3 axisDisp(0.0f);
        size_t outIdx = 0;
        for (size_t i = 0; i < active_.size(); ++i) {
            ActiveShake s = active_[i];
            if (s.fresh) {
                s.fresh = false;
                // Advance at most a tiny fraction on the first rendered frame so
                // phase-offset shakes (e.g. ID=10 phase=0.16) hit their authored
                // initial peak rather than skipping 16-33 ms ahead.
                s.elapsed = std::min(dt, 0.004f);
            } else {
                s.elapsed += dt;
            }
            if (s.elapsed + s.entry.phase >= s.entry.duration) {
                continue;
            }
            const float val = evaluateCameraShakeSample(s.entry, s.elapsed) * s.attenuation;
            if (s.entry.axis < 3 && std::abs(val) > std::abs(axisDisp[s.entry.axis])) {
                axisDisp[s.entry.axis] = val;
            }
            active_[outIdx++] = s;
        }
        active_.resize(outIdx);
        return axisDisp;
    }

private:
    std::vector<ActiveShake> active_;
};

/// DBC lookup tables driving TBC 2.4.3 camera shakes:
/// - CameraShakes.dbc (shake parameters by ID)
/// - SpellEffectCameraShakes.dbc (up to 3 CameraShakes IDs for spells & M2 $SHK)
/// - CreatureModelData.dbc (FootstepShakeSize col 11 & DeathThudShakeSize col 12 -> CameraShakes ID)
class CameraShakeTable {
public:
    template <typename AssetMgr = pipeline::AssetManager>
    bool load(AssetMgr& assets) {
        const auto csDbc = assets.loadDBC("CameraShakes.dbc");
        const auto secsDbc = assets.loadDBC("SpellEffectCameraShakes.dbc");
        const auto cmdDbc = assets.loadDBC("CreatureModelData.dbc");
        const bool ok = load(csDbc.get(), secsDbc.get(), cmdDbc.get());
        if (ok) {
            LOG_INFO("CameraShakeTable: loaded ", shakes_.size(), " shakes, ",
                     spellEffectShakes_.size(), " spell effect shakes, ",
                     footstepShakeByModel_.size(), " footstep-shake models, ",
                     deathThudShakeByModel_.size(), " death-thud models");
        } else {
            LOG_WARNING("CameraShakeTable: CameraShakes.dbc unavailable");
        }
        return ok;
    }

    bool load(const pipeline::DBCFile* cameraShakesDbc,
              const pipeline::DBCFile* spellEffectShakesDbc,
              const pipeline::DBCFile* creatureModelDataDbc);

    [[nodiscard]] bool isLoaded() const { return !shakes_.empty(); }
    [[nodiscard]] size_t shakeCount() const { return shakes_.size(); }
    [[nodiscard]] size_t spellEffectShakeCount() const { return spellEffectShakes_.size(); }
    [[nodiscard]] size_t footstepModelCount() const { return footstepShakeByModel_.size(); }
    [[nodiscard]] size_t deathThudModelCount() const { return deathThudShakeByModel_.size(); }

    [[nodiscard]] const CameraShakeEntry* findShake(uint32_t shakeId) const {
        const auto it = shakes_.find(shakeId);
        return it != shakes_.end() ? &it->second : nullptr;
    }

    [[nodiscard]] std::array<uint32_t, 3> findSpellEffectShakes(uint32_t spellEffectShakeId) const {
        const auto it = spellEffectShakes_.find(spellEffectShakeId);
        return it != spellEffectShakes_.end() ? it->second : std::array<uint32_t, 3>{0, 0, 0};
    }

    [[nodiscard]] uint32_t footstepShakeId(uint32_t creatureModelDataId) const {
        const auto it = footstepShakeByModel_.find(creatureModelDataId);
        return it != footstepShakeByModel_.end() ? it->second : 0;
    }

    [[nodiscard]] uint32_t deathThudShakeId(uint32_t creatureModelDataId) const {
        const auto it = deathThudShakeByModel_.find(creatureModelDataId);
        return it != deathThudShakeByModel_.end() ? it->second : 0;
    }

private:
    std::unordered_map<uint32_t, CameraShakeEntry> shakes_;
    std::unordered_map<uint32_t, std::array<uint32_t, 3>> spellEffectShakes_;
    std::unordered_map<uint32_t, uint32_t> footstepShakeByModel_;
    std::unordered_map<uint32_t, uint32_t> deathThudShakeByModel_;
};

} // namespace rendering
} // namespace wowee
