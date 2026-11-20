#include "ui/autocast_shine.hpp"

#include "pipeline/m2_loader.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace wowee {
namespace ui {

namespace {

float sampleTrackFloatFirst(const pipeline::M2AnimationTrack& track, float fallback) {
    if (!track.sequences.empty() && !track.sequences[0].floatValues.empty()) {
        return track.sequences[0].floatValues[0];
    }
    return fallback;
}

glm::vec3 interpCurveVec3(const std::vector<float>& times,
                          const std::vector<glm::vec3>& values,
                          float t) {
    if (values.empty()) return glm::vec3(1.0f);
    if (values.size() == 1 || times.size() != values.size()) return values.front();
    if (t <= times.front()) return values.front();
    if (t >= times.back()) return values.back();
    for (size_t i = 0; i + 1 < times.size(); ++i) {
        if (t <= times[i + 1]) {
            const float span = times[i + 1] - times[i];
            const float u = span > 1e-6f ? (t - times[i]) / span : 0.0f;
            return glm::mix(values[i], values[i + 1], u);
        }
    }
    return values.back();
}

float interpCurveFloat(const std::vector<float>& times,
                       const std::vector<float>& values,
                       float t) {
    if (values.empty()) return 1.0f;
    if (values.size() == 1 || times.size() != values.size()) return values.front();
    if (t <= times.front()) return values.front();
    if (t >= times.back()) return values.back();
    for (size_t i = 0; i + 1 < times.size(); ++i) {
        if (t <= times[i + 1]) {
            const float span = times[i + 1] - times[i];
            const float u = span > 1e-6f ? (t - times[i]) / span : 0.0f;
            return values[i] + (values[i + 1] - values[i]) * u;
        }
    }
    return values.back();
}

float hashAngle(uint32_t emitterIndex, int64_t birthIndex) {
    uint32_t h = static_cast<uint32_t>(birthIndex) ^ (emitterIndex * 0x9E3779B9u);
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    constexpr float kTwoPi = 6.2831853071795864769f;
    return (static_cast<float>(h & 0x00FFFFFFu) / 16777216.0f) * kTwoPi;
}

} // namespace

AutoCastShineModel AutoCastShineModel::fromM2(const pipeline::M2Model& model) {
    AutoCastShineModel out;
    if (model.sequences.empty() || model.bones.empty() || model.particleEmitters.empty()) {
        return out;
    }

    out.durationMs = model.sequences[0].duration;
    if (!model.textures.empty()) {
        out.texturePath = model.textures[0].filename;
    }

    glm::vec2 minXY(std::numeric_limits<float>::max());
    glm::vec2 maxXY(std::numeric_limits<float>::lowest());

    out.bones.reserve(model.bones.size());
    for (const auto& bone : model.bones) {
        BoneTrack track;
        track.pivot = bone.pivot;
        if (!bone.translation.sequences.empty()) {
            const auto& seq = bone.translation.sequences[0];
            track.timestampsMs = seq.timestamps;
            track.translations = seq.vec3Values;
            const size_t n = std::min(track.timestampsMs.size(), track.translations.size());
            track.timestampsMs.resize(n);
            track.translations.resize(n);
            for (size_t i = 0; i < n; ++i) {
                const glm::vec3 p = track.pivot + track.translations[i];
                minXY.x = std::min(minXY.x, p.x);
                minXY.y = std::min(minXY.y, p.y);
                maxXY.x = std::max(maxXY.x, p.x);
                maxXY.y = std::max(maxXY.y, p.y);
            }
        } else {
            minXY.x = std::min(minXY.x, track.pivot.x);
            minXY.y = std::min(minXY.y, track.pivot.y);
            maxXY.x = std::max(maxXY.x, track.pivot.x);
            maxXY.y = std::max(maxXY.y, track.pivot.y);
        }
        out.bones.push_back(std::move(track));
    }

    out.boundsMin = minXY;
    out.boundsMax = maxXY;

    out.emitters.reserve(model.particleEmitters.size());
    for (const auto& src : model.particleEmitters) {
        Emitter em;
        em.boneIndex = src.bone;
        em.blendingType = src.blendingType;
        em.lifespan = std::max(sampleTrackFloatFirst(src.lifespan, 1.0f), 0.01f);
        em.emissionRate = std::max(sampleTrackFloatFirst(src.emissionRate, 300.0f), 1.0f);
        em.midpoint = src.midPoint;
        em.spin = src.spin;
        em.colorTimes = src.particleColor.timestamps;
        em.colorValues = src.particleColor.vec3Values;
        em.alphaTimes = src.particleAlpha.timestamps;
        em.alphaValues = src.particleAlpha.floatValues;
        em.scaleTimes = src.particleScale.timestamps;
        em.scaleValues = src.particleScale.floatValues;
        out.emitters.push_back(std::move(em));
    }

    return out;
}

glm::vec3 AutoCastShineModel::evaluateBonePosition(size_t boneIndex, float timeMs) const {
    if (boneIndex >= bones.size()) return glm::vec3(0.0f);
    const auto& bone = bones[boneIndex];
    if (bone.timestampsMs.empty()) return bone.pivot;

    float t = 0.0f;
    if (durationMs > 0) {
        t = std::fmod(timeMs, static_cast<float>(durationMs));
        if (t < 0.0f) t += static_cast<float>(durationMs);
    }

    if (t <= static_cast<float>(bone.timestampsMs.front())) {
        return bone.pivot + bone.translations.front();
    }
    if (t >= static_cast<float>(bone.timestampsMs.back())) {
        return bone.pivot + bone.translations.back();
    }
    for (size_t i = 0; i + 1 < bone.timestampsMs.size(); ++i) {
        const float t0 = static_cast<float>(bone.timestampsMs[i]);
        const float t1 = static_cast<float>(bone.timestampsMs[i + 1]);
        if (t <= t1) {
            const float span = t1 - t0;
            const float u = span > 1e-6f ? (t - t0) / span : 0.0f;
            return bone.pivot + glm::mix(bone.translations[i], bone.translations[i + 1], u);
        }
    }
    return bone.pivot + bone.translations.back();
}

glm::vec2 AutoCastShineModel::evaluateNormalizedPoint(size_t boneIndex, float timeMs) const {
    const glm::vec3 pos = evaluateBonePosition(boneIndex, timeMs);
    const float spanX = boundsMax.x - boundsMin.x;
    const float spanY = boundsMax.y - boundsMin.y;
    const float u = spanX > 1e-6f ? std::clamp((pos.x - boundsMin.x) / spanX, 0.0f, 1.0f) : 0.5f;
    const float v = spanY > 1e-6f ? std::clamp(1.0f - (pos.y - boundsMin.y) / spanY, 0.0f, 1.0f) : 0.5f;
    return glm::vec2(u, v);
}

void AutoCastShineModel::evaluateSprites(double timeSeconds, float x0, float y0, float x1, float y1,
                                         float modelScale, float widgetAlpha,
                                         std::vector<AutoCastShineSprite>& out) const {
    out.clear();
    if (!isValid() || widgetAlpha <= 0.0f) return;

    const float rw = x1 - x0;
    const float rh = y1 - y0;
    if (rw <= 1.0f || rh <= 1.0f) return;

    const float modelSpan = 0.5f * ((boundsMax.x - boundsMin.x) + (boundsMax.y - boundsMin.y));
    const float rectSpan = 0.5f * (rw + rh);
    const float scaleRatio = (modelScale > 0.0f ? modelScale : 1.2f) / 1.2f;
    const float modelToPixels = (rectSpan / modelSpan) * scaleRatio;

    // Sample particles on a fixed birth-time grid so each spawned GlowStar stays
    // stationary at its birth point on the perimeter as the emitter moves on.
    constexpr int kSamplesPerEmitter = 64;
    out.reserve(emitters.size() * kSamplesPerEmitter);

    for (size_t ei = 0; ei < emitters.size(); ++ei) {
        const auto& em = emitters[ei];
        if (em.boneIndex >= bones.size()) continue;

        const double dt = static_cast<double>(em.lifespan) / static_cast<double>(kSamplesPerEmitter);
        const int64_t latestBirth = static_cast<int64_t>(std::floor(timeSeconds / dt));

        for (int k = 0; k < kSamplesPerEmitter; ++k) {
            const int64_t birthIdx = latestBirth - k;
            const double birthTime = static_cast<double>(birthIdx) * dt;
            const float age = static_cast<float>(timeSeconds - birthTime);
            if (age < 0.0f || age >= em.lifespan) continue;

            const float lifeRatio = age / em.lifespan;
            const float alpha = interpCurveFloat(em.alphaTimes, em.alphaValues, lifeRatio) * widgetAlpha;
            if (alpha <= 0.004f) continue;

            const float authoredScale = interpCurveFloat(em.scaleTimes, em.scaleValues, lifeRatio);
            const float halfSize = authoredScale * modelToPixels;
            if (halfSize <= 0.25f) continue;

            const glm::vec3 color = interpCurveVec3(em.colorTimes, em.colorValues, lifeRatio);
            const glm::vec2 uv = evaluateNormalizedPoint(em.boneIndex, static_cast<float>(birthTime * 1000.0));

            AutoCastShineSprite sprite;
            sprite.x = x0 + uv.x * rw;
            sprite.y = y0 + uv.y * rh;
            sprite.halfSize = halfSize;
            sprite.rotation = hashAngle(static_cast<uint32_t>(ei), birthIdx) + em.spin * age;
            sprite.r = std::clamp(color.r, 0.0f, 1.0f);
            sprite.g = std::clamp(color.g, 0.0f, 1.0f);
            sprite.b = std::clamp(color.b, 0.0f, 1.0f);
            sprite.a = std::clamp(alpha, 0.0f, 1.0f);
            out.push_back(sprite);
        }
    }
}

} // namespace ui
} // namespace wowee
