#pragma once

/// What an M2 material's blend mode means for the pipeline it draws on.
///
/// A batch whose texture carries no alpha channel used to be forced onto the
/// cutout pipeline whenever its blend mode was 2 or higher. For blend mode 2 -
/// alpha blend - that is reasonable: blending by an alpha that is 1 everywhere
/// is the same as drawing opaque, so cutting out at least gives the artist's
/// silhouette a chance.
///
/// For the additive modes it is wrong, and destroys the effect. Additive does
/// not use alpha for transparency at all - black is what disappears, because
/// adding zero changes nothing. A glow card is authored exactly that way: an
/// opaque texture, bright in the middle, black at the edges.
///
/// Orgrimmar's bonfire is the case that showed it. Its glow is
/// GENERICGLOW_ALPHA_128.BLP - named for an alpha it does not have, alphaDepth
/// 0 in the file - on a material with blend mode 4. Forced to cutout, every
/// texel passed the test and the flame rendered as a flat opaque grey disc
/// sitting behind the logs.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wowee::rendering {

/// M2 material blend modes, as stored.
enum M2BlendMode : uint8_t {
    M2_BLEND_OPAQUE = 0,
    M2_BLEND_ALPHA_KEY = 1,   ///< cutout: alpha tested against a threshold
    M2_BLEND_ALPHA = 2,       ///< blended by the texture's alpha
    M2_BLEND_ADD = 3,         ///< added to what is behind; black is invisible
    M2_BLEND_ADD_ALPHA = 4,   ///< additive, scaled by alpha where there is one
    M2_BLEND_MODULATE = 5,
    M2_BLEND_MODULATE2X = 6,
};

inline bool m2BatchIsTransparent(uint8_t blendMode, bool effectModel, bool authoredMaterial) {
    return blendMode >= M2_BLEND_ALPHA || (effectModel && !authoredMaterial);
}

/// Back-face culling for meshes with an authored one-sided material (render
/// flags without 0x04), whatever their blend mode. Do not guess when a
/// material is missing, or when a draw group contains mirrored/degenerate
/// transforms whose winding can differ. The sky keeps its pipelines.
inline bool m2UseAuthoredCull(bool enabled, bool sky,
                             bool hasAuthoredMaterial, bool windingPreserved,
                             uint16_t materialFlags) {
    constexpr uint16_t kTwoSided = 0x04;
    return enabled && !sky && hasAuthoredMaterial && windingPreserved &&
           !(materialFlags & kTwoSided);
}

/// True when the mode adds to the framebuffer rather than covering it.
///
/// These never need an alpha channel: black is the transparent colour.
inline bool m2BlendIsAdditive(uint8_t blendMode) {
    return blendMode == M2_BLEND_ADD || blendMode == M2_BLEND_ADD_ALPHA;
}

/// Should this batch be alpha tested?
///
/// Alpha key is, that being what it means - but only where there is alpha to
/// key on. A blended batch whose texture has no alpha is tested as a fallback,
/// because blending by a missing alpha draws it solid. An additive batch never
/// is: it has nothing to test and does not need one.
///
/// An alpha key over a texture with no transparent texel is the case worth
/// naming. Every texel passes, so the test decides nothing - but it still puts
/// the batch on the cutout pipeline, where alpha-to-coverage decides how much
/// of each sample the batch covers. Tirisfal's canopy trees are two batches,
/// and their trunk is exactly this: TirrisFallCanopyTree01_Trunk is DXT1 with
/// no punch-through block anywhere in it, drawn alpha-keyed.
///
/// Canopytree07, the one of the seven whose trunk is plain opaque, was read
/// here as the one that rendered right. It was not. Its trunk samples
/// SilverPineTree01TrunkSkin, which does carry alpha, and the foliage rule in
/// m2_renderer_render.cpp forced it onto the cutout pipeline whatever this
/// function said - so it had the same hole through its middle that every
/// Silverpine tree had. Opaque was not reaching the pipeline at all until that
/// rule learned to ask BLPImage::alphaIsSilhouette.
///
/// Safe to lean on, because hasAlpha is measured rather than guessed:
/// BLPImage::hasTransparency walks level 0 and, for DXT1, counts a block only
/// when it is in punch-through mode and a texel actually selects index 3.
inline bool m2BatchNeedsAlphaTest(uint8_t blendMode, bool hasAlpha) {
    if (blendMode == M2_BLEND_ALPHA_KEY) return hasAlpha;
    if (m2BlendIsAdditive(blendMode)) return false;
    return blendMode >= M2_BLEND_ALPHA && !hasAlpha;
}

/// Resolved texture stage combiner modes (0 Opaque, 1 Mod, 2 Decal, 3 Add, 4 Mod2x).
struct M2StageCombiners {
    int32_t stage0 = 1;
    int32_t stage1 = 1;
};

/// Resolve M2 per-stage texture combiners from model.textureCombinerCombos.
///
/// In TBC 2.4.3 (M2 v260-263, wow.exe build 8606 @ 0x744718), when header
/// globalFlags & 0x08 is set, the stage setup loop reads combinerCombos[stage]
/// directly (0, 1) without adding batch.shader. Coilfang_Marsh_Waterfall.m2
/// (v260, globalFlags 0x18) has combinerCombos = [1, 1, 4, 4, ...] with pool
/// batches b[39] (shader=2) and b[40]/b[61] (shader=1): native 2.4.3 renders
/// them as (1, 1) Mod/Mod (translucent cyan mist), whereas indexing by
/// batch.shader selects Mod2x (4) and blows out the pool to solid white.
/// Sunwell_Fire_Barrier_Ext.m2 (v263, combinerCombos = [1, 4], shader=0) still
/// resolves to (1, 4) Mod/Mod2x. WotLK+ (v264+) indexes combinerCombos[shader + stage].
/// Special shader IDs with bit 0x8000 keep modulating both stages.
inline M2StageCombiners m2ResolveStageCombiners(uint32_t modelVersion,
                                                const std::vector<uint16_t>& combinerCombos,
                                                uint16_t batchShader,
                                                uint16_t textureCount) {
    M2StageCombiners out{};
    if (combinerCombos.empty() || (batchShader & 0x8000u) != 0u) {
        return out;
    }
    const size_t base = (modelVersion < 264u) ? 0u : static_cast<size_t>(batchShader);
    const size_t stages = (textureCount >= 2u) ? 2u : 1u;
    if (base + stages > combinerCombos.size()) {
        return out;
    }
    out.stage0 = static_cast<int32_t>(combinerCombos[base]);
    if (textureCount >= 2u) {
        out.stage1 = static_cast<int32_t>(combinerCombos[base + 1u]);
    }
    return out;
}

}  // namespace wowee::rendering
