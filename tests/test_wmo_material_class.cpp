// WMO material classes come from MOMT flags and shader ids.
#include <catch_amalgamated.hpp>
#include "pipeline/wmo_loader.hpp"

TEST_CASE("Stormwind exterior-lighting override is independent of indoor visibility", "[wmo-material]") {
    using wowee::pipeline::wmoUsesLocalLighting;
    CHECK_FALSE(wmoUsesLocalLighting(0x2841)); // Stormwind street group
    CHECK_FALSE(wmoUsesLocalLighting(0x2a41)); // Stormwind thief01
    CHECK_FALSE(wmoUsesLocalLighting(0xb841)); // Stormwind canal505
    CHECK(wmoUsesLocalLighting(0x2a05));       // Stormwind AuctionHouse
    CHECK(wmoUsesLocalLighting(0x2805));       // Ironforge interior
    CHECK_FALSE(wmoUsesLocalLighting(0x8809)); // Outdoor group
}

TEST_CASE("A WMO doodad listed by an outdoor-lit group takes the outdoor light", "[wmo-material]") {
    using wowee::pipeline::WMOModel;
    using wowee::pipeline::wmoDoodadUsesLocalLighting;
    // bootybay.wmo: MODD 3 (BootyBayEntrance_02) is listed by the tunnel,
    // group 22 (0xb805), and by outdoor group 42 (0x8849); MODD 943 (Sack01)
    // only by group 32 (0x2805).
    WMOModel model;
    model.groups.resize(3);
    model.groups[0].flags = 0xb805;
    model.groups[0].doodadRefs = {3, 7};
    model.groups[1].flags = 0x8849;
    model.groups[1].doodadRefs = {3};
    model.groups[2].flags = 0x2805;
    model.groups[2].doodadRefs = {943};
    CHECK_FALSE(wmoDoodadUsesLocalLighting(model, 3));
    CHECK(wmoDoodadUsesLocalLighting(model, 7));
    CHECK(wmoDoodadUsesLocalLighting(model, 943));
    CHECK_FALSE(wmoDoodadUsesLocalLighting(model, 5)); // listed by no group
    // A Stormwind-style group (interior, 0x40 exterior-lit) counts as outdoor-lit.
    model.groups[2].flags = 0x2a41;
    CHECK_FALSE(wmoDoodadUsesLocalLighting(model, 943));
}

#include "rendering/wmo_material_class.hpp"

TEST_CASE("WMO diffuse materials remain matte", "[wmo-material]") {
    using wowee::rendering::wmoShaderUsesSpecular;
    for (uint32_t shader : {0u, 4u, 6u, 8u, 9u, 13u, 14u, 16u}) {
        CHECK_FALSE(wmoShaderUsesSpecular(shader));
    }
    for (uint32_t shader : {1u, 2u, 5u, 7u, 11u, 12u}) {
        CHECK(wmoShaderUsesSpecular(shader));
    }
}

using wowee::rendering::wmoMaterialIsNightGlow;
using wowee::rendering::WMO_MAT_SIDN;

TEST_CASE("night glow follows MOMT F_SIDN, not texture names", "[wmo-material]") {
    // townhall.wmo MM_CLOCKFACE_01: 0xd0; stormwind.wmo MM_CLOCKFACE_01: 0xc0.
    CHECK(wmoMaterialIsNightGlow(0xd0));
    CHECK_FALSE(wmoMaterialIsNightGlow(0xc0));
    CHECK(wmoMaterialIsNightGlow(WMO_MAT_SIDN));
}

TEST_CASE("WMO MOMT environment-mapped and metal shaders are classified accurately", "[wmo-material]") {
    using wowee::rendering::wmoShaderUsesEnvMap;
    using wowee::rendering::wmoShaderIsMetal;
    using wowee::rendering::wmoShaderSpecularAlphaMask;

    // Shader 3 (Env: Karazhan ballroom floor, Stratholme ice) and 5 (EnvMetal: Coilfang, Sunwell floor)
    CHECK(wmoShaderUsesEnvMap(3));
    CHECK(wmoShaderUsesEnvMap(5));
    for (uint32_t s : {0u, 1u, 2u, 4u, 6u, 7u}) {
        CHECK_FALSE(wmoShaderUsesEnvMap(s));
    }

    // Shader 2 (Metal: Sunwell bronze roofs/trims) and 5 (EnvMetal)
    CHECK(wmoShaderIsMetal(2));
    CHECK(wmoShaderIsMetal(5));
    for (uint32_t s : {0u, 1u, 3u, 4u, 6u}) {
        CHECK_FALSE(wmoShaderIsMetal(s));
    }

    // Specular alpha mask: Specular (1), Metal (2), EnvMetal (5)
    CHECK(wmoShaderSpecularAlphaMask(1));
    CHECK(wmoShaderSpecularAlphaMask(2));
    CHECK(wmoShaderSpecularAlphaMask(5));
    CHECK_FALSE(wmoShaderSpecularAlphaMask(0));
    CHECK_FALSE(wmoShaderSpecularAlphaMask(3));
}

TEST_CASE("WMO MOMT clamp flags 0x40 and 0x80 control S and T wrap modes independently", "[wmo-material]") {
    using wowee::rendering::wmoMaterialClampS;
    using wowee::rendering::wmoMaterialClampT;
    using wowee::rendering::WMO_MAT_CLAMP_S;
    using wowee::rendering::WMO_MAT_CLAMP_T;

    CHECK_FALSE(wmoMaterialClampS(0x00));
    CHECK_FALSE(wmoMaterialClampT(0x00));
    CHECK(wmoMaterialClampS(WMO_MAT_CLAMP_S));
    CHECK_FALSE(wmoMaterialClampT(WMO_MAT_CLAMP_S));
    CHECK_FALSE(wmoMaterialClampS(WMO_MAT_CLAMP_T));
    CHECK(wmoMaterialClampT(WMO_MAT_CLAMP_T));
    // duskwood_townhall.wmo MM_CLOCKFACE_01 carries 0xd0 (F_SIDN | F_CLAMP_S | F_CLAMP_T)
    CHECK(wmoMaterialClampS(0xd0));
    CHECK(wmoMaterialClampT(0xd0));
}

TEST_CASE("WMO MOMT shader 5 EnvMetal modulates reflection by lit base color", "[wmo-material]") {
    using wowee::rendering::wmoEnvMetalChannel;

    // Dark interior (Steamvault EBCFMARSH_CLINDER.BLP + KKG_CFANG_ENVMAPTEST01.BLP):
    // diffuse = 0.51, interior lighting = 0.30 -> litBase = 0.153, env = 0.96, alpha = 1.0.
    // Modulated by litBase gives ~0.30 instead of saturating to >1.0 unlit neon cyan.
    const float darkOval = wmoEnvMetalChannel(0.51f * 0.30f, 0.96f, 1.0f);
    CHECK(darkOval == Catch::Approx(0.153f + 0.96f * 0.153f));
    CHECK(darkOval < 0.35f);

    // Zero alpha masks out environment reflection completely.
    CHECK(wmoEnvMetalChannel(0.25f, 1.0f, 0.0f) == Catch::Approx(0.25f));

    // Bright interior (Exodar crystal wall): lighting ~0.85, alpha ~0.24 stays proportional.
    const float brightCrystal = wmoEnvMetalChannel(0.60f * 0.85f, 0.50f, 0.24f);
    CHECK(brightCrystal == Catch::Approx(0.51f + 0.50f * 0.51f * 0.24f));
}
