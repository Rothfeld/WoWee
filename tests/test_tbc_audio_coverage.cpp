// End-to-end TBC 2.4.3 (build 8606) audio validation suite.
//
// Exercises real TBC DBCs, WAV/MP3 assets, headless PCM synthesis, 3D stereo
// panning/attenuation, stoppable sound handles, mute/suspend gates, UI lookups,
// day/night zone music, intro cooldowns, weather/underwater loops, M2 $WNG/$WGG
// wing events, SpellVisualKit sounds, weapon impact/sheathe/ranged tables,
// GameObjectDisplayInfo sounds, NPCSounds, EmotesTextSound, and all 10 TBC
// playable races' vocal banks.
//
// Run with:
//   WOWEE_TBC_DATA_DIR=/workspace/azerothcore/client/wowee-data/expansions/tbc ./build/bin/test_tbc_audio_coverage

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <catch_amalgamated.hpp>

#include "audio/activity_sound_manager.hpp"
#include "audio/ambient_sound_manager.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/audio_engine.hpp"
#include "audio/combat_sound_manager.hpp"
#include "audio/creature_sound_index.hpp"
#include "audio/footstep_manager.hpp"
#include "audio/mount_sound_manager.hpp"
#include "audio/movement_sound_manager.hpp"
#include "audio/music_manager.hpp"
#include "audio/npc_voice_manager.hpp"
#include "audio/player_voice_manager.hpp"
#include "audio/sound_entries.hpp"
#include "audio/spell_sound_manager.hpp"
#include "audio/ui_sound_manager.hpp"
#include "game/zone_manager.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/m2_loader.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace wowee;

namespace {

struct TbcAudioTestEnv {
    pipeline::AssetManager assets;
    pipeline::DBCLayout dbcLayout;
    bool ready = false;

    TbcAudioTestEnv() {
        // Construct AudioEngine and SoundEntryTable singletons BEFORE TbcAudioTestEnv
        // finishes construction so C++ reverse static destruction tears down
        // TbcAudioTestEnv BEFORE those singletons are destroyed.
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

    ~TbcAudioTestEnv() {
        audio::SoundEntryTable::instance().reset();
        audio::AudioEngine::instance().setPlaybackObserver({});
        audio::AudioEngine::instance().setAssetManager(nullptr);
        audio::AudioEngine::instance().shutdown();
        pipeline::setActiveDBCLayout(nullptr);
        assets.shutdown();
    }
};

TbcAudioTestEnv& env() {
    static TbcAudioTestEnv instance;
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

} // namespace

TEST_CASE("AudioEngine decodes TBC WAV and MP3 SFX, pitches, pans 3D, stops, and captures", "[audio][tbc]") {
    auto& tbc = env();
    auto& engine = audio::AudioEngine::instance();
    engine.shutdown();
    REQUIRE(engine.initializeHeadless(48000, 2));
    engine.setAssetManager(&tbc.assets);
    engine.setMasterVolume(1.0f);
    engine.setSuspended(false);

    std::vector<audio::AudioEngine::PlaybackEvent> events;
    engine.setPlaybackObserver([&](const audio::AudioEngine::PlaybackEvent& ev) {
        events.push_back(ev);
    });

    SECTION("Real TBC WAV and MP3 SFX both decode and synthesize non-zero PCM") {
        events.clear();
        REQUIRE(engine.playSound2D("Sound\\Interface\\iQuestComplete.wav", 0.9f, 1.0f));
        REQUIRE(events.size() == 1);
        CHECK(events.back().started);
        CHECK(events.back().decodedFrames > 1000);
        const auto wavRms = renderRms(4096);
        CHECK(wavRms.total > 1e-4);

        // Drain active sounds before testing MP3 SFX (which returns MA_AT_END on decode)
        engine.shutdown();
        REQUIRE(engine.initializeHeadless(48000, 2));
        engine.setAssetManager(&tbc.assets);
        engine.setPlaybackObserver([&](const audio::AudioEngine::PlaybackEvent& ev) {
            events.push_back(ev);
        });

        events.clear();
        REQUIRE(tbc.assets.fileExists("Sound\\CinematicVoices\\BloodElfNarration.mp3"));
        REQUIRE(engine.playSound2D("Sound\\CinematicVoices\\BloodElfNarration.mp3", 1.0f, 1.0f));
        REQUIRE(events.size() == 1);
        CHECK(events.back().started);
        CHECK(events.back().decodedFrames > 5000);
        const auto mp3Rms = renderRms(48000);
        CHECK(mp3Rms.total > 1e-4);
    }

    SECTION("2D pitch multiplier alters playback duration as expected") {
        auto measureActiveChunks = [&](float pitch) {
            engine.shutdown();
            REQUIRE(engine.initializeHeadless(48000, 2));
            engine.setAssetManager(&tbc.assets);
            REQUIRE(engine.playSound2D("Sound\\Interface\\uEscapeScreenOpen.wav", 1.0f, pitch));
            int chunks = 0;
            for (int i = 0; i < 120; ++i) {
                (void)renderRms(1024);
                engine.update(1024.0f / 48000.0f);
                if (engine.getActiveSoundCount() == 0) break;
                ++chunks;
            }
            return chunks;
        };

        const int normalChunks = measureActiveChunks(1.0f);
        const int doublePitchChunks = measureActiveChunks(2.0f);
        REQUIRE(normalChunks > 2);
        CHECK(doublePitchChunks < normalChunks);
    }

    SECTION("3D positional audio pans left/right and attenuates with distance") {
        engine.shutdown();
        REQUIRE(engine.initializeHeadless(48000, 2));
        engine.setAssetManager(&tbc.assets);
        engine.setListenerPosition(glm::vec3(0.0f, 0.0f, 0.0f));
        engine.setListenerOrientation(glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));

        // Left of listener (-X)
        REQUIRE(engine.playSound3D("Sound\\Interface\\iQuestComplete.wav",
                                   glm::vec3(-12.0f, 0.0f, 0.0f), 1.0f, 1.0f, 100.0f, 2.0f));
        const auto leftPan = renderRms(4096);
        CHECK(leftPan.left > leftPan.right * 1.25);

        engine.shutdown();
        REQUIRE(engine.initializeHeadless(48000, 2));
        engine.setAssetManager(&tbc.assets);
        engine.setListenerPosition(glm::vec3(0.0f, 0.0f, 0.0f));
        engine.setListenerOrientation(glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));

        // Right of listener (+X)
        REQUIRE(engine.playSound3D("Sound\\Interface\\iQuestComplete.wav",
                                   glm::vec3(12.0f, 0.0f, 0.0f), 1.0f, 1.0f, 100.0f, 2.0f));
        const auto rightPan = renderRms(4096);
        CHECK(rightPan.right > rightPan.left * 1.25);

        // Near vs far distance attenuation
        engine.shutdown();
        REQUIRE(engine.initializeHeadless(48000, 2));
        engine.setAssetManager(&tbc.assets);
        engine.setListenerPosition(glm::vec3(0.0f));
        REQUIRE(engine.playSound3D("Sound\\Interface\\iQuestComplete.wav",
                                   glm::vec3(0.0f, 0.0f, -2.0f), 1.0f, 1.0f, 80.0f, 2.0f));
        const auto nearRms = renderRms(4096);

        engine.shutdown();
        REQUIRE(engine.initializeHeadless(48000, 2));
        engine.setAssetManager(&tbc.assets);
        engine.setListenerPosition(glm::vec3(0.0f));
        REQUIRE(engine.playSound3D("Sound\\Interface\\iQuestComplete.wav",
                                   glm::vec3(0.0f, 0.0f, -40.0f), 1.0f, 1.0f, 80.0f, 2.0f));
        const auto farRms = renderRms(4096);
        CHECK(nearRms.total > farRms.total * 2.0);
    }

    SECTION("Stoppable 2D/3D handles, master mute, and background suspend") {
        engine.shutdown();
        REQUIRE(engine.initializeHeadless(48000, 2));
        engine.setAssetManager(&tbc.assets);

        const uint32_t handle = engine.playSound2DStoppable("Sound\\Spells\\PreCastFireMedium.wav", 1.0f);
        REQUIRE(handle != 0);
        CHECK(renderRms(2048).total > 1e-4);
        engine.stopSound(handle);
        CHECK(renderRms(2048).total == Catch::Approx(0.0).margin(1e-6));

        const uint32_t handle3d = engine.playSound3DStoppable(
            "Sound\\Spells\\PreCastFrostMagicMedium.wav", glm::vec3(0.0f, 0.0f, -3.0f), 1.0f);
        REQUIRE(handle3d != 0);
        CHECK(renderRms(2048).total > 1e-4);
        engine.stopSound(handle3d);
        CHECK(renderRms(2048).total == Catch::Approx(0.0).margin(1e-6));

        // Suspend silences output without clobbering masterVolume
        engine.setMasterVolume(0.8f);
        const uint32_t suspHandle = engine.playSound2DStoppable("Sound\\Interface\\iQuestComplete.wav", 1.0f);
        REQUIRE(suspHandle != 0);
        engine.setSuspended(true);
        CHECK(engine.getMasterVolume() == Catch::Approx(0.8f));
        CHECK(renderRms(1024).total == Catch::Approx(0.0).margin(1e-6));
        engine.setSuspended(false);
        CHECK(renderRms(2048).total > 1e-4);
        engine.stopSound(suspHandle);

        // Master volume 0 prevents starting new one-shots
        engine.setMasterVolume(0.0f);
        CHECK_FALSE(engine.playSound2D("Sound\\Interface\\iQuestComplete.wav", 1.0f));
        engine.setMasterVolume(1.0f);
    }

    SECTION("Live device fallback initializes on headless Linux and captures output ring") {
        engine.setPlaybackObserver({});
        engine.shutdown();
        REQUIRE(engine.initialize());
        engine.setAssetManager(&tbc.assets);
        CHECK_FALSE(engine.getOutputDeviceName().empty());
        REQUIRE(engine.beginOutputCapture());
        REQUIRE(engine.playSound2D("Sound\\Interface\\iQuestComplete.wav", 0.8f));
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        const uint32_t avail = engine.capturedOutputAvailable();
        CHECK(avail > 0);
        std::vector<float> captured(static_cast<size_t>(avail) * engine.getOutputChannels(), 0.0f);
        CHECK(engine.readCapturedOutput(captured.data(), avail) > 0);
        engine.endOutputCapture();
        engine.shutdown();
        REQUIRE(engine.initializeHeadless(48000, 2));
        engine.setAssetManager(&tbc.assets);
    }

    engine.setPlaybackObserver({});
}

TEST_CASE("SoundEntryTable and UiSoundManager resolve TBC SoundEntries, UISoundLookups, and ItemGroupSounds",
          "[audio][tbc]") {
    auto& tbc = env();
    auto& engine = audio::AudioEngine::instance();
    engine.shutdown();
    REQUIRE(engine.initializeHeadless(48000, 2));
    engine.setAssetManager(&tbc.assets);

    auto& table = audio::SoundEntryTable::instance();
    table.reset();
    REQUIRE(table.ensureLoaded(&tbc.assets));
    CHECK(table.entryCount() > 6000);

    audio::UiSoundManager ui;
    REQUIRE(ui.initialize(&tbc.assets));

    std::vector<audio::AudioEngine::PlaybackEvent> events;
    engine.setPlaybackObserver([&](const audio::AudioEngine::PlaybackEvent& ev) {
        events.push_back(ev);
    });

    // Direct SoundEntries.Name lookups + UISoundLookups.dbc legacy aliases
    for (const char* name : {
             "gsTitleOptionOK",
             "igMainMenuOptionCheckBoxOn",
             "igQuestFailed",
             "igQuestListOpen",
             "TellMessage",
             "LevelUp",
             "TITLEENTERWORLD",
             "LOOTWINDOWCOINSOUND",
             "BAGMENUBUTTONPRESS",
             "INTERFACESOUND_MONEYFRAMEOPEN",
             "GAMEDIALOGOPEN",
         }) {
        INFO("UI sound name: " << name);
        const auto* entry = table.findByName(&tbc.assets, name);
        REQUIRE(entry != nullptr);
        events.clear();
        CHECK(ui.playByName(name));
        CHECK_FALSE(events.empty());
    }

    // TBC-fixed direct UI sound banks (bag open/close, fishing bite, whisper)
    auto expectPlays = [&](auto fn, const char* label) {
        INFO("UiSoundManager method: " << label);
        events.clear();
        fn();
        REQUIRE_FALSE(events.empty());
        CHECK(events.back().started);
    };
    expectPlays([&] { ui.playBagOpen(); }, "playBagOpen");
    expectPlays([&] { ui.playBagClose(); }, "playBagClose");
    expectPlays([&] { ui.playFishingBite(); }, "playFishingBite");
    expectPlays([&] { ui.playWhisperReceived(); }, "playWhisperReceived");
    expectPlays([&] { ui.playLootCoinSmall(); }, "playLootCoinSmall");
    expectPlays([&] { ui.playLevelUp(); }, "playLevelUp");

    // Numeric SoundEntries ID (8959 = RaidWarning) + ItemGroupSounds + ItemDisplayInfo
    events.clear();
    CHECK(ui.playById(8959));
    CHECK_FALSE(events.empty());

    events.clear();
    CHECK(ui.playItemGroupSound(7, /*pickup=*/true));
    CHECK(ui.playItemGroupSound(7, /*pickup=*/false));
    CHECK(events.size() == 2);

    events.clear();
    CHECK(ui.playItemDisplaySound(220, /*pickup=*/true));
    CHECK(ui.playItemDisplaySound(220, /*pickup=*/false));
    CHECK(events.size() == 2);

    engine.setPlaybackObserver({});
}

TEST_CASE("ZoneManager, AudioCoordinator, MusicManager, and AmbientSoundManager follow TBC 2.4.3 rules",
          "[audio][tbc]") {
    auto& tbc = env();
    auto& engine = audio::AudioEngine::instance();
    engine.shutdown();
    REQUIRE(engine.initializeHeadless(48000, 2));
    engine.setAssetManager(&tbc.assets);

    game::ZoneManager zones;
    zones.initialize();
    zones.setUseOriginalSoundtrack(false);
    zones.enrichFromDBC(&tbc.assets);

    SECTION("Day and night ZoneMusic tracks are strictly separated") {
        // Elwynn Forest AreaTable ID = 12
        const uint32_t musicId = zones.getAreaZoneMusicId(12u);
        REQUIRE(musicId != 0);
        const auto* combinedTracks = zones.getZoneMusicTracks(musicId);
        REQUIRE(combinedTracks != nullptr);
        CHECK_FALSE(combinedTracks->empty());

        for (int i = 0; i < 40; ++i) {
            const std::string dayTrack =
                zones.pickZoneMusicTrack(12u, musicId, /*night=*/false, /*includeWoweeTracks=*/false);
            REQUIRE_FALSE(dayTrack.empty());
            CHECK(tbc.assets.fileExists(dayTrack));
            std::string lower = dayTrack;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            CHECK(lower.find("night") == std::string::npos);
        }

        const std::string nightTrack =
            zones.pickZoneMusicTrack(12u, musicId, /*night=*/true, /*includeWoweeTracks=*/false);
        REQUIRE_FALSE(nightTrack.empty());
        CHECK(tbc.assets.fileExists(nightTrack));
    }

    SECTION("Stormwind City resolves ZoneIntroMusicTable and respects MinDelayMinutes cooldown") {
        // Stormwind City AreaTable ID = 1519
        const uint32_t introId = zones.getAreaIntroMusicId(1519u);
        REQUIRE(introId != 0);
        const auto* intro = zones.getZoneIntroMusic(introId);
        REQUIRE(intro != nullptr);
        REQUIRE_FALSE(intro->tracks.empty());
        CHECK(tbc.assets.fileExists(intro->tracks.front()));
        CHECK(intro->minDelaySeconds > 0.0f);

        audio::AudioCoordinator ac;
        REQUIRE(ac.initialize());
        ac.initializeEarlyAssets(&tbc.assets);

        audio::ZoneAudioContext ctx;
        ctx.zoneManager = &zones;
        ctx.hasTile = true;
        ctx.deltaTime = 0.25f;
        ctx.areaId = 1519u; // Enter Stormwind -> intro plays
        ctx.serverZoneId = 1519u;
        ac.updateZoneAudio(ctx);
        REQUIRE(ac.getMusicManager() != nullptr);
        std::string firstTrack = ac.getMusicManager()->getCurrentTrack();
        std::transform(firstTrack.begin(), firstTrack.end(), firstTrack.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        CHECK(firstTrack.find("stormwind") != std::string::npos);
        CHECK(firstTrack.find("intro") != std::string::npos);

        // Leave to Elwynn (12) and immediately re-enter Stormwind: intro must be on cooldown!
        ctx.areaId = 12u;
        ctx.serverZoneId = 12u;
        ctx.deltaTime = 5.0f;
        ac.updateZoneAudio(ctx);
        ctx.deltaTime = 1.0f;
        ac.updateZoneAudio(ctx); // Complete crossfade into Elwynn

        ctx.areaId = 1519u;
        ctx.serverZoneId = 1519u;
        ctx.deltaTime = 5.0f;
        ac.updateZoneAudio(ctx);
        ctx.deltaTime = 1.0f;
        ac.updateZoneAudio(ctx); // Complete crossfade into Stormwind regular ZoneMusic
        std::string reenterTrack = ac.getMusicManager()->getCurrentTrack();
        std::transform(reenterTrack.begin(), reenterTrack.end(), reenterTrack.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        CHECK(reenterTrack.find("stormwind") != std::string::npos);
        CHECK(reenterTrack.find("intro") == std::string::npos);

        ac.shutdown();
        REQUIRE(engine.initializeHeadless(48000, 2));
        engine.setAssetManager(&tbc.assets);
    }

    SECTION("MusicManager streams TBC MP3 music and AmbientSoundManager loads TBC Rain/Storm/Underwater") {
        audio::MusicManager music;
        REQUIRE(music.initialize(&tbc.assets));
        music.playMusic("Sound\\Music\\GlueScreenMusic\\BC_main_theme.mp3", /*loop=*/true);
        CHECK(engine.isMusicPlaying());
        CHECK(renderRms(48000).total > 1e-4);
        music.stopMusic(0.0f);

        audio::AmbientSoundManager ambient;
        REQUIRE(ambient.initialize(&tbc.assets));

        std::vector<audio::AudioEngine::PlaybackEvent> events;
        engine.setPlaybackObserver([&](const audio::AudioEngine::PlaybackEvent& ev) {
            events.push_back(ev);
        });

        // Rain (RainMediumLoop.wav in TBC)
        events.clear();
        ambient.setWeather(audio::AmbientSoundManager::WeatherType::RAIN_MEDIUM);
        ambient.update(0.1f, glm::vec3(0.0f), /*isIndoor=*/false, /*isSwimming=*/false);
        CHECK_FALSE(events.empty());

        // Storm / Sandstorm (SandStormMedium.wav in TBC)
        events.clear();
        ambient.setWeather(audio::AmbientSoundManager::WeatherType::STORM_MEDIUM);
        ambient.update(0.1f, glm::vec3(0.0f), /*isIndoor=*/false, /*isSwimming=*/false);
        CHECK_FALSE(events.empty());

        // Underwater loop (UndwaterLoop.wav in TBC)
        ambient.setWeather(audio::AmbientSoundManager::WeatherType::NONE);
        events.clear();
        ambient.update(0.1f, glm::vec3(0.0f), /*isIndoor=*/false, /*isSwimming=*/true);
        CHECK_FALSE(events.empty());

        ambient.shutdown();
        engine.setPlaybackObserver({});
    }
}

TEST_CASE("Movement, M2 $WNG/$WGG wing events, Mount, Combat, SpellVisualKit, GameObject, NPC, and Player vocals",
          "[audio][tbc]") {
    auto& tbc = env();
    auto& engine = audio::AudioEngine::instance();
    engine.shutdown();
    REQUIRE(engine.initializeHeadless(48000, 2));
    engine.setAssetManager(&tbc.assets);

    std::vector<audio::AudioEngine::PlaybackEvent> events;
    engine.setPlaybackObserver([&](const audio::AudioEngine::PlaybackEvent& ev) {
        events.push_back(ev);
    });

    SECTION("MovementSoundManager water splash and wading steps") {
        audio::MovementSoundManager move;
        REQUIRE(move.initialize(&tbc.assets));
        events.clear();
        move.playEnterWater(audio::MovementSoundManager::CharacterSize::SMALL);
        CHECK_FALSE(events.empty());
        events.clear();
        move.playEnterWater(audio::MovementSoundManager::CharacterSize::MEDIUM);
        CHECK_FALSE(events.empty());
        events.clear();
        move.playEnterWater(audio::MovementSoundManager::CharacterSize::GIANT);
        CHECK_FALSE(events.empty());
        events.clear();
        move.playWaterFootstep(audio::MovementSoundManager::CharacterSize::MEDIUM);
        CHECK_FALSE(events.empty());
        move.shutdown();
    }

    SECTION("M2Loader parses $WNG and $WGG wing events on TBC Gryphon and MountSoundManager plays WingFlap") {
        const auto m2Bytes = tbc.assets.readFile("Creature\\Gryphon\\Gryphon.m2");
        REQUIRE_FALSE(m2Bytes.empty());
        const pipeline::M2Model gryphon = pipeline::M2Loader::load(m2Bytes);
        REQUIRE_FALSE(gryphon.sequences.empty());

        size_t totalWingFlaps = 0;
        size_t totalWingGlides = 0;
        for (const auto& seq : gryphon.wingFlapEventTimes) totalWingFlaps += seq.size();
        for (const auto& seq : gryphon.wingGlideEventTimes) totalWingGlides += seq.size();
        CHECK(totalWingFlaps > 0);
        CHECK(totalWingGlides > 0);

        // CreatureDisplayInfo 141 authors WingFlap (433) in TBC CreatureSoundData.dbc
        audio::MountSoundManager mount;
        REQUIRE(mount.initialize(&tbc.assets));
        mount.onMount(141, /*isFlying=*/true, "Creature\\Gryphon\\Gryphon.m2");
        events.clear();
        mount.playWingFlapSound();
        CHECK_FALSE(events.empty());
        mount.shutdown();
    }

    SECTION("CombatSoundManager plays WeaponImpactSounds, SheatheSoundLookups, and Ranged shots") {
        audio::CombatSoundManager combat;
        REQUIRE(combat.initialize(&tbc.assets));

        for (uint32_t subClass : {0u, 1u, 4u, 7u, 10u, 13u, 15u}) {
            INFO("Weapon subclass: " << subClass);
            events.clear();
            combat.playWeaponImpactBySubclass(subClass, /*metalWeapon=*/true,
                                              audio::CombatSoundManager::ImpactType::FLESH, false);
            CHECK_FALSE(events.empty());

            events.clear();
            combat.playWeaponImpactBySubclass(subClass, /*metalWeapon=*/true,
                                              audio::CombatSoundManager::ImpactType::PLATE, true);
            CHECK_FALSE(events.empty());

            events.clear();
            combat.playWeaponImpactBySubclass(subClass, /*metalWeapon=*/true,
                                              audio::CombatSoundManager::ImpactType::SHIELD, false);
            CHECK_FALSE(events.empty());
        }

        // Parry weapon clash (1H Sword = subclass 7, metal = true)
        events.clear();
        combat.playWeaponImpactBySubclass(7u, /*metalWeapon=*/true,
                                          audio::CombatSoundManager::ImpactType::METAL_WEAPON, false);
        CHECK_FALSE(events.empty());

        // Sheathe / Unsheathe: Metal sword, Wood staff, Shield
        events.clear();
        CHECK(combat.playSheathe(2, 7, /*material=*/1, /*isShield=*/false, /*unsheathe=*/true));
        CHECK(combat.playSheathe(2, 7, /*material=*/1, /*isShield=*/false, /*unsheathe=*/false));
        CHECK(combat.playSheathe(2, 10, /*material=*/2, /*isShield=*/false, /*unsheathe=*/true));
        CHECK(combat.playSheathe(4, 6, /*material=*/1, /*isShield=*/true, /*unsheathe=*/true));
        CHECK(events.size() == 4);

        // Ranged shots: Bow (2), Gun (3), Thrown (16), Crossbow (18), Wand (19)
        for (uint32_t rangedSub : {2u, 3u, 16u, 18u, 19u}) {
            INFO("Ranged subclass: " << rangedSub);
            events.clear();
            CHECK(combat.playRangedShot(rangedSub));
            CHECK_FALSE(events.empty());
        }

        combat.shutdown();
    }

    SECTION("SpellVisualKit col 15 SoundID and GameObjectDisplayInfo sound columns resolve real TBC audio") {
        auto kitDbc = tbc.assets.loadDBC("SpellVisualKit.dbc");
        REQUIRE(kitDbc != nullptr);
        REQUIRE(kitDbc->isLoaded());
        REQUIRE(kitDbc->getFieldCount() > 15);

        auto& table = audio::SoundEntryTable::instance();
        for (uint32_t kitId : {213u, 282u, 362u, 402u, 1027u, 9350u, 9357u}) {
            INFO("SpellVisualKit ID: " << kitId);
            const int32_t row = kitDbc->findRecordById(kitId);
            REQUIRE(row >= 0);
            const uint32_t soundId = kitDbc->getUInt32(static_cast<uint32_t>(row), 15);
            REQUIRE(soundId != 0);
            events.clear();
            CHECK(table.play2D(&tbc.assets, soundId, 1.0f));
            CHECK_FALSE(events.empty());
        }

        auto godi = tbc.assets.loadDBC("GameObjectDisplayInfo.dbc");
        REQUIRE(godi != nullptr);
        REQUIRE(godi->isLoaded());
        REQUIRE(godi->getFieldCount() > 11);
        int verifiedGoSounds = 0;
        for (uint32_t r = 0; r < godi->getRecordCount() && verifiedGoSounds < 12; ++r) {
            const uint32_t openSound = godi->getUInt32(r, 3);
            if (openSound == 0) continue;
            events.clear();
            if (table.play3D(&tbc.assets, openSound, glm::vec3(0.0f, 0.0f, -5.0f), 1.0f)) {
                CHECK_FALSE(events.empty());
                ++verifiedGoSounds;
            }
        }
        CHECK(verifiedGoSounds >= 8);
    }

    SECTION("NpcVoiceManager, PlayerVoiceManager, EmotesTextSound, and ActivitySoundManager cover TBC races") {
        audio::NpcVoiceManager npc;
        REQUIRE(npc.initialize(&tbc.assets));

        // Find a TBC display with NPCSounds (CreatureDisplayInfo col 12) and one with CreatureSoundData Death (col 6)
        auto cdi = tbc.assets.loadDBC("CreatureDisplayInfo.dbc");
        REQUIRE(cdi != nullptr);
        uint32_t npcSoundDisplay = 0;
        for (uint32_t r = 0; r < cdi->getRecordCount(); ++r) {
            if (cdi->getUInt32(r, 12) != 0) {
                npcSoundDisplay = cdi->getUInt32(r, 0);
                break;
            }
        }
        REQUIRE(npcSoundDisplay != 0);
        events.clear();
        npc.playGreeting(0xF1300001ull, audio::VoiceType::HUMAN_MALE, glm::vec3(0.0f, 0.0f, -4.0f), npcSoundDisplay);
        CHECK_FALSE(events.empty());

        // Display 2404 (Horse) has Injury / Death in CreatureSoundData
        events.clear();
        npc.playWound(0xF1300002ull, 2404u, /*isCrit=*/true, glm::vec3(0.0f, 0.0f, -4.0f));
        CHECK_FALSE(events.empty());
        events.clear();
        npc.playDeath(0xF1300002ull, 2404u, glm::vec3(0.0f, 0.0f, -4.0f));
        CHECK_FALSE(events.empty());
        npc.shutdown();

        // PlayerVoiceManager: all 10 TBC playable races (1..8, 10=BloodElf, 11=Draenei) x 2 genders
        audio::PlayerVoiceManager pvm;
        REQUIRE(pvm.initialize(&tbc.assets));

        // EmotesText.dbc ID 60 = LAUGH ("laugh") or 20 = BEG / 54 = HELLO / 24 = CHEER; probe EmotesTextSound directly
        auto ets = tbc.assets.loadDBC("EmotesTextSound.dbc");
        REQUIRE(ets != nullptr);
        REQUIRE(ets->isLoaded());
        uint32_t sampleEmoteId = ets->getUInt32(0, 1);
        REQUIRE(sampleEmoteId != 0);

        for (uint8_t raceId : {1, 2, 3, 4, 5, 6, 7, 8, 10, 11}) {
            for (uint8_t gender : {0, 1}) {
                INFO("Race=" << int(raceId) << " Gender=" << int(gender));
                // Emote vocalization (find first EmotesTextId authored for this race+gender)
                uint32_t raceEmoteId = 0;
                for (uint32_t r = 0; r < ets->getRecordCount(); ++r) {
                    if (ets->getUInt32(r, 2) == raceId && ets->getUInt32(r, 3) == gender &&
                        ets->getUInt32(r, 4) != 0) {
                        raceEmoteId = ets->getUInt32(r, 1);
                        break;
                    }
                }
                REQUIRE(raceEmoteId != 0);
                events.clear();
                CHECK(pvm.playEmote(raceEmoteId, raceId, gender));
                CHECK_FALSE(events.empty());
                events.clear();
                CHECK(pvm.playEmote3D(raceEmoteId, raceId, gender, glm::vec3(3.0f, 0.0f, -3.0f)));
                CHECK_FALSE(events.empty());
            }
        }
        pvm.shutdown();

        // ActivitySoundManager: all 10 TBC playable races including Blood Elf & Draenei wound + death vocals
        audio::ActivitySoundManager act;
        REQUIRE(act.initialize(&tbc.assets));
        for (const char* model : {
                 "Character\\Human\\Male\\HumanMale.m2",
                 "Character\\Human\\Female\\HumanFemale.m2",
                 "Character\\Orc\\Male\\OrcMale.m2",
                 "Character\\Orc\\Female\\OrcFemale.m2",
                 "Character\\Dwarf\\Male\\DwarfMale.m2",
                 "Character\\NightElf\\Female\\NightElfFemale.m2",
                 "Character\\Scourge\\Female\\ScourgeFemale.m2",
                 "Character\\Tauren\\Male\\TaurenMale.m2",
                 "Character\\Gnome\\Female\\GnomeFemale.m2",
                 "Character\\Troll\\Female\\TrollFemale.m2",
                 "Character\\BloodElf\\Male\\BloodElfMale.m2",
                 "Character\\BloodElf\\Female\\BloodElfFemale.m2",
                 "Character\\Draenei\\Male\\DraeneiMale.m2",
                 "Character\\Draenei\\Female\\DraeneiFemale.m2",
             }) {
            INFO("Character model: " << model);
            act.setCharacterVoiceProfile(model);
            events.clear();
            act.playWound(/*isCrit=*/true);
            CHECK_FALSE(events.empty());
            events.clear();
            act.playDeath();
            CHECK_FALSE(events.empty());
        }
        // Vanilla races also resolve PlayerExertions hard-landing vocals
        act.setCharacterVoiceProfile("Character\\Dwarf\\Female\\DwarfFemale.m2");
        events.clear();
        act.playLanding(/*hardLanding=*/true);
        CHECK_FALSE(events.empty());
        act.shutdown();
    }

    engine.setPlaybackObserver({});
}
