#pragma once

/// Rules for drawing a SpellChainEffects.dbc beam (channel beams such as
/// Hydross' Blue Beam 38015, chain 17) that do not need a renderer.
///
/// Measured against build 8606 with paired captures of the same server beam
/// (_experiments/20261118T1328Z.hydross_beam_scroll in the workspace root).

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <vector>

namespace wowee::rendering {

/// Texture coordinate along a beam, s yards from the caster on a beam
/// `length` yards long, t seconds after the beam appeared.
///
/// One copy of the texture spans the whole beam, whatever its length, and it
/// travels |TexCoordScale| beam lengths a second, toward the target when
/// TexCoordScale is positive; a negative scale also mirrors the texture.
/// The chain textures (SoulBeam, ManaBeam, WhiteBeam) hold two waves, so a
/// point on the beam sees 2 x |TexCoordScale| crests a second. Native
/// (_experiments/20261118T221857Z.chain_rate): ManaBeam (chain 4, -2) at
/// 24 yd and SoulBeam (chain 30, Archimonde's Drain World Tree, +2): the
/// brightness along the beam has its longest period at the beam's length,
/// moving 2 lengths/s (2.0 Hz), and its strongest crests at half the length
/// (4.0 Hz); WhiteBeam (1.66) crests 3.34 Hz. Two copies (TextureLength,
/// 2.0 on 317 of the 320 rows) drew twice the waves crossing twice as fast.
inline float chainTexCoord(float s, float length, float texCoordScale, float t) {
    assert(length > 0.0f);
    const float mirror = texCoordScale < 0.0f ? -1.0f : 1.0f;
    return mirror * (s / length - texCoordScale * t);
}

/// The further strands a row's Combo string adds, drawn whole between the
/// same ends. Each entry is 4 bytes: a row ID's bits 0-6 in the first, ID
/// bit 7 as 0x02 and ID bit 8 as 0x20 of the fourth; bytes 2-3 are always
/// 0x01 and the fourth's 0x10 is not interpreted. Native Tear of Azzinoth
/// Summon Channel 39857 (237: "o..\x03", "o..\x13", 239 twice) draws three
/// green waves; Sunshatter 45215 (215) a blue bolt (209), a broad
/// purple-white wave (211) and thin purple bolts (217) beside its green one.
/// An entry with 0x20 set adds another copy of the row itself (`parent`),
/// whatever ID it spells: build 8606 draws Fel Consumption 45023 (314, two
/// entries spelling 315 Beam_Purple) as three green SoulBeam strands lit
/// together, no purple; the Fel Crystals' Fel Crystal Visual 44355 (292, one
/// entry spelling 293 DrainManaLightning) as two thin yellow strands, no
/// green lightning (_experiments/20261119T1500Z.beam_bend_motion).
inline std::vector<uint32_t> chainComboStrands(std::string_view combo, uint32_t parent) {
    assert(combo.size() % 4 == 0);
    std::vector<uint32_t> ids;
    for (size_t i = 0; i < combo.size(); i += 4) {
        const auto low = static_cast<uint8_t>(combo[i]);
        const auto high = static_cast<uint8_t>(combo[i + 3]);
        ids.push_back((high & 0x20u) ? parent : low | ((high & 0x02u) << 6));
    }
    return ids;
}

/// How lit a pulsing row is s yards from the caster on a beam `length`
/// yards long, t seconds after it appeared (0..1). Rows with Flags 0x20 and
/// a PulseSpeed light a window PulseOnLength yards long, fading over
/// PulseFadeLength on each side, that sweeps along the beam at PulseSpeed
/// yd/s, from the caster when positive, from the target when negative; once
/// it has left the far end it enters again. Build 8606, Fel Consumption
/// 45023 (314: Flags 0x68, -13 yd/s, 3.8 on, 1.8 fade) over 24 yd: a lit
/// band about 7 yd long runs from the target to the caster and the next
/// enters about 2.4 s after the last; Drain Life (8: Flags 0x44, -24, 4, 1)
/// is lit whole (_experiments/20261119T1500Z.beam_bend_motion).
inline float chainPulse(float s, float length, float speed, float onLength, float fadeLength, float t) {
    assert(speed != 0.0f && onLength >= 0.0f && fadeLength >= 0.0f && length > 0.0f);
    const float span = length + onLength + 2.0f * fadeLength;
    const float head = std::fmod(std::abs(speed) * t, span) - fadeLength;  // leading edge of the lit part
    const float x = speed > 0.0f ? s : length - s;                          // distance the window has come
    const float behind = head - x;                                          // yards behind the leading edge
    if (behind < 0.0f) return fadeLength > 0.0f ? std::clamp(1.0f + behind / fadeLength, 0.0f, 1.0f) : 0.0f;
    if (behind <= onLength) return 1.0f;
    return fadeLength > 0.0f ? std::clamp(1.0f - (behind - onLength) / fadeLength, 0.0f, 1.0f) : 0.0f;
}

/// One frame of a beam vertex's noise offset: it keeps JointSmoothness of
/// where it was and moves the rest toward a fresh random offset (drawn every
/// frame, or every MinDurationBetweenJointJumps when that is set). A new
/// beam starts at a full random draw. Build 8606, Blue Beam 38034 (chain 4:
/// NoiseScale 0.01, JointSmoothness 0.75) between still units 24 yd apart,
/// texture motion cancelled: the bends' correlation from one frame to the
/// next is 0.74-0.75 whether native renders 15, 30 or 57 frames a second
/// (0.53 after 2 frames, 0.30 after 4 at 57 fps), so it is a per-frame step,
/// and the bends' rms is 0.93 px at every rate, what this step gives for a
/// uniform draw in the noise disc (0.38 of the draw's own rms). The bends
/// are new within a quarter second at 30 fps; drawn once and kept, they
/// stood still (_experiments/20261119T1500Z.beam_bend_motion).
template <typename T>
T chainNoiseStep(const T& offset, const T& drawn, float smoothness) {
    assert(smoothness >= 0.0f && smoothness <= 1.0f);
    return offset * smoothness + drawn * (1.0f - smoothness);
}

/// How far the beam's interior vertices stray from the straight line:
/// NoiseScale of the beam's length. Native ManaBeam (0.01) at 28 yd kinks by
/// about 0.15 yd on screen; Sunshatter's bolt (0.019) at 24 yd by about 0.5 yd.
inline float chainNoiseRadius(float noiseScale, float length) { return noiseScale * length; }

/// How far joint i (1-based, of JointCount spread evenly between the ends)
/// may stray from the line, in yards: JointOffsetRadius, times
/// MinorJointScale on every JointsPerMinorJoint-th joint and MajorJointScale
/// on every (JointsPerMinorJoint x MinorJointsPerMajorJoint)-th. Native
/// Open Portal from Shattrath 46801 (chain 411: 27 joints, 0.07, 3, 4, 3.7,
/// 8.45) over 24 yd: strands kinked about every yard, bent over several,
/// spread about a yard across; WoWee drew them straight (NoiseScale 0).
struct ChainJoints {
    uint32_t count = 0, perMinor = 0, minorPerMajor = 0;
    float radius = 0, minorScale = 0, majorScale = 0;
};
inline float chainJointRadius(const ChainJoints& joints, uint32_t i) {
    assert(i >= 1 && i <= joints.count);
    if (joints.perMinor && joints.minorPerMajor && i % (joints.perMinor * joints.minorPerMajor) == 0)
        return joints.radius * joints.majorScale;
    if (joints.perMinor && i % joints.perMinor == 0) return joints.radius * joints.minorScale;
    return joints.radius;
}

/// The joints' offset at fraction f (0..1) along the beam: straight between
/// neighbouring joints, zero at the ends. `offsets` holds one per joint.
template <typename T>
T chainJointOffsetAt(const std::vector<T>& offsets, float f) {
    const size_t n = offsets.size();
    if (n == 0) return T(0);
    const float x = std::clamp(f, 0.0f, 1.0f) * static_cast<float>(n + 1);
    const size_t k = static_cast<size_t>(x);
    const float a = x - static_cast<float>(k);
    const T before = (k == 0 || k > n) ? T(0) : offsets[k - 1];
    const T after = k >= n ? T(0) : offsets[k];
    return before * (1.0f - a) + after * a;
}

/// `offsets` (one per beam vertex, ends included) read at the fractions of a
/// beam of `count` vertices: the same shape over a new segment count.
template <typename T>
std::vector<T> chainResample(const std::vector<T>& offsets, size_t count) {
    assert(offsets.size() >= 2 && count >= 2);
    std::vector<T> out(count);
    for (size_t i = 0; i < count; ++i) {
        const float x = static_cast<float>(i) / static_cast<float>(count - 1) * static_cast<float>(offsets.size() - 1);
        const size_t k = std::min(static_cast<size_t>(x), offsets.size() - 2);
        const float a = x - static_cast<float>(k);
        out[i] = offsets[k] * (1.0f - a) + offsets[k + 1] * a;
    }
    return out;
}

/// CastKit chain beam segment timing (seconds after SMSG_SPELL_GO).
struct CastBeamTiming {
    float delay = 0.0f;     // seconds before this segment lights
    float duration = 0.0f;  // seconds this segment stays lit
};

/// Timing of a CastKit chain beam segment `hopIndex` (0-based) from
/// SpellChainEffects.dbc SegDuration (field 5, ms) and SegDelay (field 6, ms).
///
/// Measured against build 8606:
/// - Chain Heal (chain 2: SegDuration 2000 ms, SegDelay 200 ms): lights 0.20 s
///   after SMSG_SPELL_GO and stays lit 2.00 s (single-target and multi-target).
/// - Chain Lightning (chain 1: 1000 ms, 300 ms): lights 0.30 s after SPELL_GO.
/// - Forked Lightning (chain 95: 1000 ms, 0 ms): lights immediately for 1.00 s.
/// - Spite (chain 220: 6499 ms, 300 ms): lasts 6.5 s.
/// When SegDuration is 0, falls back to the kit's model/spell duration.
inline CastBeamTiming castBeamTiming(uint32_t segDurationMs, uint32_t segDelayMs,
                                     float fallbackSeconds, size_t hopIndex = 0,
                                     bool chained = false) {
    const float duration = segDurationMs > 0
        ? static_cast<float>(segDurationMs) / 1000.0f
        : std::max(0.0f, fallbackSeconds);
    const float delaySec = static_cast<float>(segDelayMs) / 1000.0f;
    const float delay = chained ? delaySec * static_cast<float>(hopIndex + 1) : delaySec;
    return {.delay = delay, .duration = duration};
}

}  // namespace wowee::rendering
