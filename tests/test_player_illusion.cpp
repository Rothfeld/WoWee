#include <catch_amalgamated.hpp>

#include "core/player_illusion.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/dbc_loader.hpp"

#include <filesystem>
#include <fstream>
#include <vector>

using wowee::core::HumanoidDisplayExtraInfo;
using wowee::core::PlayerHumanoidMorph;
using wowee::core::PlayerIllusionRaceLimits;
using wowee::core::packAppearanceBytes;
using wowee::core::resolvePlayerHumanoidMorph;
using wowee::core::wrapAppearanceIndex;

namespace {

constexpr PlayerIllusionRaceLimits kHumanMaleLimits{
    .skinCount = 10,
    .faceCount = 12,
    .hairStyleCount = 12,
    .hairColorCount = 10,
    .facialHairCount = 9,
};

std::string layoutPath(const std::string& expansion) {
    return (std::filesystem::path(WOWEE_SOURCE_DIR) /
            "Data" / "expansions" / expansion / "dbc_layouts.json").string();
}

} // namespace

TEST_CASE("CanEquip racial illusion maps player appearance and enables live equipment", "[illusion][morph]") {
    // Old Hillsbrad Human Illusion Male (displayId 19723 -> CDIE 13749):
    // race=1 (Human), sex=0 (Male), all appearance columns 0, CanEquip=1.
    const HumanoidDisplayExtraInfo humanIllusionMale{
        .raceId = 1,
        .sexId = 0,
        .skinId = 0,
        .faceId = 0,
        .hairStyleId = 0,
        .hairColorId = 0,
        .facialHairId = 0,
        .canEquip = true,
    };

    SECTION("Armless (skin=0 face=0 hair=0 hairColor=0 facial=2) preserves exact values") {
        const uint32_t playerBytes = packAppearanceBytes(0, 0, 0, 0);
        const PlayerHumanoidMorph m = resolvePlayerHumanoidMorph(
            humanIllusionMale, playerBytes, 2, kHumanMaleLimits);
        CHECK(m.isEquipablePlayerIllusion);
        CHECK(m.usePlayerAppearance);
        CHECK(m.targetRace == 1);
        CHECK(m.targetSex == 0);
        CHECK(m.appearanceBytes == playerBytes);
        CHECK(m.facialHair == 2);
    }

    SECTION("Legless (skin=0 face=0 hair=7 hairColor=6 facial=5) preserves blond long hair") {
        const uint32_t playerBytes = packAppearanceBytes(0, 0, 7, 6);
        const PlayerHumanoidMorph m = resolvePlayerHumanoidMorph(
            humanIllusionMale, playerBytes, 5, kHumanMaleLimits);
        CHECK(m.isEquipablePlayerIllusion);
        CHECK(m.usePlayerAppearance);
        CHECK(m.targetRace == 1);
        CHECK(m.targetSex == 0);
        CHECK(m.appearanceBytes == playerBytes);
        CHECK(m.facialHair == 5);
    }

    SECTION("Out-of-range player appearance indices wrap cleanly into target race limits") {
        // Undead Male has facialHair 0..16, Human Male has 0..8 (count 9).
        const uint32_t playerBytes = packAppearanceBytes(13, 15, 19, 12);
        const PlayerHumanoidMorph m = resolvePlayerHumanoidMorph(
            humanIllusionMale, playerBytes, 14, kHumanMaleLimits);
        CHECK(m.isEquipablePlayerIllusion);
        CHECK(m.usePlayerAppearance);
        CHECK(m.appearanceBytes == packAppearanceBytes(3, 3, 7, 2));
        CHECK(m.facialHair == 5);
    }

    SECTION("Zero target limits never divide by zero and map to 0") {
        const PlayerIllusionRaceLimits emptyLimits{};
        const uint32_t playerBytes = packAppearanceBytes(4, 5, 6, 7);
        const PlayerHumanoidMorph m = resolvePlayerHumanoidMorph(
            humanIllusionMale, playerBytes, 8, emptyLimits);
        CHECK(m.isEquipablePlayerIllusion);
        CHECK(m.usePlayerAppearance);
        CHECK(m.appearanceBytes == 0u);
        CHECK(m.facialHair == 0u);
    }
}

TEST_CASE("CanEquip illusion with preset appearance keeps preset look while equipping live gear", "[illusion][morph]") {
    // Blood Elf Illusion (displayId 20368 -> CDIE 14026): CanEquip=1 with preset face/hair.
    const HumanoidDisplayExtraInfo bloodElfPreset{
        .raceId = 10,
        .sexId = 0,
        .skinId = 9,
        .faceId = 7,
        .hairStyleId = 10,
        .hairColorId = 2,
        .facialHairId = 4,
        .canEquip = true,
    };

    const uint32_t playerBytes = packAppearanceBytes(1, 2, 3, 4);
    const PlayerHumanoidMorph m = resolvePlayerHumanoidMorph(
        bloodElfPreset, playerBytes, 1, kHumanMaleLimits);
    CHECK(m.isEquipablePlayerIllusion);
    CHECK_FALSE(m.usePlayerAppearance);
    CHECK(m.targetRace == 10);
    CHECK(m.targetSex == 0);
    CHECK(m.appearanceBytes == packAppearanceBytes(9, 7, 10, 2));
    CHECK(m.facialHair == 4);
}

TEST_CASE("Non-CanEquip humanoid NPC morph uses NPC appearance and does not equip player gear", "[illusion][morph]") {
    const HumanoidDisplayExtraInfo npcMorph{
        .raceId = 1,
        .sexId = 0,
        .skinId = 1,
        .faceId = 2,
        .hairStyleId = 3,
        .hairColorId = 4,
        .facialHairId = 5,
        .canEquip = false,
    };

    const uint32_t playerBytes = packAppearanceBytes(0, 0, 7, 6);
    const PlayerHumanoidMorph m = resolvePlayerHumanoidMorph(
        npcMorph, playerBytes, 2, kHumanMaleLimits);
    CHECK_FALSE(m.isEquipablePlayerIllusion);
    CHECK_FALSE(m.usePlayerAppearance);
    CHECK(m.targetRace == 1);
    CHECK(m.targetSex == 0);
    CHECK(m.appearanceBytes == packAppearanceBytes(1, 2, 3, 4));
    CHECK(m.facialHair == 5);
}

TEST_CASE("DBC layouts expose CreatureDisplayInfoExtra.CanEquip on 21-field expansions", "[illusion][dbc]") {
    for (const char* exp : {"classic", "tbc", "wotlk"}) {
        INFO("expansion: " << exp);
        wowee::pipeline::DBCLayout layout;
        REQUIRE(layout.loadFromJson(layoutPath(exp)));
        const auto* cdie = layout.getLayout("CreatureDisplayInfoExtra");
        REQUIRE(cdie != nullptr);
        CHECK(cdie->field("CanEquip") == 19u);
        CHECK(cdie->field("BakeName") == 20u);
    }
}

TEST_CASE("TBC 2.4.3 CreatureDisplayInfoExtra.dbc marks Old Hillsbrad Human Illusion as CanEquip", "[illusion][dbc]") {
    const std::filesystem::path dbcPath = "/workspace/azerothcore/server/data/dbc/CreatureDisplayInfoExtra.dbc";
    if (!std::filesystem::exists(dbcPath)) {
        SKIP("TBC server DBC directory not present");
    }

    std::ifstream in(dbcPath, std::ios::binary);
    REQUIRE(in.good());
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    wowee::pipeline::DBCFile dbc;
    REQUIRE(dbc.load(bytes));
    REQUIRE(dbc.getFieldCount() == 21u);

    // CDIE 13749 (display 19723 Human Male Illusion) and 13750 (display 19724 Human Female Illusion)
    const int32_t maleIdx = dbc.findRecordById(13749);
    const int32_t femaleIdx = dbc.findRecordById(13750);
    REQUIRE(maleIdx >= 0);
    REQUIRE(femaleIdx >= 0);
    CHECK(dbc.getUInt32(static_cast<uint32_t>(maleIdx), 1) == 1u);   // Human
    CHECK(dbc.getUInt32(static_cast<uint32_t>(maleIdx), 2) == 0u);   // Male
    CHECK((dbc.getUInt32(static_cast<uint32_t>(maleIdx), 19) & 0x1u) == 1u);
    CHECK(dbc.getUInt32(static_cast<uint32_t>(femaleIdx), 1) == 1u); // Human
    CHECK(dbc.getUInt32(static_cast<uint32_t>(femaleIdx), 2) == 1u); // Female
    CHECK((dbc.getUInt32(static_cast<uint32_t>(femaleIdx), 19) & 0x1u) == 1u);
}
