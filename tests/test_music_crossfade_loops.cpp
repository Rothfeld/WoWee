// Wave-2 Audio Side-Discovery Parity Suite:
// 1. Native TBC 2.4.3 music bus gain law (0.6081 * SoundEntries.vol * slider, PlayMusic vs ZoneMusic)
// 2. Simultaneous dual-decoder overlapping crossfade (zero -120 dBFS silence gap)
// 3. Sound_EnableMusic=0 / MusicVolume=0 stream & trace gate + immediate resume
// 4. 3D DistanceCutoff pre-cull and M2 $DSO (ForceOneShot, e.g. SE:3431) vs $DSL (ForceLoop, e.g. SE:3246)

#include <catch_amalgamated.hpp>

#include "audio/audio_coordinator.hpp"
#include "audio/audio_engine.hpp"
#include "audio/footstep_manager.hpp"
#include "audio/music_manager.hpp"
#include "audio/sound_entries.hpp"
#include "game/zone_manager.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/m2_loader.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <string>
#include <vector>

using namespace wowee;

namespace {

struct TbcAssets {
    pipeline::AssetManager assets;
    pipeline::DBCLayout layouts;
    bool ready = false;

    TbcAssets() {
        (void)audio::AudioEngine::instance();
        (void)audio::SoundEntryTable::instance();

        namespace fs = std::filesystem;
        std::vector<std::string> candidates;
        if (const char* envPath = std::getenv("WOWEE_TBC_DATA_DIR"); envPath && *envPath) {
            candidates.emplace_back(envPath);
        }
        candidates.emplace_back("/workspace/azerothcore/client/wowee-data/expansions/tbc");
        candidates.emplace_back("/workspace/azerothcore/client/wowee-data");

        const fs::path layoutPath = fs::path(WOWEE_SOURCE_DIR) / "Data" / "expansions" / "tbc" / "dbc_layouts.json";
        if (fs::exists(layoutPath) && layouts.loadFromJson(layoutPath.string())) {
            pipeline::setActiveDBCLayout(&layouts);
        }

        for (const auto& dir : candidates) {
            if (fs::exists(dir) && assets.initialize(dir)) {
                assets.setExpansionDataPath(dir);
                if (auto dbc = assets.loadDBC("SoundEntries.dbc"); dbc && dbc->isLoaded()) {
                    ready = true;
                    break;
                }
            }
        }
    }

    ~TbcAssets() {
        audio::SoundEntryTable::instance().reset();
        audio::AudioEngine::instance().setPlaybackObserver({});
        audio::AudioEngine::instance().setAssetManager(nullptr);
        audio::AudioEngine::instance().shutdown();
        pipeline::setActiveDBCLayout(nullptr);
        assets.shutdown();
    }
};

TbcAssets& tbcEnv() {
    static TbcAssets instance;
    if (!instance.ready) {
        SKIP("TBC data directory with SoundEntries.dbc not available");
    }
    return instance;
}

std::shared_ptr<const std::vector<uint8_t>> makeStereoSineWav(
    float freqHz, float amplitude, uint32_t sampleRate, uint32_t frameCount) {
    const uint32_t channels = 2u;
    const uint32_t bytesPerSample = 2u;
    const uint32_t dataBytes = frameCount * channels * bytesPerSample;
    const uint32_t riffSize = 36u + dataBytes;

    auto buf = std::make_shared<std::vector<uint8_t>>(44u + dataBytes, 0u);
    auto wr16 = [&](size_t off, uint16_t v) { std::memcpy(buf->data() + off, &v, 2); };
    auto wr32 = [&](size_t off, uint32_t v) { std::memcpy(buf->data() + off, &v, 4); };

    std::memcpy(buf->data() + 0, "RIFF", 4);
    wr32(4, riffSize);
    std::memcpy(buf->data() + 8, "WAVEfmt ", 8);
    wr32(16, 16u);
    wr16(20, 1u); // PCM
    wr16(22, static_cast<uint16_t>(channels));
    wr32(24, sampleRate);
    wr32(28, sampleRate * channels * bytesPerSample);
    wr16(32, static_cast<uint16_t>(channels * bytesPerSample));
    wr16(34, 16u);
    std::memcpy(buf->data() + 36, "data", 4);
    wr32(40, dataBytes);

    const double twoPiF = 2.0 * std::numbers::pi * static_cast<double>(freqHz) / static_cast<double>(sampleRate);
    for (uint32_t i = 0; i < frameCount; ++i) {
        const double s = static_cast<double>(amplitude) * std::sin(twoPiF * static_cast<double>(i));
        const auto pcm16 = static_cast<int16_t>(std::clamp(std::lround(s * 32767.0), -32768L, 32767L));
        const size_t base = 44u + static_cast<size_t>(i) * 4u;
        std::memcpy(buf->data() + base, &pcm16, 2);
        std::memcpy(buf->data() + base + 2u, &pcm16, 2);
    }
    return buf;
}

double goertzelAmplitude(const std::vector<float>& stereoPcm, uint32_t sampleRate, double targetHz) {
    const size_t frames = stereoPcm.size() / 2u;
    if (frames == 0) return 0.0;
    const double omega = 2.0 * std::numbers::pi * targetHz / static_cast<double>(sampleRate);
    double sumCos = 0.0;
    double sumSin = 0.0;
    for (size_t i = 0; i < frames; ++i) {
        const double mono = 0.5 * (static_cast<double>(stereoPcm[i * 2u]) +
                                   static_cast<double>(stereoPcm[i * 2u + 1u]));
        const double phase = omega * static_cast<double>(i);
        sumCos += mono * std::cos(phase);
        sumSin += mono * std::sin(phase);
    }
    return (2.0 / static_cast<double>(frames)) * std::hypot(sumCos, sumSin);
}

double stereoRmsDbfs(const std::vector<float>& stereoPcm) {
    if (stereoPcm.empty()) return -240.0;
    double sumSq = 0.0;
    for (float s : stereoPcm) {
        sumSq += static_cast<double>(s) * static_cast<double>(s);
    }
    const double rms = std::sqrt(sumSq / static_cast<double>(stereoPcm.size()));
    return 20.0 * std::log10(rms + 1e-12);
}

} // namespace

TEST_CASE("MusicManager applies Native TBC 2.4.3 music bus gain (0.6081) and per-SoundEntry volume",
          "[audio][tbc][music_gain]") {
    auto& env = tbcEnv();
    auto& engine = audio::AudioEngine::instance();
    engine.shutdown();
    REQUIRE(engine.initializeHeadless(48000, 2));
    engine.setAssetManager(&env.assets);
    engine.setMasterVolume(1.0f);
    engine.setSuspended(false);

    audio::MusicManager music;
    REQUIRE(music.initialize(&env.assets));
    music.setVolume(100);

    // Verify DBC SoundEntries col 24 resolution across ZoneMusic sets with distinct volumes:
    //   SE:2523 Zone-Forest Day        = 0.40 -> 0.40 * 0.6081 = 0.24324
    //   SE:2532 Zone-Stormwind         = 0.69 -> 0.69 * 0.6081 = 0.41959
    //   SE:12291 Zone-QuelDanasStage01 = 0.80 -> 0.80 * 0.6081 = 0.48648
    //   SE:9789 Zone-EversongDay       = 0.90 -> 0.90 * 0.6081 = 0.54729
    //   SE:2902 Zone Music Day Org     = 1.00 -> 1.00 * 0.6081 = 0.60810
    CHECK(music.resolveSoundEntryVolume(2523u) == Catch::Approx(0.40f).margin(0.005f));
    CHECK(music.resolveSoundEntryVolume(2532u) == Catch::Approx(0.69f).margin(0.005f));
    CHECK(music.resolveSoundEntryVolume(12291u) == Catch::Approx(0.80f).margin(0.005f));
    CHECK(music.resolveSoundEntryVolume(12528u) == Catch::Approx(0.80f).margin(0.005f));
    CHECK(music.resolveSoundEntryVolume(9789u) == Catch::Approx(0.90f).margin(0.005f));
    CHECK(music.resolveSoundEntryVolume(2902u) == Catch::Approx(1.00f).margin(0.005f));

    // ZoneMusic 394 (Quel'Danas Stage 1) -> SE:12291 (vol=0.80), ZoneMusic 302 (Eversong) -> SE:9789
    CHECK(music.resolveZoneMusicSoundEntryId(394u, /*night=*/false) == 12291u);
    CHECK(music.resolveZoneMusicSoundEntryId(395u, /*night=*/false) == 12292u);
    CHECK(music.resolveZoneMusicSoundEntryId(396u, /*night=*/false) == 12293u);
    CHECK(music.resolveZoneMusicSoundEntryId(402u, /*night=*/false) == 12528u);

    // 1. Direct Lua PlayMusic(path) (soundEntryId == 0) uses entryVolume=1.0 -> 0.6081 * slider,
    // even when the MP3 also appears in SE:2523 (vol=0.40).
    music.playMusic("Sound\\Music\\ZoneMusic\\Forest\\DayForest01.mp3", /*loop=*/true, /*fadeInMs=*/0.0f, /*soundEntryId=*/0u);
    CHECK(engine.getMusicVolume() == Catch::Approx(audio::MusicManager::kNativeMusicBusGain).margin(0.002f));

    // Slider 50% scales linearly -> 0.30405
    music.setVolume(50);
    CHECK(engine.getMusicVolume() == Catch::Approx(0.5f * audio::MusicManager::kNativeMusicBusGain).margin(0.002f));
    music.setVolume(100);
    CHECK(engine.getMusicVolume() == Catch::Approx(audio::MusicManager::kNativeMusicBusGain).margin(0.002f));

    // 2. Shared MP3 es_ruinswalkday03.mp3 played via Eversong (SE:9789, vol=0.90) vs Quel'Danas (SE:12528, vol=0.80)
    music.stopMusic(0.0f);
    music.playMusic("Sound\\Music\\ZoneMusic\\Eversong\\ES_RuinsWalkDay03.mp3", false, 0.0f, 9789u);
    CHECK(engine.getMusicVolume() == Catch::Approx(0.90f * audio::MusicManager::kNativeMusicBusGain).margin(0.003f));

    music.stopMusic(0.0f);
    music.playMusic("Sound\\Music\\ZoneMusic\\Eversong\\ES_RuinsWalkDay03.mp3", false, 0.0f, 12528u);
    CHECK(engine.getMusicVolume() == Catch::Approx(0.80f * audio::MusicManager::kNativeMusicBusGain).margin(0.003f));

    // 3. Stormwind ZoneMusic (SE:2532, vol=0.69 -> 0.4196)
    music.stopMusic(0.0f);
    music.playMusic("Sound\\Music\\CityMusic\\Stormwind\\stormwind05-zone.mp3", false, 0.0f, 2532u);
    CHECK(engine.getMusicVolume() == Catch::Approx(0.69f * audio::MusicManager::kNativeMusicBusGain).margin(0.003f));

    music.shutdown();
    engine.shutdown();
}

TEST_CASE("AudioEngine and MusicManager execute simultaneous dual-decoder crossfades without silence gaps",
          "[audio][tbc][music_crossfade]") {
    auto& env = tbcEnv();
    auto& engine = audio::AudioEngine::instance();
    engine.shutdown();
    constexpr uint32_t kSr = 48000u;
    REQUIRE(engine.initializeHeadless(kSr, 2));
    engine.setAssetManager(&env.assets);
    engine.setMasterVolume(1.0f);
    engine.setSuspended(false);

    SECTION("Synthetic 440 Hz -> 880 Hz 4.0s overlap keeps both decoders active and constant envelope") {
        auto wav440 = makeStereoSineWav(440.0f, 0.8f, kSr, kSr * 8u);
        auto wav880 = makeStereoSineWav(880.0f, 0.8f, kSr, kSr * 8u);

        constexpr float kGain = 0.5f;
        REQUIRE(engine.playMusic(wav440, kGain, /*loop=*/true, "synth_440.wav", /*keepOutgoingForCrossfade=*/false));
        CHECK(engine.isMusicPlaying());
        CHECK_FALSE(engine.isCrossfadeOutPlaying());

        // Warm 200 ms of steady 440 Hz
        std::vector<float> chunk(static_cast<size_t>(kSr / 10u) * 2u, 0.0f);
        REQUIRE(engine.renderMixFrames(chunk.data(), kSr / 10u) == kSr / 10u);
        CHECK(goertzelAmplitude(chunk, kSr, 440.0) == Catch::Approx(0.8 * kGain).margin(0.02));

        // Start 4.0s simultaneous overlapping crossfade to 880 Hz
        REQUIRE(engine.playMusic(wav880, 0.0f, /*loop=*/true, "synth_880.wav", /*keepOutgoingForCrossfade=*/true));
        REQUIRE(engine.isMusicPlaying());
        REQUIRE(engine.isCrossfadeOutPlaying());

        constexpr float kFadeSec = 4.0f;
        constexpr uint32_t kSteps = 40u; // 40 x 100 ms = 4.0 s
        double minWindowDbfs = 0.0;
        double midAmp440 = 0.0;
        double midAmp880 = 0.0;

        for (uint32_t step = 0; step < kSteps; ++step) {
            const float tMid = (static_cast<float>(step) + 0.5f) * 0.1f;
            const float inFrac = std::clamp(tMid / kFadeSec, 0.0f, 1.0f);
            const float outFrac = std::clamp(1.0f - tMid / kFadeSec, 0.0f, 1.0f);
            engine.setMusicVolume(kGain * inFrac);
            engine.setCrossfadeOutVolume(kGain * outFrac);

            REQUIRE(engine.renderMixFrames(chunk.data(), kSr / 10u) == kSr / 10u);
            const double dbfs = stereoRmsDbfs(chunk);
            minWindowDbfs = std::min(minWindowDbfs, dbfs);
            // Uncorrelated 440 Hz + 880 Hz sines never dip into a silence gap (> -14 dBFS throughout)
            CHECK(dbfs > -14.0);

            if (step == 19u || step == 20u) {
                midAmp440 = goertzelAmplitude(chunk, kSr, 440.0);
                midAmp880 = goertzelAmplitude(chunk, kSr, 880.0);
            }
        }

        // At midpoint (~2.0s), BOTH 440 Hz and 880 Hz must be simultaneously present at ~50% gain!
        CHECK(midAmp440 == Catch::Approx(0.8 * kGain * 0.5).margin(0.03));
        CHECK(midAmp880 == Catch::Approx(0.8 * kGain * 0.5).margin(0.03));

        // Finish crossfade and stop outgoing slot
        engine.stopCrossfadeOut();
        engine.setMusicVolume(kGain);
        CHECK_FALSE(engine.isCrossfadeOutPlaying());
        REQUIRE(engine.renderMixFrames(chunk.data(), kSr / 10u) == kSr / 10u);
        CHECK(goertzelAmplitude(chunk, kSr, 440.0) < 1e-4);
        CHECK(goertzelAmplitude(chunk, kSr, 880.0) == Catch::Approx(0.8 * kGain).margin(0.02));
    }

    SECTION("Live WorldState 3426 transition via notifyWorldStateMusicChanged crossfades real TBC MP3s with zero dead gap") {
        game::ZoneManager zones;
        zones.initialize();
        zones.setUseOriginalSoundtrack(false);
        zones.enrichFromDBC(&env.assets);

        audio::AudioCoordinator ac;
        REQUIRE(ac.initialize());
        // Re-bind headless engine after ac.initialize() so renderMixFrames is deterministic
        engine.shutdown();
        REQUIRE(engine.initializeHeadless(kSr, 2));
        ac.initializeEarlyAssets(&env.assets);
        REQUIRE(ac.getMusicManager() != nullptr);
        ac.getMusicManager()->setVolume(100);

        audio::ZoneAudioContext ctx;
        ctx.zoneManager = &zones;
        ctx.hasTile = true;
        ctx.areaId = 4085u;
        ctx.tileZoneId = 4080u;
        ctx.serverZoneId = 4080u;
        ctx.gameTimeHours = 12.0f;

        zones.setWorldState(3426u, 0u);
        // Advance 4.5s so Stage 0 track finishes its initial fade-in and reaches steady state
        std::vector<float> chunk(static_cast<size_t>(kSr / 10u) * 2u, 0.0f);
        for (int i = 0; i < 45; ++i) {
            ctx.deltaTime = 0.1f;
            ac.updateZoneAudio(ctx);
            REQUIRE(engine.renderMixFrames(chunk.data(), kSr / 10u) == kSr / 10u);
        }
        const std::string track0 = ac.getMusicManager()->getCurrentTrack();
        REQUIRE(zones.zoneMusicSetContainsTrack(394u, track0));
        // Seek 15s into track0 so outgoing audio is well past any soft MP3 intro
        std::vector<float> skip15s(static_cast<size_t>(kSr) * 2u, 0.0f);
        for (int s = 0; s < 15; ++s) {
            REQUIRE(engine.renderMixFrames(skip15s.data(), kSr) == kSr);
        }

        // Trigger WorldState 3426 = 1 EVEN INSIDE switch cooldown via notifyWorldStateMusicChanged()!
        REQUIRE(zones.setWorldState(3426u, 1u));
        ac.notifyWorldStateMusicChanged();

        ctx.deltaTime = 0.1f;
        ac.updateZoneAudio(ctx);
        const std::string track1 = ac.getMusicManager()->getCurrentTrack();
        CHECK(zones.zoneMusicSetContainsTrack(395u, track1));
        CHECK(ac.getMusicManager()->isCrossfading());
        CHECK(engine.isMusicPlaying());
        CHECK(engine.isCrossfadeOutPlaying());

        // Step across the entire 4.0s crossfade: verify zero -120 dBFS silence windows!
        int silentWindows = 0;
        for (int i = 0; i < 42; ++i) {
            REQUIRE(engine.renderMixFrames(chunk.data(), kSr / 10u) == kSr / 10u);
            if (stereoRmsDbfs(chunk) < -85.0) {
                ++silentWindows;
            }
            ctx.deltaTime = 0.1f;
            ac.updateZoneAudio(ctx);
        }
        CHECK(silentWindows == 0);
        CHECK_FALSE(ac.getMusicManager()->isCrossfading());
        CHECK_FALSE(engine.isCrossfadeOutPlaying());

        ac.shutdown();
    }

    engine.shutdown();
}

TEST_CASE("MusicManager starts zero streams or trace events when disabled or volume=0 and resumes immediately",
          "[audio][tbc][music_enable_gate]") {
    auto& env = tbcEnv();
    auto& engine = audio::AudioEngine::instance();

    const std::filesystem::path tracePath =
        std::filesystem::temp_directory_path() / "wowee_test_music_gate_trace.jsonl";
    std::filesystem::remove(tracePath);

    engine.shutdown();
    REQUIRE(engine.setAudioTracePath(tracePath.string()));
    REQUIRE(engine.initializeHeadless(48000, 2));
    engine.setAssetManager(&env.assets);

    std::vector<audio::AudioEngine::PlaybackEvent> musicEvents;
    engine.setPlaybackObserver([&](const audio::AudioEngine::PlaybackEvent& ev) {
        if (ev.music) musicEvents.push_back(ev);
    });

    game::ZoneManager zones;
    zones.initialize();
    zones.setUseOriginalSoundtrack(false);
    zones.enrichFromDBC(&env.assets);

    audio::AudioCoordinator ac;
    REQUIRE(ac.initialize());
    engine.shutdown();
    REQUIRE(engine.initializeHeadless(48000, 2));
    ac.initializeEarlyAssets(&env.assets);

    auto* music = ac.getMusicManager();
    REQUIRE(music != nullptr);

    // Simulate settings.cfg / cvars.cfg enable_music=0, music_volume=0 (SettingsPanel calls setVolume(0))
    music->setVolume(0);
    CHECK_FALSE(music->isEnabled());

    audio::ZoneAudioContext ctx;
    ctx.zoneManager = &zones;
    ctx.hasTile = true;
    ctx.areaId = 1519u; // Stormwind City (has both IntroMusic & ZoneMusic!)
    ctx.tileZoneId = 1519u;
    ctx.serverZoneId = 1519u;
    ctx.gameTimeHours = 12.0f;

    for (int i = 0; i < 20; ++i) {
        ctx.deltaTime = 0.25f;
        ac.updateZoneAudio(ctx);
    }
    music->preloadMusic("Sound\\Music\\CityMusic\\Stormwind\\stormwind06-zone.mp3");
    music->playMusic("Sound\\Music\\CityMusic\\Stormwind\\stormwind06-zone.mp3", true);
    music->crossfadeTo("Sound\\Music\\CityMusic\\Stormwind\\stormwind05-zone.mp3");

    CHECK(musicEvents.empty());
    CHECK_FALSE(music->isPlaying());
    CHECK_FALSE(engine.isMusicPlaying());

    // Re-enable music (Sound_EnableMusic = "1", MusicVolume = 100): next updateZoneAudio starts immediately!
    music->setVolume(100);
    CHECK(music->isEnabled());
    ctx.deltaTime = 0.1f;
    ac.updateZoneAudio(ctx);
    CHECK(music->isPlaying());
    CHECK(engine.isMusicPlaying());
    CHECK(musicEvents.size() == 1u);

    // Toggling Sound_EnableMusic back to 0 stops decoding immediately (Native 11_enablemusic_0 parity)
    music->setVolume(0);
    CHECK_FALSE(music->isPlaying());
    CHECK_FALSE(engine.isMusicPlaying());
    CHECK_FALSE(engine.isCrossfadeOutPlaying());

    engine.closeAudioTrace();
    engine.setPlaybackObserver({});
    ac.shutdown();
    engine.shutdown();

    std::ifstream in(tracePath);
    REQUIRE(in.is_open());
    int musicStartLines = 0;
    int zeroVolMusicStartLines = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (line.find("\"event\":\"start\"") != std::string::npos &&
            line.find("\"music\":true") != std::string::npos) {
            ++musicStartLines;
            if (line.find("stormwind06-zone.mp3") != std::string::npos) {
                ++zeroVolMusicStartLines;
            }
        }
    }
    in.close();
    std::filesystem::remove(tracePath);

    CHECK(musicStartLines == 1);
    CHECK(zeroVolMusicStartLines == 0);
}

TEST_CASE("SoundEntryTable culls 3D beyond maxDistance early and FootstepManager honors M2 $DSO vs $DSL loop modes",
          "[audio][tbc][m2_doodad_loops]") {
    auto& env = tbcEnv();
    auto& engine = audio::AudioEngine::instance();
    auto& table = audio::SoundEntryTable::instance();

    engine.shutdown();
    REQUIRE(engine.initializeHeadless(48000, 2));
    engine.setAssetManager(&env.assets);
    engine.setMasterVolume(1.0f);
    engine.setSuspended(false);
    engine.setListenerPosition(glm::vec3(0.0f, 0.0f, 0.0f));
    REQUIRE(table.ensureLoaded(&env.assets));

    std::vector<audio::AudioEngine::PlaybackEvent> sfxEvents;
    engine.setPlaybackObserver([&](const audio::AudioEngine::PlaybackEvent& ev) {
        if (!ev.music) sfxEvents.push_back(ev);
    });

    // Verify SE:3431 DeadMineSteamWhistleon metadata (flags=0x0200 kLooping, maxDist=45.0)
    const auto* se3431 = table.findById(&env.assets, 3431u);
    REQUIRE(se3431 != nullptr);
    CHECK((se3431->flags & audio::SoundEntryFlags::kLooping) != 0u);
    CHECK(se3431->maxDistance == Catch::Approx(45.0f));

    // Verify SE:3246 FlameCircleLoop metadata (flags=0x0, used by $DSL in spells/flamecircleeffect.m2!)
    const auto* se3246 = table.findById(&env.assets, 3246u);
    REQUIRE(se3246 != nullptr);
    CHECK((se3246->flags & audio::SoundEntryFlags::kLooping) == 0u);

    audio::FootstepManager fm;
    REQUIRE(fm.initialize(&env.assets));
    fm.setVolumeScale(1.0f);

    // 1. Load real TBC deadminesteamwhistleon.m2 ($DSO 3431 at ts=0, seq[0]=0..10000ms)
    auto whistleBytes = env.assets.readFile(
        "World\\Generic\\Human\\Passive Doodads\\SteamWhistles\\DeadmineSteamWhistleOn.m2");
    REQUIRE_FALSE(whistleBytes.empty());
    pipeline::M2Model whistleM2 = pipeline::M2Loader::load(whistleBytes);
    REQUIRE_FALSE(whistleM2.audioEvents.empty());

    // Beyond 45 yd (e.g. 1,300 yd away in Westfall/Stormwind): zero events started!
    sfxEvents.clear();
    const uint32_t farFired = fm.stepModelAudioEvents(
        101u, whistleM2.audioEvents, 0, 100.0f, 10000.0f, false,
        [](uint16_t, const glm::vec3&) { return glm::vec3(1300.0f, 0.0f, 0.0f); });
    CHECK(farFired == 0u);
    CHECK(sfxEvents.empty());
    CHECK_FALSE(fm.hasActiveModelLoop(101u));

    // Within 45 yd (5 yd away): $DSO 3431 plays cleanly as ONE-SHOT (not an infinite loop!)
    const uint32_t nearFired = fm.stepModelAudioEvents(
        102u, whistleM2.audioEvents, 0, 100.0f, 10000.0f, false,
        [](uint16_t, const glm::vec3&) { return glm::vec3(5.0f, 0.0f, 0.0f); });
    CHECK(nearFired == 1u);
    REQUIRE(sfxEvents.size() == 1u);
    CHECK(sfxEvents.back().soundEntryId == 3431u);
    CHECK_FALSE(fm.hasActiveModelLoop(102u));

    // 2. Load real TBC spells/flamecircleeffect.m2 ($DSL 3246 where SE:3246 has flags=0x0!)
    // ForceLoop on $DSL must make the stoppable handle loop continuously until stopped!
    auto flameBytes = env.assets.readFile("Spells\\FlameCircleEffect.m2");
    REQUIRE_FALSE(flameBytes.empty());
    pipeline::M2Model flameM2 = pipeline::M2Loader::load(flameBytes);
    REQUIRE_FALSE(flameM2.audioEvents.empty());

    sfxEvents.clear();
    const uint32_t dslFired = fm.stepModelAudioEvents(
        201u, flameM2.audioEvents, 0, 100.0f, 3000.0f, false,
        [](uint16_t, const glm::vec3&) { return glm::vec3(4.0f, 0.0f, 0.0f); });
    CHECK(dslFired == 1u);
    CHECK(fm.hasActiveModelLoop(201u));

    // Render 6.0 seconds of PCM (> sample duration) and verify the $DSL loop stays alive!
    std::vector<float> buf1s(48000u * 2u, 0.0f);
    for (int s = 0; s < 6; ++s) {
        REQUIRE(engine.renderMixFrames(buf1s.data(), 48000u) == 48000u);
        engine.update(1.0f);
    }
    CHECK(engine.isSoundEntryPlaying(3246u));

    fm.stopModelAudioInstance(201u);
    CHECK_FALSE(fm.hasActiveModelLoop(201u));
    CHECK_FALSE(engine.isSoundEntryPlaying(3246u));

    engine.setPlaybackObserver({});
    fm.shutdown();
    engine.shutdown();
}
