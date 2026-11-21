#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>

#include "pipeline/m2_loader.hpp"

namespace wowee::rendering {

/// UV span threshold separating atlas sub-rect overhangs from genuinely tiled
/// detail strips across TBC 2.4.3 GroundEffectDoodad models.
///
/// Multi-sprite detail atlases (BarrensDetailDoodad_256, DustwallowDetailDoodads_256,
/// EasternPlaguelandsDetails_256, etc.) have single-sprite UV spans <= 1.018
/// even when triangle corners overhang [0, 1] (e.g. BarGra05 v=-0.010..1.001,
/// BarGra06 v=0.489..1.313, DwmGra03 u=0.706..1.023). Genuinely repeating
/// detail strips span >= 1.47 UV units along their tiled axis (LchGra02/03 on
/// WetlandsTallGrass01 u=-0.406..2.229; LakGra02/03 on LakeDetails v=0.007..2.104).
inline constexpr float kM2GroundDetailTilingSpanThreshold = 1.1f;

/// 8-bit alpha reference (128 / 255, D3D9 ALPHAREF 0x80) separating the
/// depth-writing opaque blade core (Pass 1A) from the non-depth-writing blended
/// anti-aliased fringe (Pass 1B) on TBC 2.4.3 ground-effect detail doodads.
inline constexpr float kM2GroundDetailCoreAlphaCutoff = 128.0f / 255.0f;

/// Minimum non-zero alpha rendered in the soft fringe pass (Pass 1B).
inline constexpr float kM2GroundDetailFringeAlphaMin = 1.0f / 255.0f;

/// Push-constant drawMode values for ground-detail two-pass rendering.
inline constexpr int kM2DrawModeGroundDetailCore = 2;
inline constexpr int kM2DrawModeGroundDetailFringe = 3;

/// Compute UV extent (uSpan, vSpan) across finite vertex UVs for uvSet (0 or 1).
[[nodiscard]] inline std::pair<float, float> m2GroundDetailTextureUVSpan(
    std::span<const pipeline::M2Vertex> vertices,
    uint32_t uvSet = 0) {
    const uint32_t setIdx = uvSet > 1u ? 0u : uvSet;
    float uMin = std::numeric_limits<float>::max();
    float uMax = std::numeric_limits<float>::lowest();
    float vMin = std::numeric_limits<float>::max();
    float vMax = std::numeric_limits<float>::lowest();
    bool any = false;
    for (const auto& v : vertices) {
        const float tu = v.texCoords[setIdx].x;
        const float tv = v.texCoords[setIdx].y;
        if (!std::isfinite(tu) || !std::isfinite(tv)) continue;
        uMin = std::min(uMin, tu);
        uMax = std::max(uMax, tu);
        vMin = std::min(vMin, tv);
        vMax = std::max(vMax, tv);
        any = true;
    }
    if (!any) return {0.0f, 0.0f};
    return {uMax - uMin, vMax - vMin};
}

/// Effective M2Texture wrap flags (bit 0 = WrapS, bit 1 = WrapT) for a
/// ground-effect detail doodad.
///
/// 407 of 496 TBC GroundEffectDoodad M2s leave M2Texture.flags = 3 (WrapS|WrapT)
/// while packing multiple vegetation/rock sprites onto one shared 256x256 atlas.
/// Repeating UVs that merely overhang [0, 1] wraps into opaque stems/rocks on
/// the opposite atlas border (floating mid-air streaks above BarGra05/DwmGra03
/// and wrapped grass tendrils under BarGra06). Keep WrapS/WrapT only on axes
/// whose authored UV span genuinely tiles past one full texture width/height.
[[nodiscard]] inline uint32_t m2GroundDetailEffectiveTexFlags(
    uint32_t authoredTexFlags,
    float uSpan,
    float vSpan) {
    uint32_t out = authoredTexFlags & ~0x3u;
    if ((authoredTexFlags & 0x1u) != 0u && uSpan > kM2GroundDetailTilingSpanThreshold) {
        out |= 0x1u;
    }
    if ((authoredTexFlags & 0x2u) != 0u && vSpan > kM2GroundDetailTilingSpanThreshold) {
        out |= 0x2u;
    }
    return out;
}

/// Native TBC 2.4.3 samples ground-effect detail atlases (World\NoDXT\Detail\*)
/// at mip 0 (D3DSAMP_MIPFILTER = D3DTEXF_NONE, maxLod = 0). Several shipped TBC
/// detail BLPs (BarrensDetailDoodad_256, NewElynnGrassFlowerRock128,
/// AeriePeaksDetailDoodads_256) carry 1-bit binary mip 1..N chains that fuse
/// seed heads into rectangular bars and bleed adjacent atlas sprites across seams.
[[nodiscard]] inline constexpr bool m2GroundDetailDisableMipmaps(bool isGroundDetail) {
    return isGroundDetail;
}

/// Pass 1A predicate: opaque depth-writing core of a ground-detail blade.
[[nodiscard]] inline constexpr bool m2GroundDetailCorePassesAlpha(float alpha) {
    return alpha >= kM2GroundDetailCoreAlphaCutoff;
}

/// Pass 1B predicate: soft non-depth-writing fringe of a ground-detail blade.
/// Strictly disjoint from Pass 1A so core texels are never double-shaded and
/// near-transparent fringes never write depth over crossing blades or water.
[[nodiscard]] inline constexpr bool m2GroundDetailFringePassesAlpha(float alpha) {
    return alpha >= kM2GroundDetailFringeAlphaMin && alpha < kM2GroundDetailCoreAlphaCutoff;
}

}  // namespace wowee::rendering
