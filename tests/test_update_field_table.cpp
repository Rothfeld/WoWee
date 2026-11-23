#include <catch_amalgamated.hpp>

#include "game/update_field_table.hpp"
#include "game/update_field_helpers.hpp"

#include <filesystem>
#include <string>
#include <vector>

// The client reads update-field values by a logical name, resolved through a
// per-expansion offset file. If a name in the JSON stops matching the enum, the
// loader silently skips it and fieldIndex returns 0xFFFF forever after - the
// field reads as absent and its stat shows zero, with nothing to fail. These
// are the offsets the character sheet depends on; pin the ones a wrong value
// would silently zero. The mana-regen pair is derived as OBJECT_END(6)+0x22 and
// +0x29, cross-checked against MAXPOWER1 = OBJECT_END+0x1B = 33.

using wowee::game::UpdateFieldTable;
using wowee::game::UF;

namespace {
std::string fieldsPath(const std::string& expansion) {
    return (std::filesystem::path(WOWEE_SOURCE_DIR) /
            "Data" / "expansions" / expansion / "update_fields.json").string();
}
} // namespace

TEST_CASE("Every expansion ships a loadable update-field table", "[updatefields]") {
    for (const auto& expansion : {"classic", "tbc", "wotlk", "turtle"}) {
        INFO("expansion: " << expansion);
        UpdateFieldTable table;
        REQUIRE(table.loadFromJson(fieldsPath(expansion)));
    }
}

TEST_CASE("WotLK mana-regen fields resolve to their wire offsets", "[updatefields]") {
    UpdateFieldTable table;
    REQUIRE(table.loadFromJson(fieldsPath("wotlk")));

    // Anchors: if these drift the file changed shape and the derivation below
    // no longer holds.
    REQUIRE(table.index(UF::UNIT_FIELD_MAXPOWER1) == 33);          // OBJECT_END + 0x1B
    REQUIRE(table.index(UF::UNIT_FIELD_ATTACK_POWER) == 123);

    // The pair GetManaRegen reads. Mana is power index 0, so each sits at the
    // base of its seven-wide array.
    REQUIRE(table.index(UF::UNIT_FIELD_POWER_REGEN_FLAT_MODIFIER) == 40);              // + 0x22
    REQUIRE(table.index(UF::UNIT_FIELD_POWER_REGEN_INTERRUPTED_FLAT_MODIFIER) == 47);  // + 0x29
}

TEST_CASE("TBC 8606 character fields match the server wire layout", "[updatefields]") {
    UpdateFieldTable table;
    REQUIRE(table.loadFromJson(fieldsPath("tbc")));
    // CMaNGOS TBC UpdateFields.h, generated for client build 8606.
    REQUIRE(table.index(UF::UNIT_VIRTUAL_ITEM_SLOT_ID) == 6 + 0x1F);
    REQUIRE(table.index(UF::UNIT_VIRTUAL_ITEM_INFO) == 6 + 0x22);
    REQUIRE(table.index(UF::UNIT_FIELD_BYTES_1) == 6 + 0x99);
    REQUIRE(table.index(UF::UNIT_FIELD_STAT0) == 6 + 0xA5);
    REQUIRE(table.index(UF::UNIT_FIELD_STAT4) == 6 + 0xA9);
    REQUIRE(table.index(UF::UNIT_FIELD_RESISTANCES) == 6 + 0xB4);
    REQUIRE(table.index(UF::UNIT_FIELD_AURAS) == 6 + 0x2A);
    REQUIRE(table.index(UF::UNIT_FIELD_AURAFLAGS) == 6 + 0x62);
    REQUIRE(table.index(UF::PLAYER_BYTES) == 234 + 5);
    REQUIRE(table.index(UF::PLAYER_FIELD_COINAGE) == 234 + 0x4CB);
    REQUIRE(table.index(UF::PLAYER_FIELD_BYTES) == 234 + 0x4E4);
    REQUIRE(table.index(UF::PLAYER_AMMO_ID) == 234 + 0x4E5);
    REQUIRE(table.index(UF::PLAYER_SELF_RES_SPELL) == 234 + 0x4E6);
    REQUIRE(table.index(UF::PLAYER_FIELD_BYTES2) == 234 + 0x504);
    REQUIRE(table.index(UF::PLAYER_FIELD_KEYRING_SLOT_1) == 822);
    REQUIRE(table.index(UF::PLAYER_CRIT_PERCENTAGE) == 1321);
    // TBC publishes separate game-object state/type fields, not WotLK's bytes.
    REQUIRE_FALSE(table.hasField(UF::GAMEOBJECT_BYTES_1));

    // DynamicObject layout (CMaNGOS TBC UpdateFields.h: OBJECT_END = 6):
    // CASTER = 6..7, BYTES = 8, SPELLID = 9, RADIUS = 10, POS_X/Y/Z = 11..13.
    REQUIRE(table.index(UF::DYNAMICOBJECT_BYTES) == 8);
    REQUIRE(table.index(UF::DYNAMICOBJECT_SPELLID) == 9);
    REQUIRE(table.index(UF::DYNAMICOBJECT_RADIUS) == 10);
    REQUIRE(table.index(UF::DYNAMICOBJECT_POS_X) == 11);
    REQUIRE(table.index(UF::DYNAMICOBJECT_POS_Y) == 12);
    REQUIRE(table.index(UF::DYNAMICOBJECT_POS_Z) == 13);

    // GameObject rotation & facing fields (CMaNGOS TBC UpdateFields.h: OBJECT_END = 6).
    REQUIRE(table.index(UF::GAMEOBJECT_ROTATION) == 10);
    REQUIRE(table.index(UF::GAMEOBJECT_FACING) == 18);
    REQUIRE(table.index(UF::GAMEOBJECT_ANIMPROGRESS) == 24);
    REQUIRE_FALSE(table.hasField(UF::GAMEOBJECT_PARENTROTATION));

    // H10 TBC update fields (CMaNGOS TBC UpdateFields.h: OBJECT_END = 6, UNIT_END = 234).
    REQUIRE(table.index(UF::UNIT_MOD_CAST_SPEED) == 6 + 0xA0);
    REQUIRE(table.index(UF::UNIT_FIELD_POWER_COST_MODIFIER) == 6 + 0xD4);
    REQUIRE(table.index(UF::UNIT_FIELD_POWER_COST_MULTIPLIER) == 6 + 0xDB);
    REQUIRE(table.index(UF::PLAYER_BYTES_3) == 234 + 0x07);
    REQUIRE(table.index(UF::PLAYER__FIELD_KNOWN_TITLES) == 234 + 0x2B2);
}

TEST_CASE("H10 update field helpers match TBC server semantics", "[updatefields][h10]") {
    using namespace wowee::game;

    // 1. UNIT_MOD_CAST_SPEED
    CHECK(applyUnitModCastSpeed(2000, 1.0f) == 2000u);
    CHECK(applyUnitModCastSpeed(2000, 0.76923077f) == 1538u);  // Bloodlust (+30% haste)
    CHECK(applyUnitModCastSpeed(2500, 1.6f) == 4000u);         // Curse of Tongues (+60% cast time)
    CHECK(applyUnitModCastSpeed(0, 1.6f) == 0u);               // Instant stays instant
    CHECK(applyUnitModCastSpeed(1500, 0.0f) == 1500u);         // Unset/zero field ignored
    CHECK(applyUnitModCastSpeed(1500, -1.0f) == 1500u);        // Invalid negative ignored

    // 2. UNIT_FIELD_POWER_COST_MODIFIER & UNIT_FIELD_POWER_COST_MULTIPLIER
    CHECK(firstSchoolIndexFromMask(0x00u) == 0u);
    CHECK(firstSchoolIndexFromMask(0x01u) == 0u);  // Physical
    CHECK(firstSchoolIndexFromMask(0x02u) == 1u);  // Holy
    CHECK(firstSchoolIndexFromMask(0x04u) == 2u);  // Fire
    CHECK(firstSchoolIndexFromMask(0x08u) == 3u);  // Nature
    CHECK(firstSchoolIndexFromMask(0x10u) == 4u);  // Frost
    CHECK(firstSchoolIndexFromMask(0x20u) == 5u);  // Shadow
    CHECK(firstSchoolIndexFromMask(0x40u) == 6u);  // Arcane
    CHECK(firstSchoolIndexFromMask(0x14u) == 2u);  // Frostfire -> lowest bit Fire (2)

    CHECK(applyUnitPowerCostModifiers(500, 0, 0, 0, 0.0f) == 500u);
    CHECK(applyUnitPowerCostModifiers(500, -50, 0, 0, 0.0f) == 450u);
    CHECK(applyUnitPowerCostModifiers(500, 0, 0, 0, -1.0f) == 0u);        // Clearcasting / Inner Focus
    CHECK(applyUnitPowerCostModifiers(500, -100, 0, -10, -0.25f) == 270u); // (500-100)*0.9=360, *0.75=270
    CHECK(applyUnitPowerCostModifiers(0, 50, 0, 0, 0.5f) == 0u);          // Free spells stay free
    CHECK(applyUnitPowerCostModifiers(100, -200, 0, 0, 0.0f) == 0u);      // Clamped at 0

    // 3. PLAYER_BYTES_3
    const auto sober = decodePlayerBytes3(0x01000001u);  // female, sober, arenaFaction=1
    CHECK(sober.gender == 1u);
    CHECK(sober.drunkValue == 0u);
    CHECK(sober.drunkState == 0u);
    CHECK(sober.intoxication == Catch::Approx(0.0f));
    CHECK(sober.arenaFaction == 1u);

    const auto tipsy = decodePlayerBytes3(0x00000100u);
    CHECK(tipsy.gender == 0u);
    CHECK(tipsy.drunkValue == 256u);
    CHECK(tipsy.drunkState == 1u);
    CHECK(tipsy.intoxication == Catch::Approx(0.15f));

    const auto drunk = decodePlayerBytes3(12800u | 1u);
    CHECK(drunk.gender == 1u);
    CHECK(drunk.drunkValue == 12800u);
    CHECK(drunk.drunkState == 2u);
    CHECK(drunk.intoxication == Catch::Approx(12800.0f / 32768.0f));

    const auto smashed = decodePlayerBytes3(32768u);
    CHECK(smashed.drunkValue == 32768u);
    CHECK(smashed.drunkState == 3u);
    CHECK(smashed.intoxication == Catch::Approx(1.0f));

    // 4. PLAYER__FIELD_KNOWN_TITLES
    std::unordered_set<uint32_t> titles;
    CHECK(syncKnownTitleBitsWord(titles, 0, (1u << 1u) | (1u << 5u)));
    CHECK(titles.count(1u) == 1u);
    CHECK(titles.count(5u) == 1u);
    CHECK_FALSE(syncKnownTitleBitsWord(titles, 0, (1u << 1u) | (1u << 5u)));  // idempotent
    CHECK(syncKnownTitleBitsWord(titles, 1, (1u << 8u)));                     // bit 40 (Vengeful Gladiator)
    CHECK(titles.count(40u) == 1u);
    CHECK(syncKnownTitleBitsWord(titles, 0, (1u << 1u)));                     // bit 5 removed, bit 40 kept
    CHECK(titles.count(1u) == 1u);
    CHECK(titles.count(5u) == 0u);
    CHECK(titles.count(40u) == 1u);

    // 5. GAMEOBJECT_ANIMPROGRESS
    CHECK(normalizeGameObjectAnimProgress(0u) == Catch::Approx(0.0f));
    CHECK(normalizeGameObjectAnimProgress(128u) == Catch::Approx(128.0f / 255.0f));
    CHECK(normalizeGameObjectAnimProgress(255u) == Catch::Approx(1.0f));
    CHECK(normalizeGameObjectAnimProgress(300u) == Catch::Approx(1.0f));
}

TEST_CASE("GameObject rotation update fields resolve per expansion", "[updatefields]") {
    for (const auto& expansion : {"classic", "turtle", "tbc"}) {
        UpdateFieldTable table;
        REQUIRE(table.loadFromJson(fieldsPath(expansion)));
        REQUIRE(table.index(UF::GAMEOBJECT_ROTATION) == 10);
        REQUIRE(table.index(UF::GAMEOBJECT_FACING) == 18);
    }
    UpdateFieldTable wotlk;
    REQUIRE(wotlk.loadFromJson(fieldsPath("wotlk")));
    REQUIRE(wotlk.index(UF::GAMEOBJECT_PARENTROTATION) == 10);
    REQUIRE_FALSE(wotlk.hasField(UF::GAMEOBJECT_ROTATION));
}
