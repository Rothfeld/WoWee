// TBC 2.4.3 (Wow.exe build 8606) Audio Parity Shard A unit tests:
// - GAP-01: SESound::ComputeAttenuation (0x756bd0: rolloff=4.0, [0.9, 1.0]*maxDist fade-to-zero),
//           pre-spawn DistanceCutoff^2 cull (0x461c10), Sound_ListenerAtCharacter default true
//           (0x467ec6), +2.0f Z-offset (0x892b98), and 0.65f (0x89748c) self-2D vs camera-3D rule.
// - GAP-02: SoundEntries.dbc Flags (col 25): 0x020 NO_DUPLICATES, 0x200 LOOPING across all 30
//           looping spell SoundEntries + creature loops, 0x400 VARY_PITCH [0.85, 1.15],
//           0x800 VARY_VOLUME [-0.15, +0.15], 0x897018 per-category concurrency caps, and
//           Sound_EnableSoundWhenGameIsInBG unfocused SFX gate (0x4621ef -> 0x4664b0).
//
// Run with:
//   WOWEE_TBC_DATA_DIR=/workspace/azerothcore/client/wowee-data/expansions/tbc ./build/bin/test_audio_shard_a_engine

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <catch_amalgamated.hpp>

#include "audio/audio_coordinator.hpp"
#include "audio/audio_engine.hpp"
#include "audio/sound_entries.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/dbc_loader.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using namespace wowee;

namespace {

struct ShardATestEnv {
    pipeline::AssetManager assets;
    pipeline::DBCLayout dbcLayout;
    bool ready = false;

    ShardATestEnv() {
        (void)audio::AudioEngine::instance();
        (void)audio::SoundEntryTable::instance();

        const char* envRoot = std::getenv("WOWEE_TBC_DATA_DIR");
        const std::string root = (envRoot && *envRoot)
            ? std::string(envRoot)
            : "/workspace/azerothcore/client/wowee-data/expansions/tbc";
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

    ~ShardATestEnv() {
        audio::SoundEntryTable::instance().reset();
        audio::AudioEngine::instance().setPlaybackObserver({});
        audio::AudioEngine::instance().setAssetManager(nullptr);
        audio::AudioEngine::instance().shutdown();
        pipeline::setActiveDBCLayout(nullptr);
        assets.shutdown();
    }
};

ShardATestEnv& env() {
    static ShardATestEnv instance;
    if (!instance.ready) {
        SKIP("TBC 2.4.3 data directory unavailable; set WOWEE_TBC_DATA_DIR");
    }
    return instance;
}

double renderTotalRms(uint32_t frames) {
    std::vector<float> pcm(static_cast<size_t>(frames) * 2u, 0.0f);
    const uint32_t got = audio::AudioEngine::instance().renderMixFrames(pcm.data(), frames);
    REQUIRE(got == frames);
    double sum = 0.0;
    for (float sample : pcm) {
        REQUIRE(std::isfinite(sample));
        sum += static_cast<double>(sample) * static_cast<double>(sample);
    }
    return std::sqrt(sum / std::max<size_t>(1u, pcm.size()));
}

} // namespace

TEST_CASE("GAP-01: SESound::ComputeAttenuation (0x756bd0) and DistanceCutoff^2 pre-spawn cull (0x461c10)",
          "[audio][shard_a][gap01]") {
    auto& tbc = env();
    auto& engine = audio::AudioEngine::instance();

    SECTION("Closed-form 0x756bd0 curve matches Wow.exe build 8606 constants") {
        constexpr float minD = 8.0f;
        constexpr float maxD = 45.0f;

        CHECK(audio::AudioEngine::computeDistanceAttenuation(minD, maxD, 0.0f) == Catch::Approx(1.0f));
        CHECK(audio::AudioEngine::computeDistanceAttenuation(minD, maxD, 8.0f) == Catch::Approx(1.0f));

        // Mid-range d = 25 yd: 8 / (8 + 4 * 17) = 8 / 76 = 0.10526316 (vs legacy rolloff=1.0 -> 0.32)
        CHECK(audio::AudioEngine::computeDistanceAttenuation(minD, maxD, 25.0f) ==
              Catch::Approx(8.0f / 76.0f).epsilon(1e-5f));

        // Exactly at fadeStart = 0.9 * 45 = 40.5 yd: 8 / (8 + 4 * 32.5) = 8 / 138
        CHECK(audio::AudioEngine::computeDistanceAttenuation(minD, maxD, 40.5f) ==
              Catch::Approx(8.0f / 138.0f).epsilon(1e-5f));

        // Halfway through outer-10% linear fade (d = 42.75 yd -> fadeFactor = 0.5)
        const float expectedMidFade = (8.0f / (8.0f + 4.0f * (42.75f - 8.0f))) * 0.5f;
        CHECK(audio::AudioEngine::computeDistanceAttenuation(minD, maxD, 42.75f) ==
              Catch::Approx(expectedMidFade).epsilon(1e-4f));

        // At and beyond maxDistance: exact 0.0
        CHECK(audio::AudioEngine::computeDistanceAttenuation(minD, maxD, 45.0f) == 0.0f);
        CHECK(audio::AudioEngine::computeDistanceAttenuation(minD, maxD, 100.0f) == 0.0f);
        CHECK(audio::AudioEngine::computeDistanceAttenuation(minD, maxD, 500.0f) == 0.0f);
    }

    SECTION("Pre-spawn DistanceCutoff^2 cull prevents channel allocation and PCM synthesis at d >= maxDist") {
        engine.shutdown();
        REQUIRE(engine.initializeHeadless(48000, 2));
        engine.setAssetManager(&tbc.assets);
        engine.setListenerPosition(glm::vec3(0.0f));

        std::vector<audio::AudioEngine::PlaybackEvent> events;
        engine.setPlaybackObserver([&](const audio::AudioEngine::PlaybackEvent& ev) {
            events.push_back(ev);
        });

        auto& table = audio::SoundEntryTable::instance();
        table.reset();
        REQUIRE(table.ensureLoaded(&tbc.assets));

        // SoundEntries 2941 (HumanMalePlayerAttack): minDistance=8.0, distanceCutoff=45.0
        const auto* rec = table.findById(&tbc.assets, 2941u);
        REQUIRE(rec != nullptr);
        CHECK(rec->minDistance == Catch::Approx(8.0f));
        CHECK(rec->maxDistance == Catch::Approx(45.0f));

        // At 45.0 yd and 500.0 yd: culled before channel spawn
        CHECK_FALSE(table.play3D(&tbc.assets, 2941u, glm::vec3(0.0f, -45.0f, 0.0f), 1.0f));
        CHECK(engine.getActiveSoundCount() == 0);
        CHECK_FALSE(engine.playSound3D("Sound\\Interface\\iQuestComplete.wav",
                                       glm::vec3(0.0f, -45.0f, 0.0f), 1.0f, 1.0f, 45.0f, 8.0f));
        REQUIRE_FALSE(events.empty());
        CHECK_FALSE(events.back().started);
        CHECK(engine.getActiveSoundCount() == 0);
        CHECK(renderTotalRms(2048) == Catch::Approx(0.0).margin(1e-7));

        // Synthesized PCM RMS ratio between d = 4 yd (gain = 1.0) and d = 25 yd (gain = 8/76)
        engine.shutdown();
        REQUIRE(engine.initializeHeadless(48000, 2));
        engine.setAssetManager(&tbc.assets);
        engine.setListenerPosition(glm::vec3(0.0f));
        engine.setListenerOrientation(glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        REQUIRE(engine.playSound3D("Sound\\Spells\\PreCastFireLow.wav",
                                   glm::vec3(0.0f, 0.0f, -4.0f), 1.0f, 1.0f, 45.0f, 8.0f));
        const double rmsAt4Yd = renderTotalRms(4096);
        REQUIRE(rmsAt4Yd > 1e-4);

        engine.shutdown();
        REQUIRE(engine.initializeHeadless(48000, 2));
        engine.setAssetManager(&tbc.assets);
        engine.setListenerPosition(glm::vec3(0.0f));
        engine.setListenerOrientation(glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        REQUIRE(engine.playSound3D("Sound\\Spells\\PreCastFireLow.wav",
                                   glm::vec3(0.0f, 0.0f, -25.0f), 1.0f, 1.0f, 45.0f, 8.0f));
        const double rmsAt25Yd = renderTotalRms(4096);
        CHECK(rmsAt25Yd / rmsAt4Yd == Catch::Approx(8.0 / 76.0).epsilon(0.08));

        // Moving listener past maxDistance silences an already-running 3D loop immediately
        engine.shutdown();
        REQUIRE(engine.initializeHeadless(48000, 2));
        engine.setAssetManager(&tbc.assets);
        engine.setListenerPosition(glm::vec3(0.0f));
        const uint32_t loop3d = table.play3DStoppable(&tbc.assets, 20u, glm::vec3(0.0f, 0.0f, -5.0f), 1.0f);
        REQUIRE(loop3d != 0);
        CHECK(renderTotalRms(2048) > 1e-4);
        engine.setListenerPosition(glm::vec3(0.0f, 0.0f, 200.0f));
        CHECK(renderTotalRms(2048) == Catch::Approx(0.0).margin(1e-6));
        engine.setSoundPosition(loop3d, glm::vec3(0.0f, 0.0f, 196.0f));
        CHECK(renderTotalRms(2048) > 1e-4);
        engine.stopSound(loop3d);
        engine.setPlaybackObserver({});
    }
}

TEST_CASE("GAP-01: Sound_ListenerAtCharacter default, +2.0f Z-offset, and 0.65f local-player rule",
          "[audio][shard_a][gap01]") {
    auto& tbc = env();
    auto& engine = audio::AudioEngine::instance();
    engine.shutdown();
    REQUIRE(engine.initializeHeadless(48000, 2));
    engine.setAssetManager(&tbc.assets);

    auto& table = audio::SoundEntryTable::instance();
    table.reset();
    REQUIRE(table.ensureLoaded(&tbc.assets));

    audio::AudioCoordinator coord;
    // Wow.exe 0x467ec6: Sound_ListenerAtCharacter defaults to "1" (true)
    CHECK(coord.isListenerAtCharacter());
    CHECK_FALSE(coord.isBackgroundSoundEnabled());

    const glm::vec3 charBase(10.0f, 20.0f, 30.0f);
    const glm::vec3 camPos(10.0f, 45.0f, 42.0f);
    // Wow.exe 0x46657d / 0x892b98: +2.0f Z-offset when at character
    const glm::vec3 atChar = coord.listenerPositionFor(charBase, camPos, /*hasCharacter=*/true);
    CHECK(atChar.x == Catch::Approx(10.0f));
    CHECK(atChar.y == Catch::Approx(20.0f));
    CHECK(atChar.z == Catch::Approx(32.0f));

    coord.setListenerAtCharacter(false);
    const glm::vec3 atCam = coord.listenerPositionFor(charBase, camPos, /*hasCharacter=*/true);
    CHECK(atCam.x == Catch::Approx(camPos.x));
    CHECK(atCam.y == Catch::Approx(camPos.y));
    CHECK(atCam.z == Catch::Approx(camPos.z));

    std::vector<audio::AudioEngine::PlaybackEvent> events;
    engine.setPlaybackObserver([&](const audio::AudioEngine::PlaybackEvent& ev) {
        events.push_back(ev);
    });

    // SoundEntries 888 (LEVELUP, volume=1.0, no VARY_VOLUME)
    const auto* rec = table.findById(&tbc.assets, 888u);
    REQUIRE(rec != nullptr);

    // 1. Local player + ListenerAtCharacter == true -> 2D at 0.65f * rec->volume
    coord.setListenerAtCharacter(true);
    events.clear();
    REQUIRE(coord.playUnitSound(&tbc.assets, 888u, charBase, /*isLocalPlayer=*/true, 1.0f));
    REQUIRE(events.size() == 1);
    CHECK_FALSE(events.back().spatial);
    CHECK(events.back().volume == Catch::Approx(rec->volume * 0.65f).epsilon(1e-4f));

    // Mono 2D equal-power center pan law (FMOD parity: 1/sqrt(2) = -3.01 dB per L/R speaker)
    {
        auto makeToneWav = [](uint16_t channels) {
            constexpr uint32_t kRate = 48000;
            constexpr uint32_t kFrames = 4096;
            const uint32_t dataBytes = kFrames * channels * 2;
            std::vector<uint8_t> wav(44 + dataBytes, 0);
            auto put32 = [&](size_t off, uint32_t v) {
                wav[off] = v & 0xFF; wav[off+1] = (v >> 8) & 0xFF;
                wav[off+2] = (v >> 16) & 0xFF; wav[off+3] = (v >> 24) & 0xFF;
            };
            auto put16 = [&](size_t off, uint16_t v) {
                wav[off] = v & 0xFF; wav[off+1] = (v >> 8) & 0xFF;
            };
            std::memcpy(wav.data(), "RIFF", 4); put32(4, 36 + dataBytes);
            std::memcpy(wav.data() + 8, "WAVEfmt ", 8); put32(16, 16); put16(20, 1);
            put16(22, channels); put32(24, kRate); put32(28, kRate * channels * 2);
            put16(32, channels * 2); put16(34, 16);
            std::memcpy(wav.data() + 36, "data", 4); put32(40, dataBytes);
            for (uint32_t i = 0; i < kFrames; ++i) {
                const int16_t s = static_cast<int16_t>(16000.0 * std::sin(2.0 * 3.14159265358979323846 * 440.0 * i / kRate));
                for (uint16_t ch = 0; ch < channels; ++ch) {
                    put16(44 + (i * channels + ch) * 2, static_cast<uint16_t>(s));
                }
            }
            return wav;
        };
        const auto monoWav = makeToneWav(1);
        const auto stereoWav = makeToneWav(2);
        engine.shutdown();
        REQUIRE(engine.initializeHeadless(48000, 2));
        REQUIRE(engine.playSound2D(monoWav, 0.5f, 1.0f));
        const double monoRms = renderTotalRms(2048);
        engine.shutdown();
        REQUIRE(engine.initializeHeadless(48000, 2));
        REQUIRE(engine.playSound2D(stereoWav, 0.5f, 1.0f));
        const double stereoRms = renderTotalRms(2048);
        REQUIRE(stereoRms > 1e-3);
        CHECK(monoRms / stereoRms == Catch::Approx(0.70710678).epsilon(0.02));
        engine.shutdown();
        REQUIRE(engine.initializeHeadless(48000, 2));
        engine.setPlaybackObserver([&](const audio::AudioEngine::PlaybackEvent& ev) {
            events.push_back(ev);
        });
    }

    // 2. Local player + ListenerAtCharacter == false -> 3D at charBase + (0, 0, 2.0f)
    coord.setListenerAtCharacter(false);
    engine.setListenerPosition(camPos);
    events.clear();
    REQUIRE(coord.playUnitSound(&tbc.assets, 888u, charBase, /*isLocalPlayer=*/true, 1.0f));
    REQUIRE(events.size() == 1);
    CHECK(events.back().spatial);
    CHECK(events.back().position.x == Catch::Approx(charBase.x));
    CHECK(events.back().position.y == Catch::Approx(charBase.y));
    CHECK(events.back().position.z == Catch::Approx(charBase.z + 2.0f));
    CHECK(events.back().volume == Catch::Approx(rec->volume).epsilon(1e-4f));

    // 3. Remote unit -> always 3D at unitBasePos + (0, 0, 2.0f) even when ListenerAtCharacter == true
    coord.setListenerAtCharacter(true);
    engine.setListenerPosition(atChar);
    const glm::vec3 npcBase(14.0f, 22.0f, 30.0f);
    events.clear();
    REQUIRE(coord.playUnitSound(&tbc.assets, 888u, npcBase, /*isLocalPlayer=*/false, 1.0f));
    REQUIRE(events.size() == 1);
    CHECK(events.back().spatial);
    CHECK(events.back().position.z == Catch::Approx(npcBase.z + 2.0f));

    engine.setPlaybackObserver({});
}

TEST_CASE("GAP-02: SoundEntries Flags (0x020, 0x200, 0x400, 0x800), 0x897018 caps, and BG mute",
          "[audio][shard_a][gap02]") {
    auto& tbc = env();
    auto& engine = audio::AudioEngine::instance();
    auto& table = audio::SoundEntryTable::instance();
    table.reset();
    REQUIRE(table.ensureLoaded(&tbc.assets));

    SECTION("0x200 LOOPING loops all TBC spell/creature loop SoundEntries past buffer end until stopped") {
        auto dbc = tbc.assets.loadDBC("SoundEntries.dbc");
        REQUIRE(dbc != nullptr);
        REQUIRE(dbc->isLoaded());

        // Verify all looping spell SoundEntries (SoundType == 1 && (Flags & 0x200)) in TBC DBC
        std::vector<uint32_t> loopingSpellIds;
        for (uint32_t r = 0; r < dbc->getRecordCount(); ++r) {
            const uint32_t id = dbc->getUInt32(r, 0);
            const uint32_t st = dbc->getUInt32(r, 1);
            const uint32_t fl = dbc->getUInt32(r, 25);
            if (st == 1u && (fl & audio::SoundEntryFlags::kLooping) != 0u && table.findById(&tbc.assets, id)) {
                loopingSpellIds.push_back(id);
            }
        }
        CHECK(loopingSpellIds.size() >= 30u);

        for (uint32_t sid : loopingSpellIds) {
            engine.shutdown();
            REQUIRE(engine.initializeHeadless(48000, 2));
            engine.setAssetManager(&tbc.assets);
            const uint32_t h = table.play2DStoppable(&tbc.assets, sid, 1.0f);
            INFO("Looping spell SoundEntries ID=" << sid);
            REQUIRE(h != 0u);
            CHECK(engine.isSoundLooping(h));
            engine.stopSound(h);
            CHECK_FALSE(engine.isSoundPlaying(h));
        }

        // Deep PCM wrap verification on Frost Precast (ID 20) & FireElementalLoop (ID 1454):
        // render 3.5x decoded frame count and confirm continuous non-zero synthesis until stopSound().
        for (uint32_t sid : {20u, 1454u}) {
            engine.shutdown();
            REQUIRE(engine.initializeHeadless(48000, 2));
            engine.setAssetManager(&tbc.assets);
            engine.setListenerPosition(glm::vec3(0.0f));

            uint64_t singleFrames = 0;
            engine.setPlaybackObserver([&](const audio::AudioEngine::PlaybackEvent& ev) {
                if (ev.started) singleFrames = ev.decodedFrames;
            });

            const uint32_t h = table.play3DStoppable(&tbc.assets, sid, glm::vec3(0.0f, 0.0f, -3.0f), 1.0f);
            REQUIRE(h != 0u);
            REQUIRE(singleFrames > 1000u);
            CHECK(engine.isSoundLooping(h));

            const uint64_t targetFrames = singleFrames * 3u + singleFrames / 2u;
            uint64_t rendered = 0;
            while (rendered < targetFrames) {
                (void)renderTotalRms(4096);
                rendered += 4096u;
            }
            CHECK(engine.isSoundPlaying(h));
            CHECK(renderTotalRms(4096) > 1e-4);

            engine.stopSound(h);
            CHECK_FALSE(engine.isSoundPlaying(h));
            CHECK(renderTotalRms(2048) == Catch::Approx(0.0).margin(1e-6));
            engine.setPlaybackObserver({});
        }

        // Non-stoppable play2D on a 0x200 SoundEntry must NOT leak an un-stoppable infinite loop
        engine.shutdown();
        REQUIRE(engine.initializeHeadless(48000, 2));
        engine.setAssetManager(&tbc.assets);
        uint64_t nonStopFrames = 0;
        uint32_t nonStopRate = 48000u;
        engine.setPlaybackObserver([&](const audio::AudioEngine::PlaybackEvent& ev) {
            if (ev.started) {
                nonStopFrames = ev.decodedFrames;
                nonStopRate = std::max(1u, ev.sampleRate);
            }
        });
        REQUIRE(table.play2D(&tbc.assets, 20u, 1.0f));
        REQUIRE(nonStopFrames > 1000u);
        const uint64_t outFramesNeeded = (nonStopFrames * 48000ull) / nonStopRate + 24000ull;
        uint64_t drained = 0;
        while (drained < outFramesNeeded) {
            (void)renderTotalRms(4096);
            drained += 4096u;
        }
        CHECK(engine.getActiveSoundCount() == 0u);
        engine.setPlaybackObserver({});
    }

    SECTION("0x020 NO_DUPLICATES suppresses duplicate active SoundEntry ID and re-enables after completion") {
        engine.shutdown();
        REQUIRE(engine.initializeHeadless(48000, 2));
        engine.setAssetManager(&tbc.assets);

        // Find real TBC SoundEntries row with 0x020 (NO_DUPLICATES) and existing WAV
        auto dbc = tbc.assets.loadDBC("SoundEntries.dbc");
        REQUIRE(dbc != nullptr);
        uint32_t noDupId = 0;
        for (uint32_t r = 0; r < dbc->getRecordCount(); ++r) {
            const uint32_t id = dbc->getUInt32(r, 0);
            const uint32_t fl = dbc->getUInt32(r, 25);
            const auto* rec = table.findById(&tbc.assets, id);
            if (rec && (fl & audio::SoundEntryFlags::kNoDuplicates) != 0u &&
                !table.pickExistingPath(&tbc.assets, *rec).empty()) {
                noDupId = id;
                break;
            }
        }
        REQUIRE(noDupId != 0u);

        const uint32_t h1 = table.play2DStoppable(&tbc.assets, noDupId, 1.0f);
        REQUIRE(h1 != 0u);
        CHECK(engine.isSoundEntryPlaying(noDupId));
        // Second concurrent start of same SoundEntries ID must be rejected (0x46224c)
        CHECK_FALSE(table.play2D(&tbc.assets, noDupId, 1.0f));
        CHECK(table.play2DStoppable(&tbc.assets, noDupId, 1.0f) == 0u);

        // Stopping h1 immediately releases the NO_DUPLICATES lock
        engine.stopSound(h1);
        CHECK_FALSE(engine.isSoundEntryPlaying(noDupId));
        const uint32_t h2 = table.play2DStoppable(&tbc.assets, noDupId, 1.0f);
        CHECK(h2 != 0u);
        engine.stopSound(h2);
    }

    SECTION("0x400 VARY_PITCH [0.85, 1.15] and 0x800 VARY_VOLUME [-0.15, +0.15] match Wow.exe ranges") {
        engine.shutdown();
        REQUIRE(engine.initializeHeadless(48000, 2));
        engine.setAssetManager(&tbc.assets);

        // Real TBC 0x400 (VARY_PITCH) entry: 560 = CharacterMediumLargeDirt (87 such rows in TBC SoundEntries.dbc)
        const auto* pitchRec = table.findById(&tbc.assets, 560u);
        REQUIRE(pitchRec != nullptr);
        CHECK((pitchRec->flags & audio::SoundEntryFlags::kVaryPitch) != 0u);

        // Synthetic 0x800 (VARY_VOLUME) record backed by real TBC WAV (Wow.exe 0x462515..0x462558)
        constexpr uint32_t kSyntheticVaryVolId = 999901u;
        audio::SoundEntryRecord syntheticVolRec;
        syntheticVolRec.id = kSyntheticVaryVolId;
        syntheticVolRec.soundType = 1u;
        syntheticVolRec.name = "SyntheticVaryVolumeTest";
        syntheticVolRec.variants.push_back(audio::SoundVariant{
            .path = "Sound\\Interface\\iQuestComplete.wav",
            .weight = 1u,
        });
        syntheticVolRec.volume = 0.60f;
        syntheticVolRec.flags = audio::SoundEntryFlags::kVaryVolume;
        table.upsertRecordForTest(syntheticVolRec);

        std::vector<float> observedPitches;
        std::vector<float> observedVolumes;
        engine.setPlaybackObserver([&](const audio::AudioEngine::PlaybackEvent& ev) {
            if (ev.started) {
                observedPitches.push_back(ev.pitch);
                observedVolumes.push_back(ev.volume);
            }
        });

        table.seedRngForTest(1337u);
        observedPitches.clear();
        for (int i = 0; i < 24; ++i) {
            const uint32_t h = table.play2DStoppable(&tbc.assets, 560u, 1.0f, 1.0f);
            REQUIRE(h != 0u);
            engine.stopSound(h);
        }
        REQUIRE(observedPitches.size() == 24u);
        float minP = observedPitches.front();
        float maxP = observedPitches.front();
        for (float p : observedPitches) {
            CHECK(p >= 0.8499f);
            CHECK(p <= 1.1501f);
            minP = std::min(minP, p);
            maxP = std::max(maxP, p);
        }
        CHECK(maxP - minP > 0.05f);

        table.seedRngForTest(4242u);
        observedVolumes.clear();
        for (int i = 0; i < 24; ++i) {
            const uint32_t h = table.play2DStoppable(&tbc.assets, kSyntheticVaryVolId, 1.0f, 1.0f);
            REQUIRE(h != 0u);
            engine.stopSound(h);
        }
        REQUIRE(observedVolumes.size() == 24u);
        float minV = observedVolumes.front();
        float maxV = observedVolumes.front();
        const float loBound = std::clamp(0.60f - 0.1501f, 0.0f, 1.0f);
        const float hiBound = std::clamp(0.60f + 0.1501f, 0.0f, 1.0f);
        for (float v : observedVolumes) {
            CHECK(v >= loBound);
            CHECK(v <= hiBound);
            minV = std::min(minV, v);
            maxV = std::max(maxV, v);
        }
        CHECK(maxV - minV > 0.05f);
        engine.setPlaybackObserver({});
        table.reset();
        REQUIRE(table.ensureLoaded(&tbc.assets));
    }

    SECTION("0x897018 per-category concurrency caps enforce VocalUI=1, WeaponSwing=2, CombatImpact=4, Footstep=6") {
        CHECK(audio::AudioEngine::maxConcurrentForCategory(audio::SESoundCategory::kSfx) == 0x7FFFFFFFu);
        CHECK(audio::AudioEngine::maxConcurrentForCategory(audio::SESoundCategory::kRacialCinematic) == 1u);
        CHECK(audio::AudioEngine::maxConcurrentForCategory(audio::SESoundCategory::kVocalUi) == 1u);
        CHECK(audio::AudioEngine::maxConcurrentForCategory(audio::SESoundCategory::kItemPickup) == 2u);
        CHECK(audio::AudioEngine::maxConcurrentForCategory(audio::SESoundCategory::kNpcGreeting) == 1u);
        CHECK(audio::AudioEngine::maxConcurrentForCategory(audio::SESoundCategory::kWeaponSwing) == 2u);
        CHECK(audio::AudioEngine::maxConcurrentForCategory(audio::SESoundCategory::kDeathThud) == 2u);
        CHECK(audio::AudioEngine::maxConcurrentForCategory(audio::SESoundCategory::kVocalEmote) == 1u);
        CHECK(audio::AudioEngine::maxConcurrentForCategory(audio::SESoundCategory::kFootstepOther) == 6u);
        CHECK(audio::AudioEngine::maxConcurrentForCategory(audio::SESoundCategory::kCombatImpact) == 4u);
        CHECK(audio::AudioEngine::maxConcurrentForCategory(audio::SESoundCategory::kSpellFizzle) == 1u);
        CHECK(audio::AudioEngine::maxConcurrentForCategory(audio::SESoundCategory::kCombatVocal) == 2u);
        CHECK(audio::AudioEngine::maxConcurrentForCategory(audio::SESoundCategory::kFootstepSelf) == 4u);

        engine.shutdown();
        REQUIRE(engine.initializeHeadless(48000, 2));
        engine.setAssetManager(&tbc.assets);

        // WeaponSwingSounds2: SoundEntries 233 (LightWeaponNormal) & 235 (MediumWeaponNormal), SoundType=6 -> cap=2
        const uint32_t sw1 = table.play2DStoppable(&tbc.assets, 233u, 1.0f);
        const uint32_t sw2 = table.play2DStoppable(&tbc.assets, 235u, 1.0f);
        REQUIRE(sw1 != 0u);
        REQUIRE(sw2 != 0u);
        CHECK(table.play2DStoppable(&tbc.assets, 237u, 1.0f) == 0u);
        engine.stopSound(sw1);
        const uint32_t sw3 = table.play2DStoppable(&tbc.assets, 237u, 1.0f);
        CHECK(sw3 != 0u);
        engine.stopSound(sw2);
        engine.stopSound(sw3);
    }

    SECTION("Sound_EnableSoundWhenGameIsInBG gates unfocused one-shot SFX while preserving loops") {
        engine.shutdown();
        REQUIRE(engine.initializeHeadless(48000, 2));
        engine.setAssetManager(&tbc.assets);

        engine.setBackgroundSoundEnabled(false);
        engine.setWindowFocused(false);
        CHECK(engine.isMutedInBackground());

        // One-shot SFX while unfocused and BG sound disabled -> rejected without allocating channel
        CHECK_FALSE(table.play2D(&tbc.assets, 888u, 1.0f));
        CHECK(engine.getActiveSoundCount() == 0u);

        // Stoppable looping SFX starts silently so refocusing hears the ongoing loop
        const uint32_t loopH = table.play2DStoppable(&tbc.assets, 20u, 1.0f);
        REQUIRE(loopH != 0u);
        CHECK(renderTotalRms(2048) == Catch::Approx(0.0).margin(1e-6));

        engine.setWindowFocused(true);
        CHECK_FALSE(engine.isMutedInBackground());
        CHECK(renderTotalRms(2048) > 1e-4);
        engine.stopSound(loopH);

        // Enabling Sound_EnableSoundWhenGameIsInBG allows unfocused one-shots to play
        engine.setBackgroundSoundEnabled(true);
        engine.setWindowFocused(false);
        CHECK_FALSE(engine.isMutedInBackground());
        CHECK(table.play2D(&tbc.assets, 888u, 1.0f));
        CHECK(renderTotalRms(2048) > 1e-4);
    }
}
