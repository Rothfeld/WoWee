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

/// MOMT material flags.
enum WmoMaterialFlags : uint32_t {
    WMO_MAT_UNLIT    = 0x01,
    WMO_MAT_UNFOGGED = 0x02,
    WMO_MAT_UNCULLED = 0x04,
    WMO_MAT_EXTLIGHT = 0x08,
    WMO_MAT_SIDN     = 0x10,  ///< night glow: windows and lamps
    WMO_MAT_WINDOW   = 0x20,  ///< marked a window by the artist
};

/// Night glow (non-TBC profiles only) follows the authored F_SIDN flag and
/// emits the material's sidnColor: MM_CLOCKFACE_01 in duskwood_townhall.wmo and
/// STORMWINDLAMPGLASS carry 0xd0 with white; the same clock face in
/// stormwind.wmo carries 0xc0 and does not glow.
inline bool wmoMaterialIsNightGlow(uint32_t materialFlags) {
    return (materialFlags & WMO_MAT_SIDN) != 0;
}

}  // namespace wowee::rendering
