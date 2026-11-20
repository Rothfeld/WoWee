#include <catch_amalgamated.hpp>

#include "game/update_field_table.hpp"

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
    REQUIRE_FALSE(table.hasField(UF::GAMEOBJECT_PARENTROTATION));
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
