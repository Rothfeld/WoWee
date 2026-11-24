#include "rendering/weapon_trail.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace wowee {
namespace rendering {

std::optional<WeaponTrailSpec> parseWeaponTrailCharProc(float paramZero, float paramOne,
                                                        float paramTwo, float paramThree) {
    assert(std::isfinite(paramZero) && paramZero >= 0.0f && paramZero < 16777216.0f);
    assert(std::isfinite(paramOne) && paramOne >= 0.0f);
    assert(std::isfinite(paramTwo) && paramTwo >= 0.0f);
    assert(std::isfinite(paramThree) && paramThree >= 0.0f && paramThree <= 255.0f);

    // Native TBC 2.4.3 converts CharParamTwo (milliseconds) to whole seconds
    // via integer division by 1000. Sub-second melee kits (300..700 ms, e.g.
    // Mortal Strike/Heroic Strike/Overpower kit 324 at 600 ms, Cleave kit 366
    // at 600 ms, Sinister Strike kit 399 at 400 ms, Feint kit 823 at 700 ms)
    // truncate to 0 s and emit no weapon trail; kits >= 1000 ms (Whirlwind
    // kit 369 at 2000 ms -> 2 s, Charge/Surge kit 44 and Tackle Hit kit 8456
    // at 1000 ms -> 1 s) emit for that whole-second duration.
    const uint32_t wholeSeconds = static_cast<uint32_t>(paramTwo) / 1000u;
    if (wholeSeconds == 0u || paramThree <= 0.0f) {
        return std::nullopt;
    }

    const auto rgb = static_cast<uint32_t>(paramZero);
    WeaponTrailSpec spec;
    spec.color = glm::vec3(
        static_cast<float>((rgb >> 16) & 0xFFu) / 255.0f,
        static_cast<float>((rgb >> 8) & 0xFFu) / 255.0f,
        static_cast<float>(rgb & 0xFFu) / 255.0f);
    spec.fadeOutRate = paramOne;
    spec.durationSec = static_cast<float>(wholeSeconds);
    spec.alpha = std::clamp(paramThree / 255.0f, 0.0f, 1.0f);
    return spec;
}

std::pair<glm::vec3, glm::vec3> weaponBladeLocalSpan(const glm::vec3& visualBoundMin,
                                                     const glm::vec3& visualBoundMax) {
    if (visualBoundMax.x > 0.25f) {
        return {glm::vec3(visualBoundMax.x * 0.20f, 0.0f, 0.0f),
                glm::vec3(visualBoundMax.x, 0.0f, 0.0f)};
    }
    if (-visualBoundMin.z > visualBoundMax.x && -visualBoundMin.z > 0.10f) {
        return {glm::vec3(0.0f, 0.0f, 0.0f),
                glm::vec3(0.0f, 0.0f, visualBoundMin.z)};
    }
    return {glm::vec3(0.0f, 0.0f, 0.0f),
            glm::vec3(std::max(visualBoundMax.x, 0.25f), 0.0f, 0.0f)};
}

void stepWeaponTrailChannel(WeaponTrailChannel& channel, const WeaponTrailSpec& spec,
                            float dt, bool emitting,
                            const std::optional<std::pair<glm::vec3, glm::vec3>>& bladeWorld) {
    assert(std::isfinite(dt) && dt >= 0.0f);

    // CharParamOne (fadeOutRate) is alpha units (0..255) decayed per 50ms tick (20 Hz).
    // Example: Heroic Strike / Whirlwind (fadeOutRate=20, alpha=100) decays over
    // 100 / (20 * 20) = 0.25 s, matching Native TBC 2.4.3 captures.
    // When fadeOutRate == 0, edges hold until emission stops, then fade over 0.25 s.
    float alphaDecayPerSec = 0.0f;
    if (spec.fadeOutRate > 0.0f) {
        alphaDecayPerSec = (spec.fadeOutRate * 20.0f) / 255.0f;
    } else if (!emitting) {
        alphaDecayPerSec = spec.alpha / 0.25f;
    }

    if (alphaDecayPerSec > 0.0f && dt > 0.0f) {
        const float deltaAlpha = alphaDecayPerSec * dt;
        for (auto& edge : channel.edges) {
            edge.alpha = std::max(0.0f, edge.alpha - deltaAlpha);
        }
    }

    // Keep one zero-alpha tail edge so the last quad fades smoothly to 0,
    // dropping older fully transparent edges from the front.
    while (channel.edges.size() >= 2 &&
           channel.edges[0].alpha <= 0.0f &&
           channel.edges[1].alpha <= 0.0f) {
        channel.edges.erase(channel.edges.begin());
    }
    if (channel.edges.size() == 1 && channel.edges[0].alpha <= 0.0f) {
        channel.edges.clear();
    }

    if (!emitting || !bladeWorld.has_value()) {
        return;
    }

    const auto& [basePos, tipPos] = *bladeWorld;
    channel.sampleAccum += dt;

    if (channel.edges.empty()) {
        channel.edges.push_back({basePos, tipPos, spec.alpha});
        channel.sampleAccum = 0.0f;
        return;
    }

    const auto& last = channel.edges.back();
    const glm::vec3 tipDelta = tipPos - last.tip;
    const glm::vec3 baseDelta = basePos - last.base;
    const bool moved = glm::dot(tipDelta, tipDelta) >= kWeaponTrailMinMoveSq ||
                       glm::dot(baseDelta, baseDelta) >= kWeaponTrailMinMoveSq;

    if (!moved) {
        // Keep the head edge locked to a stationary weapon without stacking
        // degenerate zero-area quads.
        if (channel.edges.size() == 1) {
            channel.edges.back().base = basePos;
            channel.edges.back().tip = tipPos;
            channel.edges.back().alpha = spec.alpha;
        }
        return;
    }

    if (channel.sampleAccum >= kWeaponTrailSampleIntervalSec || channel.edges.size() == 1) {
        channel.edges.push_back({basePos, tipPos, spec.alpha});
        channel.sampleAccum = 0.0f;
        if (channel.edges.size() > kWeaponTrailMaxEdges) {
            channel.edges.erase(channel.edges.begin());
        }
    } else {
        // Between sample intervals, advance the newest edge to track the blade
        // tip continuously without lag.
        channel.edges.back().base = basePos;
        channel.edges.back().tip = tipPos;
        channel.edges.back().alpha = spec.alpha;
    }
}

bool isWeaponTrailChannelDead(const WeaponTrailChannel& channel) {
    if (channel.edges.size() < 2) return true;
    for (const auto& edge : channel.edges) {
        if (edge.alpha > 0.0f) return false;
    }
    return true;
}

float resolveChannelWeaponTrailDuration(float kitDurationSec, float channelRemainingSec) {
    assert(std::isfinite(kitDurationSec) && kitDurationSec >= 0.0f);
    assert(std::isfinite(channelRemainingSec));
    if (channelRemainingSec > 0.0f) {
        return std::min(kitDurationSec, channelRemainingSec);
    }
    return kitDurationSec;
}

void stopWeaponTrailEmission(ActiveUnitWeaponTrail& trail) {
    trail.spec.durationSec = std::min(trail.spec.durationSec, trail.elapsed);
}

}  // namespace rendering
}  // namespace wowee
