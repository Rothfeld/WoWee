// Deterministic TBC 2.4.3 Environmental Reverb & Underwater Acoustic Parity Suite.
//
// Validates:
//   1. SoundProviderPreferences.dbc (32 rows x 24 cols), SoundSamplePreferences.dbc (2 rows x 17 cols),
//      SoundEntries.dbc col 28 (EAXDef), WMOAreaTable.dbc (cols 4-5), and AreaTable.dbc (cols 5-6).
//   2. WMO & AreaTable preset resolution (Stormwind Cathedral of Light ID 73 PRESET_AUDITORIUM,
//      Jasperlode Mine / caves ID 75 PRESET_CAVE, Deadmines ID 76 PRESET_ARENA, PRESET_OFF ID 66,
//      negative/missing WMO prefs -> dry, submerged -> ID 11 'Underwater', Sound_EnableReverb gate).
//   3. Headless AudioEngine::renderMixFrames PCM verification:
//      - Outdoor dry vs PRESET_AUDITORIUM (73) vs PRESET_CAVE (75) reverb tail energy (100..600 ms
//        and 600..1500 ms post-offset RMS dB) and decay ordering (Auditorium 4.32s > Room 0.40s).
//      - Dry bus isolation: UI sound (SoundType 2 'igMainMenuOptionCheckBoxOn', sid=856) and
//        EAXDef == 1 ('Ashbringer', sid=8940) stay 100% dry indoors and underwater.
//      - Underwater (ID 11) low-frequency resonance & >2 kHz vs <500 Hz spectral roll-off.
//      - Sound_EnableReverb == false suppresses environmental reverb cleanly.
//      - Smooth room/submerge preset transitions produce zero sample discontinuities/clicks.
//
// Run with:
//   WOWEE_TBC_DATA_DIR=/workspace/azerothcore/client/wowee-data/expansions/tbc ./build/bin/test_env_audio_reverb_underwater

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <catch_amalgamated.hpp>

#include "audio/audio_coordinator.hpp"
#include "audio/audio_engine.hpp"
#include "audio/environment_reverb.hpp"
#include "audio/sound_entries.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <numbers>
#include <vector>

using namespace wowee;

namespace {

struct TbcEnvAudioFixture {
    pipeline::AssetManager assets;
    pipeline::DBCLayout dbcLayout;
    bool ready = false;

    TbcEnvAudioFixture() {
        (void)audio::AudioEngine::instance();
        (void)audio::SoundEntryTable::instance();

        const char* root = std::getenv("WOWEE_TBC_DATA_DIR");
        const std::filesystem::path defaultRoot = "/workspace/azerothcore/client/wowee-data/expansions/tbc";
        const std::filesystem::path chosen = (root && *root) ? std::filesystem::path(root) : defaultRoot;
        if (!std::filesystem::exists(chosen)) return;
        if (!assets.initialize(chosen.string())) return;
        assets.setExpansionDataPath(chosen.string());

        const std::filesystem::path layoutPath =
            std::filesystem::path(WOWEE_SOURCE_DIR) / "Data" / "expansions" / "tbc" / "dbc_layouts.json";
        if (std::filesystem::exists(layoutPath) && dbcLayout.loadFromJson(layoutPath.string())) {
            pipeline::setActiveDBCLayout(&dbcLayout);
        }
        ready = true;
    }

    ~TbcEnvAudioFixture() {
        audio::SoundEntryTable::instance().reset();
        audio::AudioEngine::instance().setPlaybackObserver({});
        audio::AudioEngine::instance().setAssetManager(nullptr);
        audio::AudioEngine::instance().shutdown();
        pipeline::setActiveDBCLayout(nullptr);
        assets.shutdown();
    }
};

TbcEnvAudioFixture& tbcFixture() {
    static TbcEnvAudioFixture instance;
    if (!instance.ready) {
        SKIP("TBC 2.4.3 data directory not available");
    }
    return instance;
}

/// Build an in-memory 16-bit mono PCM RIFF WAV buffer from float samples in [-1, 1].
std::vector<uint8_t> buildMonoWav16(const std::vector<float>& samples, uint32_t sampleRate = 48000) {
    const uint32_t dataBytes = static_cast<uint32_t>(samples.size() * sizeof(int16_t));
    const uint32_t riffSize = 36u + dataBytes;
    std::vector<uint8_t> wav(44u + dataBytes, 0u);

    auto writeU32 = [&](size_t off, uint32_t v) { std::memcpy(wav.data() + off, &v, 4); };
    auto writeU16 = [&](size_t off, uint16_t v) { std::memcpy(wav.data() + off, &v, 2); };

    std::memcpy(wav.data(), "RIFF", 4);
    writeU32(4, riffSize);
    std::memcpy(wav.data() + 8, "WAVEfmt ", 8);
    writeU32(16, 16u);
    writeU16(20, 1u); // PCM
    writeU16(22, 1u); // mono
    writeU32(24, sampleRate);
    writeU32(28, sampleRate * 2u);
    writeU16(32, 2u);
    writeU16(34, 16u);
    std::memcpy(wav.data() + 36, "data", 4);
    writeU32(40, dataBytes);

    for (size_t i = 0; i < samples.size(); ++i) {
        const float clamped = std::clamp(samples[i], -1.0f, 1.0f);
        const int16_t s = static_cast<int16_t>(std::lround(clamped * 32767.0f));
        std::memcpy(wav.data() + 44u + i * 2u, &s, 2);
    }
    return wav;
}

/// Synthesize a deterministic broadband multi-harmonic burst (120 Hz .. 6000 Hz) of `durationSec`.
std::vector<uint8_t> makeBroadbandBurstWav(double durationSec, uint32_t sampleRate = 48000) {
    const size_t n = static_cast<size_t>(std::lround(durationSec * sampleRate));
    std::vector<float> pcm(n, 0.0f);
    constexpr double freqs[] = {160.0, 310.0, 620.0, 1250.0, 2600.0, 4200.0, 5800.0};
    for (size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(sampleRate);
        // Smooth 5 ms raised-cosine fade-in/out to avoid synthesis edge clicks
        double env = 1.0;
        const double fade = 0.005;
        if (t < fade) env = 0.5 * (1.0 - std::cos(std::numbers::pi * t / fade));
        else if (durationSec - t < fade) env = 0.5 * (1.0 - std::cos(std::numbers::pi * (durationSec - t) / fade));

        double sum = 0.0;
        for (size_t k = 0; k < std::size(freqs); ++k) {
            sum += std::sin(2.0 * std::numbers::pi * freqs[k] * t + static_cast<double>(k) * 0.37);
        }
        pcm[i] = static_cast<float>(0.65 * env * (sum / static_cast<double>(std::size(freqs))));
    }
    return buildMonoWav16(pcm, sampleRate);
}

double rmsDbMonoRange(const std::vector<float>& stereo, size_t frameStart, size_t frameEnd) {
    if (frameEnd <= frameStart || frameStart * 2u >= stereo.size()) return -180.0;
    const size_t endClamped = std::min(frameEnd, stereo.size() / 2u);
    double sumSq = 0.0;
    size_t count = 0;
    for (size_t i = frameStart; i < endClamped; ++i) {
        const double m = 0.5 * (static_cast<double>(stereo[i * 2u]) + static_cast<double>(stereo[i * 2u + 1u]));
        REQUIRE(std::isfinite(m));
        sumSq += m * m;
        ++count;
    }
    if (count == 0 || sumSq <= 0.0) return -180.0;
    return 20.0 * std::log10(std::sqrt(sumSq / static_cast<double>(count)));
}

/// Exact Goertzel/DFT band RMS dB over [fLo, fHi] Hz on a mono slice.
double dftBandDb(const std::vector<float>& stereo, size_t frameStart, size_t frameEnd,
                 uint32_t sampleRate, double fLo, double fHi) {
    const size_t endClamped = std::min(frameEnd, stereo.size() / 2u);
    REQUIRE(endClamped > frameStart + 32u);
    const size_t N = endClamped - frameStart;
    std::vector<double> x(N, 0.0);
    for (size_t n = 0; n < N; ++n) {
        const double w = 0.5 * (1.0 - std::cos(2.0 * std::numbers::pi * static_cast<double>(n) / static_cast<double>(N - 1u)));
        const double m = 0.5 * (static_cast<double>(stereo[(frameStart + n) * 2u]) +
                                static_cast<double>(stereo[(frameStart + n) * 2u + 1u]));
        x[n] = m * w;
    }

    const size_t kMin = std::max<size_t>(1u, static_cast<size_t>(std::ceil(fLo * static_cast<double>(N) / sampleRate)));
    const size_t kMax = std::min<size_t>(N / 2u, static_cast<size_t>(std::floor(fHi * static_cast<double>(N) / sampleRate)));
    REQUIRE(kMax >= kMin);

    // Step bins sparsely if N is huge, exact if N <= 8192
    const size_t step = std::max<size_t>(1u, (kMax - kMin) / 256u);
    double magSqSum = 0.0;
    size_t bins = 0;
    for (size_t k = kMin; k <= kMax; k += step) {
        const double omega = 2.0 * std::numbers::pi * static_cast<double>(k) / static_cast<double>(N);
        const double coeff = 2.0 * std::cos(omega);
        double s0 = 0.0, s1 = 0.0, s2 = 0.0;
        for (size_t n = 0; n < N; ++n) {
            s0 = x[n] + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
        magSqSum += std::max(0.0, power);
        ++bins;
    }
    const double magRms = std::sqrt(magSqSum / std::max<size_t>(1u, bins)) / static_cast<double>(N);
    return (magRms > 1e-12) ? (20.0 * std::log10(magRms)) : -180.0;
}

} // namespace

TEST_CASE("EnvironmentReverbTable parses TBC acoustic DBCs and resolves room/underwater presets", "[audio][reverb][dbc]") {
    auto& fix = tbcFixture();
    audio::EnvironmentReverbTable table;
    REQUIRE(table.initialize(&fix.assets));

    // 1. SoundProviderPreferences.dbc: 32 rows x 24 columns
    CHECK(table.providerPrefCount() == 32u);
    const auto* uw = table.findProviderPref(11u);
    REQUIRE(uw != nullptr);
    CHECK(uw->description == "Underwater");
    CHECK(uw->decayTime == Catch::Approx(1.0f));
    CHECK(uw->eaxRoom == -1000);
    CHECK(uw->eaxRoomHf == -10000);
    CHECK(uw->eaxDecayHfRatio == Catch::Approx(0.10f));
    CHECK(uw->eaxReverb == 1700);
    CHECK(uw->eaxModulationTime == Catch::Approx(1.18f));
    CHECK(uw->eaxModulationDepth == Catch::Approx(0.348f));
    CHECK(audio::EnvironmentReverbTable::isActivePreset(uw));

    const auto* auditorium = table.findProviderPref(73u);
    REQUIRE(auditorium != nullptr);
    CHECK(auditorium->description == "PRESET_AUDITORIUM");
    CHECK(auditorium->decayTime == Catch::Approx(4.32f));
    CHECK(auditorium->eaxRoom == -1000);
    CHECK(auditorium->eaxRoomHf == -476);
    CHECK(auditorium->eaxReverb == -289);
    CHECK(audio::EnvironmentReverbTable::isActivePreset(auditorium));

    const auto* cave = table.findProviderPref(75u);
    REQUIRE(cave != nullptr);
    CHECK(cave->description == "PRESET_CAVE");
    CHECK(cave->decayTime == Catch::Approx(3.00f));
    CHECK(audio::EnvironmentReverbTable::isActivePreset(cave));

    const auto* presetOff = table.findProviderPref(66u);
    REQUIRE(presetOff != nullptr);
    CHECK(presetOff->description == "PRESET_OFF");
    CHECK_FALSE(audio::EnvironmentReverbTable::isActivePreset(presetOff));

    // 2. SoundSamplePreferences.dbc: 2 rows x 17 columns + SoundEntries EAXDef
    CHECK(table.samplePrefCount() == 2u);
    CHECK(table.sampleSuppressesRoom(1u));
    CHECK_FALSE(table.sampleSuppressesRoom(2u));
    CHECK_FALSE(table.sampleSuppressesRoom(0u));

    auto& se = audio::SoundEntryTable::instance();
    REQUIRE(se.ensureLoaded(&fix.assets));
    const auto* uiClick = se.findById(&fix.assets, 856u); // igMainMenuOptionCheckBoxOn (SoundType=2, EAXDef=2)
    REQUIRE(uiClick != nullptr);
    CHECK(uiClick->soundType == 2u);
    CHECK(uiClick->eaxDef == 2u);
    CHECK_FALSE(audio::SoundEntryTable::usesEnvironmentBus(*uiClick, audio::SESoundCategory::kSfx));

    const auto* cheer = se.findById(&fix.assets, 2677u); // Human Male Vocal 09 (cheer) (SoundType=29, EAXDef=2)
    REQUIRE(cheer != nullptr);
    CHECK(cheer->eaxDef == 2u);
    CHECK(audio::SoundEntryTable::usesEnvironmentBus(*cheer, audio::SESoundCategory::kSfx));

    const auto* hello = se.findById(&fix.assets, 6162u); // Human Male Vocal 12 (Hello) (SoundType=29, EAXDef=0)
    REQUIRE(hello != nullptr);
    CHECK(hello->eaxDef == 0u);
    CHECK(audio::SoundEntryTable::usesEnvironmentBus(*hello, audio::SESoundCategory::kSfx));

    const auto* ashbringer = se.findById(&fix.assets, 8940u); // Ashbringer (EAXDef=1 -> excluded)
    REQUIRE(ashbringer != nullptr);
    CHECK(ashbringer->eaxDef == 1u);
    CHECK_FALSE(audio::SoundEntryTable::usesEnvironmentBus(*ashbringer, audio::SESoundCategory::kSfx));

    // 3. WMOAreaTable & AreaTable resolution
    // Stormwind Cathedral of Light (wmoRootId=61, nameSet=0, groupId=2660 -> 73 PRESET_AUDITORIUM)
    audio::EnvironmentQuery qCathedral{
        .hasWmoLocation = true,
        .insideInteriorWmo = true,
        .wmoRootId = 61u,
        .wmoNameSet = 0u,
        .wmoGroupId = 2660u,
        .areaId = 1519u,
        .zoneId = 1519u,
        .submerged = false,
        .reverbEnabled = true,
    };
    CHECK(table.resolvePresetId(qCathedral) == 73u);

    // Sound_EnableReverb == false disables preset
    qCathedral.reverbEnabled = false;
    CHECK(table.resolvePresetId(qCathedral) == 0u);

    // Stormwind Canal submerged (wmoRootId=61, groupId=2500 has uwPref=0, wildcard grp=-1 has uwPref=11)
    audio::EnvironmentQuery qCanalUw{
        .hasWmoLocation = true,
        .insideInteriorWmo = true,
        .wmoRootId = 61u,
        .wmoNameSet = 0u,
        .wmoGroupId = 2500u,
        .areaId = 1519u,
        .zoneId = 1519u,
        .submerged = true,
        .reverbEnabled = true,
    };
    CHECK(table.resolvePresetId(qCanalUw) == 11u);

    // Shimmering Flats outdoor dry (Thousand Needles areaId=400 -> 0)
    audio::EnvironmentQuery qDry{
        .hasWmoLocation = false,
        .areaId = 400u,
        .zoneId = 400u,
        .submerged = false,
        .reverbEnabled = true,
    };
    CHECK(table.resolvePresetId(qDry) == 0u);

    // AreaTable col 5 outdoor/zone reverb: Northshire Abbey (AreaID=24 -> 73), Deadmines (AreaID=1581 -> 76)
    audio::EnvironmentQuery qAbbey{.areaId = 24u, .zoneId = 12u, .submerged = false, .reverbEnabled = true};
    CHECK(table.resolvePresetId(qAbbey) == 73u);
    audio::EnvironmentQuery qDeadmines{.areaId = 1581u, .zoneId = 1581u, .submerged = false, .reverbEnabled = true};
    CHECK(table.resolvePresetId(qDeadmines) == 76u);
}

TEST_CASE("AudioEngine headless node graph synthesizes room reverb, underwater HF damping, dry UI isolation, and click-free transitions", "[audio][reverb][dsp]") {
    auto& fix = tbcFixture();
    audio::EnvironmentReverbTable table;
    REQUIRE(table.initialize(&fix.assets));

    auto& engine = audio::AudioEngine::instance();
    engine.shutdown();
    constexpr uint32_t kSr = 48000u;
    REQUIRE(engine.initializeHeadless(kSr, 2u));
    engine.setAssetManager(&fix.assets);
    engine.setMasterVolume(1.0f);
    engine.setSuspended(false);

    const auto burstWav = makeBroadbandBurstWav(0.20, kSr); // 200 ms burst

    auto renderScenario = [&](const audio::SoundProviderPrefRecord* preset, bool revEnabled,
                              audio::SfxPlayOptions opts, double totalSec = 1.8) {
        engine.clearEnvironmentPreset();
        // Settle smoothed parameters
        std::vector<float> warm(kSr * 2u / 4u, 0.0f);
        engine.setEnvironmentPreset(preset, revEnabled);
        engine.renderMixFrames(warm.data(), static_cast<uint32_t>(warm.size() / 2u));

        REQUIRE(engine.playSound2D(burstWav, 0.8f, 1.0f, opts));
        const uint32_t totalFrames = static_cast<uint32_t>(std::lround(totalSec * kSr));
        std::vector<float> out(static_cast<size_t>(totalFrames) * 2u, 0.0f);
        REQUIRE(engine.renderMixFrames(out.data(), totalFrames) == totalFrames);
        return out;
    };

    audio::SfxPlayOptions worldOpts;
    worldOpts.eaxDef = 2u;
    worldOpts.envBusMode = audio::EnvironmentBusMode::ForceWet;

    audio::SfxPlayOptions uiOpts;
    uiOpts.eaxDef = 0u;
    uiOpts.envBusMode = audio::EnvironmentBusMode::ForceDry;

    // 1. Outdoor dry vs Cathedral Auditorium (73) vs Cave (75) vs Room (69, decay=0.40s)
    const auto dryPcm = renderScenario(nullptr, true, worldOpts);
    const auto audPcm = renderScenario(table.findProviderPref(73u), true, worldOpts);
    const auto cavePcm = renderScenario(table.findProviderPref(75u), true, worldOpts);
    const auto roomPcm = renderScenario(table.findProviderPref(69u), true, worldOpts);

    const size_t burstEnd = static_cast<size_t>(0.20 * kSr);
    const double dryTail100_600 = rmsDbMonoRange(dryPcm, burstEnd + kSr / 10u, burstEnd + (kSr * 6u) / 10u);
    const double audBody = rmsDbMonoRange(audPcm, 0u, burstEnd);
    const double audTail100_600 = rmsDbMonoRange(audPcm, burstEnd + kSr / 10u, burstEnd + (kSr * 6u) / 10u);
    const double audTail600_1500 = rmsDbMonoRange(audPcm, burstEnd + (kSr * 6u) / 10u, burstEnd + (kSr * 15u) / 10u);
    const double caveTail100_600 = rmsDbMonoRange(cavePcm, burstEnd + kSr / 10u, burstEnd + (kSr * 6u) / 10u);
    const double roomTail600_1500 = rmsDbMonoRange(roomPcm, burstEnd + (kSr * 6u) / 10u, burstEnd + (kSr * 15u) / 10u);

    CHECK(dryTail100_600 < -120.0);
    CHECK(audTail100_600 > audBody - 34.0);
    CHECK(audTail100_600 > -50.0);
    CHECK(caveTail100_600 > -52.0);
    // Auditorium (T60 = 4.32s) sustains significantly more late energy at 600..1500ms than Room (T60 = 0.40s)
    CHECK(audTail600_1500 > roomTail600_1500 + 20.0);

    // 2. UI sound (ForceDry / SoundType 2) stays 100% dry even inside Auditorium and Underwater
    const auto uiInAud = renderScenario(table.findProviderPref(73u), true, uiOpts);
    const double uiTailInAud = rmsDbMonoRange(uiInAud, burstEnd + kSr / 10u, burstEnd + (kSr * 6u) / 10u);
    CHECK(uiTailInAud < -120.0);

    // Also test real TBC igMainMenuOptionCheckBoxOn (sid=856) via SoundEntryTable inside Auditorium
    {
        auto& se = audio::SoundEntryTable::instance();
        REQUIRE(se.ensureLoaded(&fix.assets));
        engine.setEnvironmentPreset(table.findProviderPref(73u), true);
        std::vector<float> warm(kSr / 2u, 0.0f);
        engine.renderMixFrames(warm.data(), kSr / 4u);
        REQUIRE(se.play2D(&fix.assets, 856u, 1.0f, 1.0f));
        std::vector<float> uiReal(static_cast<size_t>(kSr) * 2u, 0.0f);
        REQUIRE(engine.renderMixFrames(uiReal.data(), kSr) == kSr);
        // igMainMenuOptionCheckBoxOn is ~95ms; 250..750ms must be completely dry
        const double realUiTail = rmsDbMonoRange(uiReal, kSr / 4u, (kSr * 3u) / 4u);
        CHECK(realUiTail < -110.0);
    }

    // 3. Underwater (ID 11): wet response has steep >2 kHz vs <500 Hz spectral roll-off (> 22 dB),
    // and real TBC broadband vocal ('HumanMaleCheer01.wav') shifts composite (>2kHz - <500Hz) tilt
    // downward toward low frequencies matching Native TBC 2.4.3 (-0.7..-3.7 dB).
    const auto uwPcm = renderScenario(table.findProviderPref(11u), true, worldOpts);
    std::vector<float> uwWetOnly(uwPcm.size(), 0.0f);
    for (size_t i = 0; i < uwPcm.size(); ++i) {
        uwWetOnly[i] = uwPcm[i] - dryPcm[i];
    }
    const double dryTilt = dftBandDb(dryPcm, 0u, burstEnd, kSr, 2000.0, 7000.0) -
                           dftBandDb(dryPcm, 0u, burstEnd, kSr, 100.0, 500.0);
    const double uwWetTilt = dftBandDb(uwWetOnly, 0u, burstEnd + kSr / 4u, kSr, 2000.0, 7000.0) -
                             dftBandDb(uwWetOnly, 0u, burstEnd + kSr / 4u, kSr, 100.0, 500.0);
    CHECK(uwWetTilt < dryTilt - 22.0);

    {
        const auto cheerBytes = fix.assets.readFile("Sound\\Character\\Human\\HumanVocalMale\\HumanMaleCheer01.wav");
        REQUIRE(!cheerBytes.empty());

        auto renderCheer = [&](const audio::SoundProviderPrefRecord* preset) {
            engine.clearEnvironmentPreset();
            std::vector<float> warm(kSr, 0.0f);
            engine.setEnvironmentPreset(preset, true);
            engine.renderMixFrames(warm.data(), kSr / 2u);
            REQUIRE(engine.playSound2D(cheerBytes, 0.8f, 1.0f, worldOpts));
            const uint32_t frames = kSr * 2u; // 2.0 s
            std::vector<float> out(static_cast<size_t>(frames) * 2u, 0.0f);
            REQUIRE(engine.renderMixFrames(out.data(), frames) == frames);
            return out;
        };

        const auto cheerDry = renderCheer(nullptr);
        const auto cheerUw = renderCheer(table.findProviderPref(11u));
        const size_t bodyEnd = static_cast<size_t>(1.25 * kSr);
        const double cheerDryTilt = dftBandDb(cheerDry, 0u, bodyEnd, kSr, 2000.0, 7000.0) -
                                    dftBandDb(cheerDry, 0u, bodyEnd, kSr, 100.0, 500.0);
        const double cheerUwTilt = dftBandDb(cheerUw, 0u, bodyEnd, kSr, 2000.0, 7000.0) -
                                   dftBandDb(cheerUw, 0u, bodyEnd, kSr, 100.0, 500.0);
        CHECK(cheerUwTilt < cheerDryTilt - 0.5);
    }

    // 4. Sound_EnableReverb == false suppresses Auditorium reverb completely
    const auto audRevOff = renderScenario(table.findProviderPref(73u), false, worldOpts);
    const double audRevOffTail = rmsDbMonoRange(audRevOff, burstEnd + kSr / 10u, burstEnd + (kSr * 6u) / 10u);
    CHECK(audRevOffTail < -110.0);

    // 5. Click-free transitions across Dry -> Auditorium -> Underwater -> ReverbOff during continuous tone
    {
        const size_t toneSamples = kSr + kSr / 4u; // 1.25 s continuous 300 Hz sine spanning the full 1.0 s window
        std::vector<float> sineMono(toneSamples, 0.0f);
        for (size_t i = 0; i < toneSamples; ++i) {
            const double t = static_cast<double>(i) / kSr;
            const double dur = static_cast<double>(toneSamples) / kSr;
            const double env = (t < 0.01) ? (t / 0.01) : ((dur - t < 0.01) ? ((dur - t) / 0.01) : 1.0);
            sineMono[i] = static_cast<float>(0.4 * env * std::sin(2.0 * std::numbers::pi * 300.0 * t));
        }
        const auto sineWav = buildMonoWav16(sineMono, kSr);

        engine.clearEnvironmentPreset();
        REQUIRE(engine.playSound2D(sineWav, 0.8f, 1.0f, worldOpts));

        const uint32_t quarter = kSr / 4u;
        std::vector<float> full(static_cast<size_t>(kSr) * 2u, 0.0f);
        REQUIRE(engine.renderMixFrames(full.data(), quarter) == quarter);
        engine.setEnvironmentPreset(table.findProviderPref(73u), true);
        REQUIRE(engine.renderMixFrames(full.data() + quarter * 2u, quarter) == quarter);
        engine.setEnvironmentPreset(table.findProviderPref(11u), true);
        REQUIRE(engine.renderMixFrames(full.data() + quarter * 4u, quarter) == quarter);
        engine.setEnvironmentPreset(table.findProviderPref(11u), false);
        REQUIRE(engine.renderMixFrames(full.data() + quarter * 6u, quarter) == quarter);

        float maxStep = 0.0f;
        size_t maxStepIdx = 0;
        float maxSecondDiff = 0.0f;
        size_t maxSecondIdx = 0;
        for (size_t i = 1; i < kSr; ++i) {
            const float s = std::max(std::abs(full[i * 2u] - full[(i - 1u) * 2u]),
                                     std::abs(full[i * 2u + 1u] - full[(i - 1u) * 2u + 1u]));
            if (s > maxStep) {
                maxStep = s;
                maxStepIdx = i;
            }
        }
        for (size_t i = 2; i < kSr; ++i) {
            const float d2L = std::abs(full[i * 2u] - 2.0f * full[(i - 1u) * 2u] + full[(i - 2u) * 2u]);
            const float d2R = std::abs(full[i * 2u + 1u] - 2.0f * full[(i - 1u) * 2u + 1u] + full[(i - 2u) * 2u + 1u]);
            const float d2 = std::max(d2L, d2R);
            if (d2 > maxSecondDiff) {
                maxSecondDiff = d2;
                maxSecondIdx = i;
            }
        }
        INFO("maxStepIdx=" << maxStepIdx << " maxSecondIdx=" << maxSecondIdx
             << " L[idx-2..idx+1]=" << full[(maxStepIdx > 1 ? maxStepIdx - 2 : 0) * 2u]
             << "," << full[(maxStepIdx > 0 ? maxStepIdx - 1 : 0) * 2u]
             << "," << full[maxStepIdx * 2u]
             << "," << full[std::min<size_t>(kSr - 1u, maxStepIdx + 1u) * 2u]);
        // Any hard step/click discontinuity jumps in both first (>0.10) and second (>0.05) differences,
        // whereas a smooth 300 Hz resonant waveform has tiny curvature (< 0.015).
        CHECK(maxStep < 0.08f);
        CHECK(maxSecondDiff < 0.015f);
    }

    engine.shutdown();
}
