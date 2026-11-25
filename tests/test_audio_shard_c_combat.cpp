// Shard C (GAP-05) deterministic TBC 2.4.3 (Wow.exe build 8606) combat audio tests:
//   - WeaponSwingSounds2.dbc (6 rows: WeaponSize 0..2 x Crit 0..1 -> SoundEntries 233..238)
//   - ItemSubClass.dbc col 9 WeaponSwingSize + underwater 0.5f swing pitch (0x467000 / 0x88d620)
//   - Full 20-column WeaponImpactSounds.dbc (cols 3..22: Flesh, Chain, Plate, ShieldMetal,
//     ShieldWood, ParryMetalMetal, ParryMetalWood, Wood, Stone, Ethereal x normal/crit)
//   - CreatureSoundData.dbc col 25 CreatureImpactType -> (0=Flesh, 1=Stone, 2=Wood, 3=Ethereal)
//     via 0x8c76b4 (Infernal CSD 24, Stone Golem CSD 84) + CGPlayer_C::GetImpactType (0x639930)
//   - SheatheSoundLookups.dbc (33 rows) + Material.dbc (cols 3, 4 including Tome 11597/11598)
//   - $BWP (1144 BowPull, 1147 GunLoad) & $BWR (1146 BowRelease, 1148 GunRelease, 5414 Wand, 7140 Thrown)
//   - 3D local + remote combat outcome queue ($CSS / $CAH keyframe triggers + headless timed fallback)
//
// Run with:
//   WOWEE_TBC_DATA_DIR=/workspace/azerothcore/client/wowee-data/expansions/tbc ./build/bin/test_audio_shard_c_combat

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <catch_amalgamated.hpp>

#include "audio/audio_engine.hpp"
#include "audio/combat_sound_manager.hpp"
#include "audio/footstep_manager.hpp"
#include "audio/player_voice_manager.hpp"
#include "audio/sound_entries.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/m2_loader.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using namespace wowee;

namespace {

struct ShardCTestEnv {
    pipeline::AssetManager assets;
    pipeline::DBCLayout dbcLayout;
    bool ready = false;

    ShardCTestEnv() {
        (void)audio::AudioEngine::instance();
        (void)audio::SoundEntryTable::instance();

        const char* root = std::getenv("WOWEE_TBC_DATA_DIR");
        if (!root || !*root) return;
        if (!std::filesystem::exists(root)) return;
        if (!assets.initialize(root)) return;
        assets.setExpansionDataPath(root);

        const std::filesystem::path layoutPath =
            std::filesystem::path(WOWEE_SOURCE_DIR) / "Data" / "expansions" / "tbc" / "dbc_layouts.json";
        if (std::filesystem::exists(layoutPath) && dbcLayout.loadFromJson(layoutPath.string())) {
            pipeline::setActiveDBCLayout(&dbcLayout);
        }
        ready = true;
    }

    ~ShardCTestEnv() {
        audio::SoundEntryTable::instance().reset();
        audio::AudioEngine::instance().setPlaybackObserver({});
        audio::AudioEngine::instance().setAssetManager(nullptr);
        audio::AudioEngine::instance().shutdown();
        pipeline::setActiveDBCLayout(nullptr);
        assets.shutdown();
    }
};

ShardCTestEnv& env() {
    static ShardCTestEnv instance;
    if (!instance.ready) {
        SKIP("Set WOWEE_TBC_DATA_DIR to extracted TBC 2.4.3 data directory");
    }
    return instance;
}

struct ChannelRms {
    double left = 0.0;
    double right = 0.0;
    double total = 0.0;
};

ChannelRms renderRms(uint32_t frames) {
    std::vector<float> pcm(static_cast<size_t>(frames) * 2u, 0.0f);
    const uint32_t got = audio::AudioEngine::instance().renderMixFrames(pcm.data(), frames);
    REQUIRE(got == frames);
    double sumL = 0.0;
    double sumR = 0.0;
    for (uint32_t i = 0; i < frames; ++i) {
        const double l = pcm[static_cast<size_t>(i) * 2u];
        const double r = pcm[static_cast<size_t>(i) * 2u + 1u];
        REQUIRE(std::isfinite(l));
        REQUIRE(std::isfinite(r));
        sumL += l * l;
        sumR += r * r;
    }
    ChannelRms out;
    out.left = std::sqrt(sumL / std::max<uint32_t>(1u, frames));
    out.right = std::sqrt(sumR / std::max<uint32_t>(1u, frames));
    out.total = std::sqrt((sumL + sumR) / std::max<uint32_t>(1u, frames * 2u));
    return out;
}

void resetHeadlessEngine(pipeline::AssetManager* assets) {
    auto& engine = audio::AudioEngine::instance();
    engine.setPlaybackObserver({});
    engine.shutdown();
    REQUIRE(engine.initializeHeadless(48000, 2));
    engine.setAssetManager(assets);
    engine.setMasterVolume(1.0f);
    engine.setSuspended(false);
    engine.setListenerPosition(glm::vec3(0.0f));
    engine.setListenerOrientation(glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
}

} // namespace

TEST_CASE("WeaponSwingSounds2.dbc, ItemSubClass col 9, and underwater 0.5f pitch match TBC 2.4.3",
          "[audio][shard_c][swing]") {
    auto& tbc = env();
    resetHeadlessEngine(&tbc.assets);

    audio::CombatSoundManager csm;
    REQUIRE(csm.initialize(&tbc.assets));

    using WS = audio::CombatSoundManager::WeaponSize;

    // Exact 6 rows of WeaponSwingSounds2.dbc -> SoundEntries 233..238
    CHECK(csm.getWeaponSwingSoundId(WS::SMALL, false) == 233u);
    CHECK(csm.getWeaponSwingSoundId(WS::SMALL, true) == 234u);
    CHECK(csm.getWeaponSwingSoundId(WS::MEDIUM, false) == 235u);
    CHECK(csm.getWeaponSwingSoundId(WS::MEDIUM, true) == 236u);
    CHECK(csm.getWeaponSwingSoundId(WS::LARGE, false) == 237u);
    CHECK(csm.getWeaponSwingSoundId(WS::LARGE, true) == 238u);

    // ItemSubClass.dbc col 9 WeaponSwingSize mapping
    CHECK(csm.getWeaponSizeForSubclass(15u) == WS::SMALL);  // Dagger
    CHECK(csm.getWeaponSizeForSubclass(13u) == WS::SMALL);  // Fist / Unarmed
    CHECK(csm.getWeaponSizeForSubclass(0u) == WS::MEDIUM);  // 1H Axe
    CHECK(csm.getWeaponSizeForSubclass(4u) == WS::MEDIUM);  // 1H Mace
    CHECK(csm.getWeaponSizeForSubclass(7u) == WS::MEDIUM);  // 1H Sword
    CHECK(csm.getWeaponSizeForSubclass(1u) == WS::LARGE);   // 2H Axe
    CHECK(csm.getWeaponSizeForSubclass(5u) == WS::LARGE);   // 2H Mace
    CHECK(csm.getWeaponSizeForSubclass(6u) == WS::LARGE);   // Polearm
    CHECK(csm.getWeaponSizeForSubclass(8u) == WS::LARGE);   // 2H Sword
    CHECK(csm.getWeaponSizeForSubclass(10u) == WS::LARGE);  // Staff
    CHECK(csm.getWeaponSizeForSubclass(20u) == WS::LARGE);  // Fishing Pole

    std::vector<audio::AudioEngine::PlaybackEvent> events;
    audio::AudioEngine::instance().setPlaybackObserver(
        [&](const audio::AudioEngine::PlaybackEvent& ev) { events.push_back(ev); });

    // Dry swing: pitch == 1.0f
    events.clear();
    REQUIRE(csm.onUnitSwingEvent(0x1001ull, glm::vec3(0.0f), /*isLocalPlayer=*/true,
                                 /*offHand=*/false, /*underwater=*/false, /*subClass=*/8u));
    REQUIRE(events.size() == 1u);
    CHECK(events.back().started);
    CHECK_FALSE(events.back().spatial);
    CHECK(events.back().pitch == Catch::Approx(1.0f));
    CHECK(renderRms(2048).total > 1e-4);

    // Underwater swing (Wow.exe 0x467000 / float 0.5f at 0x88d620): pitch == 0.5f in 3D
    resetHeadlessEngine(&tbc.assets);
    audio::AudioEngine::instance().setPlaybackObserver(
        [&](const audio::AudioEngine::PlaybackEvent& ev) { events.push_back(ev); });
    events.clear();
    REQUIRE(csm.onUnitSwingEvent(0x1002ull, glm::vec3(-12.0f, 0.0f, 0.0f), /*isLocalPlayer=*/false,
                                 /*offHand=*/false, /*underwater=*/true, /*subClass=*/15u));
    REQUIRE(events.size() == 1u);
    CHECK(events.back().started);
    CHECK(events.back().spatial);
    CHECK(events.back().pitch == Catch::Approx(0.5f));
    const auto underwaterLeft = renderRms(4096);
    CHECK(underwaterLeft.total > 1e-4);

    resetHeadlessEngine(&tbc.assets);
    REQUIRE(csm.onUnitSwingEvent(0x1003ull, glm::vec3(12.0f, 0.0f, 0.0f), /*isLocalPlayer=*/false,
                                 /*offHand=*/false, /*underwater=*/true, /*subClass=*/15u));
    const auto underwaterRight = renderRms(4096);
    CHECK(underwaterRight.total > 1e-4);
    CHECK((underwaterLeft.left / std::max(1e-9, underwaterLeft.right)) >
          (underwaterRight.left / std::max(1e-9, underwaterRight.right)) * 1.5);

    audio::AudioEngine::instance().setPlaybackObserver({});
}

TEST_CASE("All 20 WeaponImpactSounds.dbc columns, CreatureImpactType, and Armor Material match TBC 2.4.3",
          "[audio][shard_c][impact]") {
    auto& tbc = env();
    resetHeadlessEngine(&tbc.assets);

    audio::CombatSoundManager csm;
    REQUIRE(csm.initialize(&tbc.assets));
    CHECK(csm.weaponImpactRowCount() == 30u);

    using IT = audio::CombatSoundManager::ImpactType;

    // Row (subClass=0 1H Axe, metal=1): verify all 10 normal cols (3..12) and all 10 crit cols (13..22)
    CHECK(csm.getWeaponImpactSoundId(0u, true, IT::FLESH, false) == 171u);
    CHECK(csm.getWeaponImpactSoundId(0u, true, IT::CHAIN, false) == 173u);
    CHECK(csm.getWeaponImpactSoundId(0u, true, IT::PLATE, false) == 175u);
    CHECK(csm.getWeaponImpactSoundId(0u, true, IT::SHIELD_METAL, false) == 3263u);
    CHECK(csm.getWeaponImpactSoundId(0u, true, IT::SHIELD_WOOD, false) == 3262u);
    CHECK(csm.getWeaponImpactSoundId(0u, true, IT::PARRY_METAL_METAL, false) == 1002u);
    CHECK(csm.getWeaponImpactSoundId(0u, true, IT::PARRY_METAL_WOOD, false) == 1001u);
    CHECK(csm.getWeaponImpactSoundId(0u, true, IT::WOOD, false) == 3202u);
    CHECK(csm.getWeaponImpactSoundId(0u, true, IT::STONE, false) == 3206u);
    CHECK(csm.getWeaponImpactSoundId(0u, true, IT::ETHEREAL, false) == 3210u);

    CHECK(csm.getWeaponImpactSoundId(0u, true, IT::FLESH, true) == 172u);
    CHECK(csm.getWeaponImpactSoundId(0u, true, IT::CHAIN, true) == 174u);
    CHECK(csm.getWeaponImpactSoundId(0u, true, IT::PLATE, true) == 176u);
    CHECK(csm.getWeaponImpactSoundId(0u, true, IT::SHIELD_METAL, true) == 3263u);
    CHECK(csm.getWeaponImpactSoundId(0u, true, IT::SHIELD_WOOD, true) == 3262u);
    CHECK(csm.getWeaponImpactSoundId(0u, true, IT::PARRY_METAL_METAL, true) == 1002u);
    CHECK(csm.getWeaponImpactSoundId(0u, true, IT::PARRY_METAL_WOOD, true) == 1001u);
    CHECK(csm.getWeaponImpactSoundId(0u, true, IT::WOOD, true) == 3203u);
    CHECK(csm.getWeaponImpactSoundId(0u, true, IT::STONE, true) == 3207u);
    CHECK(csm.getWeaponImpactSoundId(0u, true, IT::ETHEREAL, true) == 3210u);

    // 2H Staff (subClass=10, wood=false): wood parry (1000/999) + 2H wood/stone/ethereal (3204/3208/1211)
    CHECK(csm.getWeaponImpactSoundId(10u, false, IT::PARRY_METAL_METAL, false) == 1000u);
    CHECK(csm.getWeaponImpactSoundId(10u, false, IT::PARRY_METAL_WOOD, false) == 999u);
    CHECK(csm.getWeaponImpactSoundId(10u, false, IT::WOOD, false) == 3204u);
    CHECK(csm.getWeaponImpactSoundId(10u, false, IT::WOOD, true) == 3205u);
    CHECK(csm.getWeaponImpactSoundId(10u, false, IT::STONE, false) == 3208u);
    CHECK(csm.getWeaponImpactSoundId(10u, false, IT::STONE, true) == 3209u);
    CHECK(csm.getWeaponImpactSoundId(10u, false, IT::ETHEREAL, false) == 1211u);

    // CreatureSoundData col 25 CreatureImpactType -> 0x8c76b4 (0=Flesh, 1=Stone, 2=Wood, 3=Ethereal)
    CHECK(audio::CombatSoundManager::mapCreatureImpactEnum(0u) == IT::FLESH);
    CHECK(audio::CombatSoundManager::mapCreatureImpactEnum(1u) == IT::STONE);
    CHECK(audio::CombatSoundManager::mapCreatureImpactEnum(2u) == IT::WOOD);
    CHECK(audio::CombatSoundManager::mapCreatureImpactEnum(3u) == IT::ETHEREAL);

    // Infernal (CSD 24, display 169) and Stone Golem (CSD 84, display 473) resolve to STONE
    CHECK(csm.getCreatureImpactTypeForSoundData(24u) == IT::STONE);
    CHECK(csm.getCreatureImpactTypeForSoundData(84u) == IT::STONE);
    CHECK(csm.getCreatureImpactTypeForDisplay(169u) == IT::STONE);
    CHECK(csm.getCreatureImpactTypeForDisplay(473u) == IT::STONE);

    // CGPlayer_C::GetImpactType (0x639930) via Material.dbc flags
    CHECK(csm.resolveArmorMaterialImpactType(6) == IT::PLATE);  // Material 6 = Plate (flags=3)
    CHECK(csm.resolveArmorMaterialImpactType(5) == IT::CHAIN);  // Material 5 = Chain/Mail (flags=5)
    CHECK(csm.resolveArmorMaterialImpactType(7) == IT::FLESH);  // Material 7 = Cloth
    CHECK(csm.resolveArmorMaterialImpactType(8) == IT::FLESH);  // Material 8 = Leather

    // Item.dbc + ItemDisplayInfo.dbc pre-query weapon resolution (prevents Unarmed SE:1014 before SMSG_ITEM_QUERY)
    const auto sword25 = csm.resolveWeaponFromItemId(25u);    // Worn Shortsword
    CHECK(sword25.subClass == 7u);
    CHECK(sword25.isMetal);
    CHECK(csm.getWeaponImpactSoundId(sword25.subClass, sword25.isMetal, IT::FLESH, false) == 143u);

    const auto dagger2092 = csm.resolveWeaponFromItemId(2092u); // Worn Dagger
    CHECK(dagger2092.subClass == 15u);
    CHECK(dagger2092.isMetal);
    CHECK(csm.getWeaponImpactSoundId(dagger2092.subClass, dagger2092.isMetal, IT::FLESH, false) == 1151u);

    const auto staff35 = csm.resolveWeaponFromItemId(35u);    // Bent Staff
    CHECK(staff35.subClass == 10u);
    CHECK_FALSE(staff35.isMetal);
    CHECK(csm.getWeaponImpactSoundId(staff35.subClass, staff35.isMetal, IT::FLESH, false) == 941u);

    const auto bow2504 = csm.resolveWeaponFromItemId(2504u);  // Worn Shortbow
    CHECK(bow2504.subClass == 2u);
    const auto gun2508 = csm.resolveWeaponFromItemId(2508u);  // Old Blunderbuss
    CHECK(gun2508.subClass == 3u);

    // Every single one of the 10 impact categories synthesizes non-zero PCM in TBC data
    std::vector<audio::AudioEngine::PlaybackEvent> events;
    audio::AudioEngine::instance().setPlaybackObserver(
        [&](const audio::AudioEngine::PlaybackEvent& ev) { events.push_back(ev); });

    for (IT cat : {
             IT::FLESH, IT::CHAIN, IT::PLATE, IT::SHIELD_METAL, IT::SHIELD_WOOD,
             IT::PARRY_METAL_METAL, IT::PARRY_METAL_WOOD, IT::WOOD, IT::STONE, IT::ETHEREAL}) {
        for (bool crit : {false, true}) {
            INFO("ImpactType=" << int(cat) << " crit=" << crit);
            // Reset headless mixer each iteration so Shard A's TBC 2.4.3 kCombatImpact
            // concurrency cap (4 active channels, 0x897018) does not throttle rapid test probes.
            resetHeadlessEngine(&tbc.assets);
            audio::AudioEngine::instance().setPlaybackObserver(
                [&](const audio::AudioEngine::PlaybackEvent& ev) { events.push_back(ev); });
            events.clear();
            REQUIRE(csm.playWeaponImpact3D(8u, true, cat, crit, glm::vec3(6.0f, 0.0f, -3.0f), false));
            REQUIRE(events.size() == 1u);
            CHECK(events.back().started);
            CHECK(events.back().spatial);
            CHECK(renderRms(1024).total > 1e-5);
        }
    }

    audio::AudioEngine::instance().setPlaybackObserver({});
}

TEST_CASE("SheatheSoundLookups.dbc, Material.dbc Tome 11597/11598, and $BWP/$BWR ranged events",
          "[audio][shard_c][sheathe][ranged]") {
    auto& tbc = env();
    resetHeadlessEngine(&tbc.assets);

    audio::CombatSoundManager csm;
    REQUIRE(csm.initialize(&tbc.assets));
    CHECK(csm.sheatheLookupRowCount() == 33u);
    CHECK(csm.materialRowCount() == 8u);

    // Material.dbc sheathe/unsheathe including offhand Tome (Material 7 -> 11597 / 11598)
    CHECK(csm.getMaterialSheatheSoundId(1, false) == 698u);
    CHECK(csm.getMaterialSheatheSoundId(1, true) == 700u);
    CHECK(csm.getMaterialSheatheSoundId(2, false) == 697u);
    CHECK(csm.getMaterialSheatheSoundId(2, true) == 699u);
    CHECK(csm.getMaterialSheatheSoundId(6, false) == 696u);
    CHECK(csm.getMaterialSheatheSoundId(6, true) == 701u);
    CHECK(csm.getMaterialSheatheSoundId(7, false) == 11597u);
    CHECK(csm.getMaterialSheatheSoundId(7, true) == 11598u);

    // Shield (class=4, subClass=6) even when shield item material is 1 (metal) -> 696 / 701
    CHECK(csm.getSheatheSoundId(4u, 6u, 1, /*isShield=*/true, /*unsheathe=*/false) == 696u);
    CHECK(csm.getSheatheSoundId(4u, 6u, 1, /*isShield=*/true, /*unsheathe=*/true) == 701u);
    // Offhand Tome (class=4, subClass=0, material=7, isShield=false) -> 11597 / 11598
    CHECK(csm.getSheatheSoundId(4u, 0u, 7, /*isShield=*/false, /*unsheathe=*/false) == 11597u);
    CHECK(csm.getSheatheSoundId(4u, 0u, 7, /*isShield=*/false, /*unsheathe=*/true) == 11598u);

    // $BWP (0x62fbc0) and $BWR (0x61c254)
    CHECK(csm.getBowPullSoundId(2u) == 1144u);     // Bow
    CHECK(csm.getBowPullSoundId(18u) == 1144u);    // Crossbow
    CHECK(csm.getBowPullSoundId(3u) == 1147u);     // Gun
    CHECK(csm.getBowPullSoundId(19u) == 0u);       // Wand has no pull sound

    CHECK(csm.getBowReleaseSoundId(2u) == 1146u);  // Bow
    CHECK(csm.getBowReleaseSoundId(18u) == 1146u); // Crossbow
    CHECK(csm.getBowReleaseSoundId(3u) == 1148u);  // Gun
    CHECK(csm.getBowReleaseSoundId(16u) == 7140u); // Thrown
    CHECK(csm.getBowReleaseSoundId(19u) == 5414u); // Wand

    std::vector<audio::AudioEngine::PlaybackEvent> events;
    audio::AudioEngine::instance().setPlaybackObserver(
        [&](const audio::AudioEngine::PlaybackEvent& ev) { events.push_back(ev); });

    // Keyframe $SHL/$SHR for remote player offhand Tome in 3D
    events.clear();
    REQUIRE(csm.onUnitSheatheEvent(0x2001ull, glm::vec3(9.0f, 0.0f, -2.0f),
                                   /*isLocalPlayer=*/false, 4u, 0u, 7, /*isShield=*/false, /*unsheathe=*/true));
    REQUIRE(csm.onUnitSheatheEvent(0x2001ull, glm::vec3(9.0f, 0.0f, -2.0f),
                                   /*isLocalPlayer=*/false, 4u, 0u, 7, /*isShield=*/false, /*unsheathe=*/false));
    REQUIRE(events.size() == 2u);
    CHECK(events[0].spatial);
    CHECK(events[1].spatial);
    CHECK(renderRms(2048).total > 1e-4);

    // Keyframe $BWP + $BWR for remote hunter bow & gun
    events.clear();
    REQUIRE(csm.onUnitBowPullEvent(0x2002ull, glm::vec3(-7.0f, 0.0f, -2.0f), false, 2u));
    REQUIRE(csm.onUnitBowReleaseEvent(0x2002ull, glm::vec3(-7.0f, 0.0f, -2.0f), false, 2u));
    REQUIRE(csm.onUnitBowPullEvent(0x2003ull, glm::vec3(7.0f, 0.0f, -2.0f), false, 3u));
    REQUIRE(csm.onUnitBowReleaseEvent(0x2003ull, glm::vec3(7.0f, 0.0f, -2.0f), false, 3u));
    REQUIRE(csm.onUnitBowReleaseEvent(0x2004ull, glm::vec3(0.0f), true, 19u));
    REQUIRE(csm.onUnitBowReleaseEvent(0x2005ull, glm::vec3(0.0f), true, 16u));
    CHECK(events.size() == 6u);
    CHECK(events[0].spatial);
    CHECK(events[4].spatial == false);
    CHECK(renderRms(2048).total > 1e-4);

    audio::AudioEngine::instance().setPlaybackObserver({});
}

TEST_CASE("3D local and remote combat queue dispatches on $CSS/$CAH keyframes and timed fallback",
          "[audio][shard_c][queue]") {
    auto& tbc = env();
    resetHeadlessEngine(&tbc.assets);

    audio::CombatSoundManager csm;
    REQUIRE(csm.initialize(&tbc.assets));

    using CO = audio::CombatSoundManager::CombatOutcome;
    using IT = audio::CombatSoundManager::ImpactType;

    // Verify exact Wow.exe outcome classification (EVADE beats HITINFO_MISS, DODGE->MISS sound, etc.)
    CHECK(audio::CombatSoundManager::classifyMeleeOutcome(true, false, false, false, 6u, 0, 0) == CO::EVADE_DEFLECT);
    CHECK(audio::CombatSoundManager::classifyMeleeOutcome(false, false, false, false, 8u, 0, 0) == CO::EVADE_DEFLECT);
    CHECK(audio::CombatSoundManager::classifyMeleeOutcome(true, false, false, false, 1u, 0, 0) == CO::MISS);
    CHECK(audio::CombatSoundManager::classifyMeleeOutcome(false, false, false, false, 2u, 0, 0) == CO::DODGE);
    CHECK(audio::CombatSoundManager::classifyMeleeOutcome(false, false, false, false, 3u, 0, 0) == CO::PARRY);
    CHECK(audio::CombatSoundManager::classifyMeleeOutcome(false, false, false, false, 5u, 15, 25) == CO::BLOCK);
    CHECK(audio::CombatSoundManager::classifyMeleeOutcome(false, true, false, false, 1u, 120, 0) == CO::CRIT);
    CHECK(audio::CombatSoundManager::classifyMeleeOutcome(false, false, true, false, 1u, 180, 0) == CO::CRUSHING);

    std::vector<audio::AudioEngine::PlaybackEvent> events;
    audio::AudioEngine::instance().setPlaybackObserver(
        [&](const audio::AudioEngine::PlaybackEvent& ev) { events.push_back(ev); });

    int exertionCalls = 0;
    int woundCalls = 0;
    CO lastWoundOutcome = CO::HIT;
    csm.setAttackerExertionCallback([&](uint64_t, uint32_t, const glm::vec3&, bool, bool, bool) {
        ++exertionCalls;
    });
    csm.setVictimWoundCallback([&](uint64_t, uint32_t dispId, const glm::vec3& pos, bool, bool isLocal, CO outcome) {
        ++woundCalls;
        lastWoundOutcome = outcome;
        if (outcome == CO::CRUSHING && dispId != 0u) {
            csm.playCreatureCrushingWound(dispId, pos, isLocal);
        }
    });

    SECTION("Keyframe-driven remote NPC vs NPC combat plays zero sound at t=0 and 3D swing/impact at $CSS/$CAH") {
        const uint64_t npcAttacker = 0xF1300010ull;
        const uint64_t npcVictim = 0xF1300020ull;
        csm.setUnitHasKeyframeDriver(npcAttacker, true);

        audio::CombatSoundManager::QueuedCombatEvent ev;
        ev.attackerGuid = npcAttacker;
        ev.victimGuid = npcVictim;
        ev.attackerRenderPos = glm::vec3(-10.0f, 0.0f, 2.0f);
        ev.victimRenderPos = glm::vec3(10.0f, 0.0f, 2.0f);
        ev.attackerIsLocal = false;
        ev.victimIsLocal = false;
        ev.attackerWeaponSubClass = 8u; // 2H Sword
        ev.attackerWeaponIsMetal = true;
        ev.victimImpactType = IT::STONE; // Stone Golem target!
        ev.victimDisplayId = 49u; // Human Male displayId (has InjuryCrushingBlow)
        ev.outcome = CO::CRUSHING;

        events.clear();
        csm.queueCombatEvent(ev);
        // Zero premature playback on packet receipt!
        CHECK(events.empty());
        CHECK(csm.pendingCombatEventCount(npcAttacker) == 1u);

        // Advance 100ms before keyframes: still waiting for $CSS/$CAH!
        csm.update(0.10f);
        CHECK(events.empty());

        // M2 $CSS fires at attacker (-X -> left channel louder)
        REQUIRE(csm.onUnitSwingEvent(npcAttacker, ev.attackerRenderPos, false, false, false, 8u));
        REQUIRE(events.size() == 1u);
        CHECK(events.back().spatial);
        CHECK(exertionCalls == 1);
        const auto swingPan = renderRms(2048);
        CHECK(swingPan.left > swingPan.right * 1.2);

        // Reset headless mixer so $CAH right-channel pan is measured in isolation
        resetHeadlessEngine(&tbc.assets);
        audio::AudioEngine::instance().setPlaybackObserver(
            [&](const audio::AudioEngine::PlaybackEvent& pev) { events.push_back(pev); });
        events.clear();

        // M2 $CAH fires -> pops event, plays 2H Sword on Stone + Crushing Blow vocal at victim (+X -> right channel louder)
        REQUIRE(csm.onUnitHitEvent(npcAttacker, ev.attackerRenderPos, false));
        CHECK(csm.pendingCombatEventCount(npcAttacker) == 0u);
        CHECK(woundCalls == 1);
        CHECK(lastWoundOutcome == CO::CRUSHING);
        REQUIRE(events.size() >= 1u);
        CHECK(events.front().spatial);
        const auto hitPan = renderRms(2048);
        CHECK(hitPan.right > hitPan.left * 1.2);
    }

    SECTION("All defensive outcomes (MISS, DODGE, PARRY metal/wood, BLOCK metal/wood, EVADE/DEFLECT) play in 3D") {
        const uint64_t atk = 0xF1300100ull;
        csm.setUnitHasKeyframeDriver(atk, true);

        for (CO outcome : {CO::MISS, CO::DODGE, CO::PARRY, CO::BLOCK, CO::EVADE_DEFLECT}) {
            INFO("Testing outcome=" << int(outcome));
            resetHeadlessEngine(&tbc.assets);
            audio::AudioEngine::instance().setPlaybackObserver(
                [&](const audio::AudioEngine::PlaybackEvent& ev) { events.push_back(ev); });
            audio::CombatSoundManager::QueuedCombatEvent q;
            q.attackerGuid = atk;
            q.victimGuid = 0xF1300200ull;
            q.attackerRenderPos = glm::vec3(-4.0f, 0.0f, 2.0f);
            q.victimRenderPos = glm::vec3(5.0f, 0.0f, -3.0f);
            q.attackerWeaponSubClass = 7u; // 1H Sword
            q.attackerWeaponIsMetal = true;
            q.defenderParryWeaponIsMetal = (outcome == CO::PARRY);
            q.defenderShieldIsMetal = false; // Wooden shield block -> 3262
            q.outcome = outcome;

            csm.queueCombatEvent(q);
            events.clear();
            REQUIRE(csm.onUnitHitEvent(atk));
            REQUIRE(events.size() == 1u);
            CHECK(events.back().started);
            CHECK(events.back().spatial);
            CHECK(renderRms(1024).total > 1e-5);
        }
    }

    SECTION("Headless timed fallback flushes swing then impact deterministically when no M2 model exists") {
        const uint64_t headlessUnit = 0xF1300999ull;
        CHECK_FALSE(csm.unitHasKeyframeDriver(headlessUnit));

        audio::CombatSoundManager::QueuedCombatEvent ev;
        ev.attackerGuid = headlessUnit;
        ev.victimGuid = 0x00000001ull;
        ev.attackerIsLocal = true;
        ev.victimIsLocal = false;
        ev.attackerUnderwater = true; // Underwater local swing -> pitch 0.5f!
        ev.attackerWeaponSubClass = 4u; // 1H Mace
        ev.attackerWeaponIsMetal = true;
        ev.victimImpactType = IT::PLATE;
        ev.outcome = CO::CRIT;

        events.clear();
        exertionCalls = 0;
        woundCalls = 0;
        csm.queueCombatEvent(ev);
        CHECK(events.empty());

        // Step past headless swing delay (0.08s) but before hit delay (0.20s)
        csm.update(0.10f);
        REQUIRE(events.size() == 1u);
        CHECK_FALSE(events[0].spatial);
        CHECK(events[0].pitch == Catch::Approx(0.5f));
        CHECK(exertionCalls == 1);
        CHECK(woundCalls == 0);
        CHECK(csm.pendingCombatEventCount(headlessUnit) == 1u);

        // Step past headless hit delay (0.20s) -> crit mace-on-plate impact + wound callback
        csm.update(0.15f);
        REQUIRE(events.size() == 2u);
        CHECK_FALSE(events[1].spatial);
        CHECK(events[1].pitch == Catch::Approx(1.0f));
        CHECK(woundCalls == 1);
        CHECK(lastWoundOutcome == CO::CRIT);
        CHECK(csm.pendingCombatEventCount(headlessUnit) == 0u);
    }

    audio::AudioEngine::instance().setPlaybackObserver({});
}

TEST_CASE("M2 audio bridge dispatches HumanMale.m2 combat ($CSS/$CAH/$BWP/$BWR/$SHL/$SHR) and $ESD emote keyframes",
          "[audio][shard_c][m2_bridge]") {
    auto& tbc = env();
    resetHeadlessEngine(&tbc.assets);

    audio::FootstepManager fm;
    audio::CombatSoundManager csm;
    audio::PlayerVoiceManager pvm;
    REQUIRE(fm.initialize(&tbc.assets));
    REQUIRE(csm.initialize(&tbc.assets));
    REQUIRE(pvm.initialize(&tbc.assets));

    const auto m2Bytes = tbc.assets.readFile("Character\\Human\\Male\\HumanMale.m2");
    REQUIRE_FALSE(m2Bytes.empty());
    const pipeline::M2Model model = pipeline::M2Loader::load(m2Bytes);
    REQUIRE_FALSE(model.sequences.empty());
    REQUIRE_FALSE(model.audioEvents.empty());

    using T = pipeline::M2AudioEventType;
    auto findSeqWithEvent = [&](T want) -> std::pair<int, uint32_t> {
        for (const auto& ev : model.audioEvents) {
            if (ev.type != want) continue;
            for (size_t s = 0; s < ev.times.size() && s < model.sequences.size(); ++s) {
                if (!ev.times[s].empty()) {
                    return {static_cast<int>(s), ev.times[s].front()};
                }
            }
        }
        return {-1, 0u};
    };

    std::vector<audio::AudioEngine::PlaybackEvent> events;
    auto rebindObserver = [&]() {
        resetHeadlessEngine(&tbc.assets);
        audio::AudioEngine::instance().setPlaybackObserver(
            [&](const audio::AudioEngine::PlaybackEvent& ev) { events.push_back(ev); });
        events.clear();
    };

    // 1. Real HumanMale.m2 $CSS + $CAH combat swing & hit bridge
    const auto [cssSeq, cssTime] = findSeqWithEvent(T::CombatSwing);
    const auto [cahSeq, cahTime] = findSeqWithEvent(T::CombatAttackHit);
    REQUIRE(cssSeq >= 0);
    REQUIRE(cahSeq >= 0);

    const uint64_t unitGuid = 0xF130ABCDull;
    const glm::vec3 unitPos(7.0f, 0.0f, 2.0f);
    csm.setUnitHasKeyframeDriver(unitGuid, true);

    audio::CombatSoundManager::QueuedCombatEvent q;
    q.attackerGuid = unitGuid;
    q.victimGuid = 0xF130DCBAull;
    q.attackerRenderPos = unitPos;
    q.victimRenderPos = glm::vec3(8.0f, 0.0f, 2.0f);
    q.attackerWeaponSubClass = 7u; // 1H Sword
    q.attackerWeaponIsMetal = true;
    q.victimImpactType = audio::CombatSoundManager::ImpactType::PLATE;
    q.outcome = audio::CombatSoundManager::CombatOutcome::CRIT;
    csm.queueCombatEvent(q);

    int swingFired = 0;
    int hitFired = 0;
    int bowPullFired = 0;
    int bowReleaseFired = 0;
    int sheatheFired = 0;
    int emoteFired = 0;

    audio::FootstepManager::CharacterAudioStepContext ctx;
    ctx.displayId = 49u;
    ctx.isLocalPlayer = false;
    ctx.skipFootstep = true;
    ctx.onCustomUnitEvent = [&](T evType, uint32_t eventData, uint32_t animId, const glm::vec3& pos) -> bool {
        switch (evType) {
            case T::CombatSwing:
                ++swingFired;
                return csm.onUnitSwingEvent(unitGuid, pos, false, false, false, 7u);
            case T::CombatAttackHit:
            case T::ArmorHit0:
            case T::ArmorHit1:
            case T::ArmorHit2:
            case T::ArmorHit3:
                ++hitFired;
                return csm.onUnitHitEvent(unitGuid, pos, false);
            case T::BowPull:
                ++bowPullFired;
                return csm.onUnitBowPullEvent(unitGuid, pos, false, 2u);
            case T::BowRelease:
                ++bowReleaseFired;
                return csm.onUnitBowReleaseEvent(unitGuid, pos, false, 2u);
            case T::SheatheLeft:
            case T::SheatheRight:
                ++sheatheFired;
                return csm.onUnitSheatheEvent(unitGuid, pos, false, 2u, 7u, 1, false, true);
            case T::EmoteSound:
                ++emoteFired;
                return pvm.playEmoteAnimSoundForEvent(eventData, 233u, animId, false, pos);
            default:
                return false;
        }
    };

    auto bonePos = [&](uint16_t, const glm::vec3&) { return unitPos; };

    rebindObserver();
    fm.stepCharacterAudioEvents(101u, model, cssSeq, 0.0f, false, -1, 0.0f, ctx, bonePos);
    fm.stepCharacterAudioEvents(101u, model, cssSeq, static_cast<float>(cssTime + 5u), false, -1, 0.0f, ctx, bonePos);
    CHECK(swingFired >= 1);
    REQUIRE_FALSE(events.empty());
    CHECK(events.back().spatial);
    CHECK(renderRms(1024).total > 1e-5);

    rebindObserver();
    fm.removeCharacterAudioInstance(101u);
    fm.stepCharacterAudioEvents(101u, model, cahSeq, 0.0f, false, -1, 0.0f, ctx, bonePos);
    fm.stepCharacterAudioEvents(101u, model, cahSeq, static_cast<float>(cahTime + 5u), false, -1, 0.0f, ctx, bonePos);
    CHECK(hitFired >= 1);
    CHECK(csm.pendingCombatEventCount(unitGuid) == 0u);
    REQUIRE_FALSE(events.empty());
    CHECK(events.back().spatial);
    CHECK(renderRms(1024).total > 1e-5);

    // 2. Real HumanMale.m2 $BWP, $BWR, $SHR/$SHL, and $ESD events
    for (auto [evType, counterPtr, label] : {
             std::tuple{T::BowPull, &bowPullFired, "$BWP"},
             std::tuple{T::BowRelease, &bowReleaseFired, "$BWR"},
             std::tuple{T::SheatheRight, &sheatheFired, "$SHR"},
             std::tuple{T::EmoteSound, &emoteFired, "$ESD"},
         }) {
        INFO("M2 bridge event " << label);
        const auto [seqIdx, tsMs] = findSeqWithEvent(evType);
        REQUIRE(seqIdx >= 0);
        rebindObserver();
        fm.removeCharacterAudioInstance(102u);
        fm.stepCharacterAudioEvents(102u, model, seqIdx, 0.0f, false, -1, 0.0f, ctx, bonePos);
        fm.stepCharacterAudioEvents(102u, model, seqIdx, static_cast<float>(tsMs + 5u), false, -1, 0.0f, ctx, bonePos);
        CHECK(*counterPtr >= 1);
        REQUIRE_FALSE(events.empty());
        CHECK(events.back().spatial);
        CHECK(renderRms(1024).total > 1e-5);
    }

    // 3. PlayerVoiceManager Emotes.dbc col 2 AnimID -> col 6 EmoteSoundID lookup
    CHECK(pvm.getEmoteSoundId(233u) == 3782u);
    CHECK(pvm.getEmoteSoundId(234u) == 3202u);
    CHECK(pvm.getEmoteSoundId(380u) == 3354u);

    audio::AudioEngine::instance().setPlaybackObserver({});
    fm.shutdown();
    pvm.shutdown();
    csm.shutdown();
}
