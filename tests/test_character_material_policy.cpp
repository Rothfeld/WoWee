#include <algorithm>
#include <catch_amalgamated.hpp>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>
#include "rendering/character_material_policy.hpp"

using wowee::rendering::characterBatchDrawsBefore;
using wowee::rendering::characterBatchIsHair;
using wowee::rendering::characterBatchNeedsCutout;
using wowee::rendering::characterMaterialDomain;
using wowee::rendering::CharacterMaterialDomain;

constexpr auto character = CharacterMaterialDomain::Character;
constexpr auto creature = CharacterMaterialDomain::Creature;
constexpr auto scene = CharacterMaterialDomain::Scene;
constexpr auto attachment = CharacterMaterialDomain::Attachment;

TEST_CASE("attached item materials are distinct from characters and enchant effects", "[character][m2]") {
    CHECK(characterMaterialDomain(false, false, false, false) == character);
    CHECK(characterMaterialDomain(false, true, false, false) == creature);
    CHECK(characterMaterialDomain(true, true, true, false) == scene);
    CHECK(characterMaterialDomain(false, false, true, false) == attachment);
    CHECK(characterMaterialDomain(false, true, true, false) == attachment);
    // Existing enchant handling remains unchanged; it is not an item surface.
    CHECK(characterMaterialDomain(false, false, true, true) == character);
}

TEST_CASE("opaque creature and vehicle surfaces ignore an atlas alpha mask", "[character][m2]") {
    // SiegeVehicle's two bodywork batches both say opaque, but both atlases
    // contain low-alpha texels inside the UVs those batches actually sample.
    CHECK_FALSE(characterBatchIsHair(creature, false, 0, 0, 1));
    CHECK_FALSE(characterBatchNeedsCutout(creature, 0, true, false));
    CHECK_FALSE(characterBatchNeedsCutout(creature, 0, false, false));
    // The existing character and scene paths are not silently changed.
    CHECK(characterBatchNeedsCutout(character, 0, true, false));
    CHECK_FALSE(characterBatchNeedsCutout(scene, 0, true, false));
}

TEST_CASE("creature alpha-key and real hair remain cut out", "[character][m2]") {
    CHECK(characterBatchNeedsCutout(creature, 1, true, false));
    CHECK(characterBatchNeedsCutout(creature, 1, false, false));
    CHECK(characterBatchIsHair(creature, true, 201, 0, 1));
    CHECK(characterBatchNeedsCutout(creature, 0, true, true));
    CHECK_FALSE(characterBatchIsHair(scene, true, 201, 0, 1));
}

TEST_CASE("creature geosets are not mistaken for character hair", "[character][m2]") {
    CHECK_FALSE(characterBatchIsHair(creature, false, 201, 1, 1));
    CHECK_FALSE(characterBatchIsHair(creature, false, 42, 0, 2));
    CHECK(characterBatchIsHair(character, false, 201, 1, 1));
    CHECK(characterBatchIsHair(character, false, 42, 0, 2));
    CHECK_FALSE(characterBatchIsHair(character, false, 0, 0, 1));
}

TEST_CASE("creature blended and additive batches are not forced to cut out", "[character][m2]") {
    for (uint16_t mode : {uint16_t{2}, uint16_t{3}, uint16_t{4}, uint16_t{5}, uint16_t{6}}) {
        CHECK_FALSE(characterBatchNeedsCutout(creature, mode, false, false));
        CHECK_FALSE(characterBatchNeedsCutout(creature, mode, true, false));
    }
    // Preserve the existing character fallback and scene semantics.
    CHECK(characterBatchNeedsCutout(character, 2, false, false));
    CHECK_FALSE(characterBatchNeedsCutout(scene, 2, false, false));
}

TEST_CASE("attached equipment uses authored material alpha and not character geosets", "[character][m2]") {
    // The Bloodsurge blade is opaque despite an atlas alpha mask. Alpha-key
    // attachments must still cut out, and blended items must not be forced to.
    for (bool hasAlpha : {false, true}) {
        CHECK_FALSE(characterBatchNeedsCutout(attachment, 0, hasAlpha, false));
        CHECK(characterBatchNeedsCutout(attachment, 1, hasAlpha, false));
        for (uint16_t mode = 2; mode <= 7; ++mode)
            CHECK_FALSE(characterBatchNeedsCutout(attachment, mode, hasAlpha, false));
    }
    CHECK_FALSE(characterBatchIsHair(attachment, false, 201, 1, 1));
    CHECK_FALSE(characterBatchIsHair(attachment, false, 42, 0, 2));
    CHECK_FALSE(characterBatchIsHair(attachment, true, 201, 0, 1));
}

TEST_CASE("character batch ordering scopes materialLayer within each submesh", "[character][m2]") {
    // Glave_1H_DualBlade_D_02.m2 (Warglaive of Azzinoth):
    //   batch 0: prio=0, submesh=0, layer=0 (Opaque blade)
    //   batch 1: prio=0, submesh=0, layer=1 (Mod2x ArmorReflect4)
    //   batch 2: prio=0, submesh=0, layer=2 (Alpha hilt mask)
    //   batch 3: prio=0, submesh=1, layer=0 (AddAlpha scrolling green glow shell)
    // Submesh 1's layer-0 glow shell must draw AFTER submesh 0's layer-1/2 passes
    // so the blade's Mod2x reflection and Alpha mask do not wipe out the glow.
    struct BatchKey { int8_t prio; uint16_t submesh; uint16_t layer; };
    const std::vector<BatchKey> batches = {
        {0, 0, 0},
        {0, 0, 1},
        {0, 0, 2},
        {0, 1, 0},
    };
    std::vector<size_t> order(batches.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return characterBatchDrawsBefore(
            batches[a].prio, batches[a].submesh, batches[a].layer,
            batches[b].prio, batches[b].submesh, batches[b].layer);
    });
    CHECK(order == std::vector<size_t>{0, 1, 2, 3});

    // Within one submesh, materialLayer still orders passes 0 -> 1 -> 2.
    CHECK(characterBatchDrawsBefore(0, 0, 0, 0, 0, 1));
    CHECK(characterBatchDrawsBefore(0, 0, 1, 0, 0, 2));
    CHECK_FALSE(characterBatchDrawsBefore(0, 0, 2, 0, 0, 1));

    // priorityPlane still takes top precedence across submeshes.
    CHECK(characterBatchDrawsBefore(-1, 3, 2, 0, 0, 0));
    CHECK(characterBatchDrawsBefore(0, 1, 0, 1, 0, 0));
}

namespace {
std::string readRepoText(const char* relPath) {
    namespace fs = std::filesystem;
    fs::path dir = fs::path(__FILE__).parent_path().parent_path();
    std::ifstream in(dir / relPath);
    REQUIRE(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
} // namespace

TEST_CASE("character shaders preserve zero-normal diffuse attenuation instead of synthetic up normal", "[character][m2]") {
    // Models like TitanFemale.m2 (82/950 zero-normal skirt verts, 106/660 TBC
    // creature M2s) rely on D3D9/TBC Gouraud N.L = 0 on zero-normal vertices.
    // Snapping zero normals to (0, 0, 1) creates bright polygonal triangles
    // and sharp seams across the skirt.
    const std::string vert = readRepoText("assets/shaders/character.vert.glsl");
    const std::string frag = readRepoText("assets/shaders/character.frag.glsl");
    CHECK(vert.find("out float NormalWeight") != std::string::npos);
    CHECK(vert.find("NormalWeight = clamp(length(aNormal), 0.0, 1.0);") != std::string::npos);
    CHECK(frag.find("in float NormalWeight") != std::string::npos);
    CHECK(frag.find("vec3 lightNorm = (dot(Normal, Normal) > 1e-8) ? (norm * clamp(NormalWeight, 0.0, 1.0)) : vec3(0.0);") != std::string::npos);
}

TEST_CASE("character pipelines default to LESS_OR_EQUAL depth compare matching D3D9", "[character][m2]") {
    const std::string src = readRepoText("src/rendering/character_renderer.cpp");
    CHECK(src.find("VkCompareOp depthCompare = VK_COMPARE_OP_LESS_OR_EQUAL") != std::string::npos);
    CHECK(src.find("VkCompareOp depthCompare = VK_COMPARE_OP_LESS,") == std::string::npos);
    CHECK(src.find("VkCompareOp depthCompare = VK_COMPARE_OP_LESS)") == std::string::npos);
}

TEST_CASE("weapon texture replacement preserves hardcoded type-0 textures and fills replaceable slots", "[character][m2]") {
    struct FakeTex {
        uint32_t type;
        std::string filename;
    };
    using wowee::rendering::collectReplaceableItemTextureSlots;

    // Exarch Maladaar's Sword_1H_Stratholme_D_03.m2: both slots are hardcoded
    // type-0 filenames (1Hswd_03.blp, WeaponSpec_tan.blp); must NOT be replaced
    // by ItemDisplayInfo's leftover Sword_1H_Blackwing_A_02 texture.
    const std::vector<FakeTex> maladaarSword{
        {0u, "Item\\ObjectComponents\\Weapon\\1Hswd_03.blp"},
        {0u, "Item\\ObjectComponents\\Weapon\\WeaponSpec_tan.blp"},
    };
    CHECK(collectReplaceableItemTextureSlots(maladaarSword).empty());

    // Standard replaceable weapon (e.g. Sword_1H_Blackwing_A_02.m2): type-2 at slot 0.
    const std::vector<FakeTex> replaceableAt0{
        {2u, ""},
        {0u, "ITEM\\OBJECTCOMPONENTS\\WEAPON\\ARMORREFLECT4.BLP"},
    };
    CHECK(collectReplaceableItemTextureSlots(replaceableAt0) == std::vector<uint32_t>{0u});

    // Multi-material weapon with hardcoded hilt at slot 0 and replaceable blade/handle at 1/2.
    const std::vector<FakeTex> multiSlot{
        {0u, "Item\\ObjectComponents\\Weapon\\Hilt.blp"},
        {3u, ""},
        {4u, ""},
    };
    CHECK(collectReplaceableItemTextureSlots(multiSlot) == std::vector<uint32_t>{1u, 2u});

    // Legacy single-slot unnamed model (e.g. helm_mail_c_01_bem.m2 type-1 empty filename).
    const std::vector<FakeTex> legacyUnnamed{{1u, ""}};
    CHECK(collectReplaceableItemTextureSlots(legacyUnnamed) == std::vector<uint32_t>{0u});
}
