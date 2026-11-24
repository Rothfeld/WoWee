// Unit death / corpse aura & SpellVisualKit CharProc 13 kitLight cleanup tests.
#include <catch_amalgamated.hpp>

#include "game/entity.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/dbc_loader.hpp"
#include "rendering/corpse_aura_policy.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <vector>

using wowee::rendering::auraVisualPersistsOnCorpse;
using wowee::rendering::isCorpseAuraSuppressed;
using wowee::rendering::KitLightState;
using wowee::rendering::kSpellAttrEx3DeathPersistent;
using wowee::rendering::resetKitLightOnDeath;
using wowee::rendering::tickKitLight;

TEST_CASE("corpse aura persistence respects SPELL_ATTR_EX3_DEATH_PERSISTENT", "[corpse-aura]") {
    CHECK(kSpellAttrEx3DeathPersistent == 0x00100000u);
    CHECK(auraVisualPersistsOnCorpse(kSpellAttrEx3DeathPersistent));
    CHECK(auraVisualPersistsOnCorpse(0x04100000u));
    CHECK_FALSE(auraVisualPersistsOnCorpse(0x00000000u));
    CHECK_FALSE(auraVisualPersistsOnCorpse(0x04000000u)); // Banish 44836 attrEx3
}

TEST_CASE("corpse aura suppression distinguishes corpses from living and feigning units", "[corpse-aura]") {
    constexpr uint32_t kDead = wowee::game::UNIT_DYNFLAG_DEAD;
    constexpr uint32_t kLoot = wowee::game::UNIT_DYNFLAG_LOOTABLE;

    // Living unit: never suppressed.
    CHECK_FALSE(isCorpseAuraSuppressed(false, 1000, 1000, 0, kDead, kLoot));
    // Feign Death: health > 0 with UNIT_DYNFLAG_DEAD is NOT a corpse.
    CHECK_FALSE(isCorpseAuraSuppressed(false, 500, 1000, kDead, kDead, kLoot));
    // Dead via health == 0 (with maxHealth > 0).
    CHECK(isCorpseAuraSuppressed(false, 0, 1000, 0, kDead, kLoot));
    // Dead via UNIT_DYNFLAG_LOOTABLE (even if CREATE omitted health).
    CHECK(isCorpseAuraSuppressed(false, 0, 0, kLoot, kDead, kLoot));
    // Dead via UNIT_DYNFLAG_DEAD with health == 0.
    CHECK(isCorpseAuraSuppressed(false, 0, 0, kDead, kDead, kLoot));
    // Dead via tracked deadCreatureGuids_.
    CHECK(isCorpseAuraSuppressed(true, 0, 0, 0, kDead, kLoot));
}

TEST_CASE("CharProc 13 kitLight clears immediately on unit death and expires normally while alive", "[corpse-aura]") {
    // Sathrovarr's Demonic Visual (44800) / Shadow Image Visual (45263) kit 7441:
    // CharProc 13 sets kitLight = (0, 0, 0) for 1e11 seconds.
    KitLightState light{glm::vec3(0.0f, 0.0f, 0.0f), 1e11f};
    tickKitLight(light, 10.0f);
    CHECK(light.color == glm::vec3(0.0f, 0.0f, 0.0f));
    CHECK(light.remaining > 1e10f);

    // On unit death, kitLight must reset immediately to white (1, 1, 1) with 0 remaining.
    resetKitLightOnDeath(light);
    CHECK(light.color == glm::vec3(1.0f, 1.0f, 1.0f));
    CHECK(light.remaining == 0.0f);

    // Short-duration impact kit (Chain Lightning kit 282: 0x6C6CFF for 0.5 s) expires on tick.
    KitLightState impact{glm::vec3(0x6C / 255.0f, 0x6C / 255.0f, 1.0f), 0.5f};
    tickKitLight(impact, 0.25f);
    CHECK(impact.remaining == Catch::Approx(0.25f));
    CHECK(impact.color.r == Catch::Approx(0x6C / 255.0f));
    tickKitLight(impact, 0.30f);
    CHECK(impact.remaining == 0.0f);
    CHECK(impact.color == glm::vec3(1.0f, 1.0f, 1.0f));
}

TEST_CASE("TBC Spell.dbc confirms Sathrovarr Demonic Visual 44800 is not death-persistent while Ghost 8326 is",
          "[corpse-aura][dbc]") {
    const std::filesystem::path spellPath = "/workspace/azerothcore/server/data/dbc/Spell.dbc";
    const std::filesystem::path layoutPath =
        std::filesystem::path(WOWEE_SOURCE_DIR) / "Data/expansions/tbc/dbc_layouts.json";
    if (!std::filesystem::exists(spellPath) || !std::filesystem::exists(layoutPath)) {
        SKIP("TBC Spell.dbc or dbc_layouts.json not present");
    }

    wowee::pipeline::DBCLayout layout;
    REQUIRE(layout.loadFromJson(layoutPath.string()));
    const auto* spellL = layout.getLayout("Spell");
    REQUIRE(spellL != nullptr);

    std::ifstream file(spellPath, std::ios::binary);
    REQUIRE(file.good());
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    wowee::pipeline::DBCFile dbc;
    REQUIRE(dbc.load(bytes));

    const uint32_t idCol = (*spellL)["ID"];
    const uint32_t attrEx3Col = (*spellL)["AttributesEx3"];
    const uint32_t visCol = (*spellL)["SpellVisualID"];

    bool foundDemonic = false;
    bool foundShadowImage = false;
    bool foundGhost = false;
    bool foundStolenSoulVisual = false;
    for (uint32_t i = 0; i < dbc.getRecordCount(); ++i) {
        const uint32_t id = dbc.getUInt32(i, idCol);
        const uint32_t attrEx3 = dbc.getUInt32(i, attrEx3Col);
        const uint32_t vis = dbc.getUInt32(i, visCol);
        if (id == 44800u) {
            foundDemonic = true;
            CHECK(vis == 8732u);
            CHECK_FALSE(auraVisualPersistsOnCorpse(attrEx3));
        } else if (id == 45263u) {
            foundShadowImage = true;
            CHECK(vis == 8732u);
            CHECK_FALSE(auraVisualPersistsOnCorpse(attrEx3));
        } else if (id == 8326u) {
            foundGhost = true;
            CHECK(auraVisualPersistsOnCorpse(attrEx3));
        } else if (id == 32395u) {
            // Stolen Soul Visual (Exarch Maladaar): has SPELL_ATTR_EX3_DEATH_PERSISTENT
            // so its CharProc 13 black kitLight (kit 6374) must survive unit death.
            foundStolenSoulVisual = true;
            CHECK(vis == 7264u);
            CHECK(auraVisualPersistsOnCorpse(attrEx3));
        }
    }
    CHECK(foundDemonic);
    CHECK(foundShadowImage);
    CHECK(foundGhost);
    CHECK(foundStolenSoulVisual);
}
