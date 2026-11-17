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
