#pragma once

/// WMO material classes, from MOMT flags and shader only.
///
/// Texture names do not decide: TOWER_WINDOW_01 (ruined guard towers) and
/// BM_ZEPPELIN_WINDOWBASE01 are stone walls whose names say window.

#include <cstdint>
#include <string>

namespace wowee::rendering {

/// MOMT shader variants with a specular term. Diffuse and emissive variants
/// do not acquire highlights merely because they use the same GPU pipeline.
constexpr bool wmoShaderUsesSpecular(uint32_t shader) {
    return shader == 1 || shader == 2 || shader == 5 || shader == 7 ||
           shader == 11 || shader == 12;
}

/// MOMT shader variants that sample texture2 as a spherical environment map:
/// shader 3 (Env, e.g. Karazhan ballroom floor / Stratholme ice) and
/// shader 5 (EnvMetal, e.g. Coilfang Reservoir pipes / Draenei crystal walls /
/// Sunwell inner floor).
constexpr bool wmoShaderUsesEnvMap(uint32_t shader) {
    return shader == 3 || shader == 5;
}

/// MOMT shader variants with metallic shading: shader 2 (Metal, e.g. Sunwell
/// bronze roofs/trims) and shader 5 (EnvMetal).
constexpr bool wmoShaderIsMetal(uint32_t shader) {
    return shader == 2 || shader == 5;
}

/// MOMT shader variants whose diffuse alpha channel masks specular highlights:
/// Specular (1), Metal (2), and EnvMetal (5).
constexpr bool wmoShaderSpecularAlphaMask(uint32_t shader) {
    return shader == 1 || shader == 2 || shader == 5;
}

/// MOMT material flags.
enum WmoMaterialFlags : uint32_t {
    WMO_MAT_UNLIT    = 0x01,
    WMO_MAT_UNFOGGED = 0x02,
    WMO_MAT_UNCULLED = 0x04,
    WMO_MAT_EXTLIGHT = 0x08,
    WMO_MAT_SIDN     = 0x10,  ///< night glow: windows and lamps
    WMO_MAT_WINDOW   = 0x20,  ///< marked a window by the artist
    WMO_MAT_CLAMP_S  = 0x40,  ///< clamp U texture coordinate to edge
    WMO_MAT_CLAMP_T  = 0x80,  ///< clamp V texture coordinate to edge
};

constexpr bool wmoMaterialClampS(uint32_t materialFlags) {
    return (materialFlags & WMO_MAT_CLAMP_S) != 0;
}

constexpr bool wmoMaterialClampT(uint32_t materialFlags) {
    return (materialFlags & WMO_MAT_CLAMP_T) != 0;
}

/// Night glow (non-TBC profiles only) follows the authored F_SIDN flag and
/// emits the material's sidnColor: MM_CLOCKFACE_01 in duskwood_townhall.wmo and
/// STORMWINDLAMPGLASS carry 0xd0 with white; the same clock face in
/// stormwind.wmo carries 0xc0 and does not glow.
inline bool wmoMaterialIsNightGlow(uint32_t materialFlags) {
    return (materialFlags & WMO_MAT_SIDN) != 0;
}

}  // namespace wowee::rendering
