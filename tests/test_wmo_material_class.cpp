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
