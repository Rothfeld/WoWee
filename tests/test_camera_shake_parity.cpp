// TBC 2.4.3 (build 8606) data-driven camera shake parity tests:
// - CameraShakes.dbc waveform evaluation (inch amplitude 1/36 yd, phase offset,
//   SINE vs DAMPED_SINE envelope, duration - phase cutoff)
// - 3D camera-to-source distance attenuation (full inside 9 yd, exp decay to 100 yd,
//   zero at >= 100 yd) against live Native TBC 2.4.3 CGCamera telemetry
// - Multi-shake per-axis max(|disp|) rule & camera horizontal basis / world +Z mapping
// - Shipped TBC DBC + M2 data chain: Kodo Graveyard (Ancient/Aged/Dying Kodo ->
//   CreatureModelData 27 -> FootstepShakeSize=1, DeathThudShakeSize=10), player
//   Kodo mounts (0/0), Fel Reaver (82/12), giants, KodoBeast.m2 footfall + $DTH,
//   and spell/world M2 $SHK -> SpellEffectCameraShakes.dbc.
#include <catch_amalgamated.hpp>

#include "pipeline/dbc_layout.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/m2_loader.hpp"
#include "rendering/camera_shake.hpp"
#include "rendering/footfall_window.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

std::vector<uint8_t> readBytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    return {std::istreambuf_iterator<char>(input), {}};
}

std::optional<fs::path> findCaseInsensitive(const fs::path& base, const fs::path& relative) {
    fs::path cur = base;
    for (const auto& part : relative) {
        const fs::path exact = cur / part;
        std::error_code ec;
        if (fs::exists(exact, ec)) {
            cur = exact;
            continue;
        }
        std::string targetLower = part.string();
        std::transform(targetLower.begin(), targetLower.end(), targetLower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        bool matched = false;
        for (const auto& entry : fs::directory_iterator(cur, ec)) {
            std::string nameLower = entry.path().filename().string();
            std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (nameLower == targetLower) {
                cur = entry.path();
                matched = true;
                break;
            }
        }
        if (!matched) return std::nullopt;
    }
    return cur;
}

std::optional<fs::path> resolveTbcDataDir() {
    if (const char* env = std::getenv("WOWEE_TBC_DATA_DIR"); env && *env) {
        const fs::path p(env);
        std::error_code ec;
        if (fs::is_directory(p, ec)) return p;
    }
    const fs::path defaultDir("/workspace/azerothcore/client/wowee-data/expansions/tbc");
    std::error_code ec;
    if (fs::is_directory(defaultDir, ec)) return defaultDir;
    return std::nullopt;
}

wowee::pipeline::DBCFile loadTbcDbc(const fs::path& tbcRoot, const char* dbcName) {
    std::optional<fs::path> found = findCaseInsensitive(tbcRoot, fs::path("DBFilesClient") / dbcName);
    if (!found) {
        found = findCaseInsensitive(fs::path("/workspace/azerothcore/server/data/dbc"), fs::path(dbcName));
    }
    REQUIRE(found.has_value());
    const auto bytes = readBytes(*found);
    REQUIRE_FALSE(bytes.empty());
    wowee::pipeline::DBCFile dbc;
    REQUIRE(dbc.load(bytes));
    return dbc;
}

wowee::pipeline::M2Model loadTbcM2(const fs::path& tbcRoot, const char* relPath) {
    const auto found = findCaseInsensitive(tbcRoot, fs::path(relPath));
    REQUIRE(found.has_value());
    const auto bytes = readBytes(*found);
    REQUIRE_FALSE(bytes.empty());
    return wowee::pipeline::M2Loader::load(bytes);
}

int primarySequenceIndex(const wowee::pipeline::M2Model& model, uint32_t animId) {
    for (size_t i = 0; i < model.sequences.size(); ++i) {
        if (model.sequences[i].id == animId && model.sequences[i].variationIndex == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

} // namespace

TEST_CASE("CameraShakes.dbc waveform matches Build 8606 CGCamera telemetry", "[camera_shake]") {
    using wowee::rendering::CameraShakeEntry;
    using wowee::rendering::evaluateCameraShakeSample;

    SECTION("ID=1 (Kodo footstep): 2 in amplitude, 3 Hz damped sine, dur=0.40 s, phase=0.06 s") {
        const CameraShakeEntry id1{
            .id = 1,
            .shakeType = 1,
            .axis = 2,
            .amplitude = 2.0f,
            .frequency = 3.0f,
            .duration = 0.40f,
            .phase = 0.06f,
            .coefficient = 1.0f,
        };
        // At elapsed = 0 s (tau = 0.06 s), shake already starts mid-rise (+0.04734 yd).
        CHECK(evaluateCameraShakeSample(id1, 0.0f) == Catch::Approx(0.04734f).margin(0.0003f));

        // Peak occurs near elapsed = 0.0196 s (tau = 0.0796 s):
        // Build 8606 telemetry measured +0.05116 yd on Kodo Beast & Ancient Kodo.
        float maxPositive = 0.0f;
        float minNegative = 0.0f;
        for (int i = 0; i < 400; ++i) {
            const float t = static_cast<float>(i) * 0.001f;
            const float s = evaluateCameraShakeSample(id1, t);
            maxPositive = std::max(maxPositive, s);
            minNegative = std::min(minNegative, s);
        }
        CHECK(maxPositive == Catch::Approx(0.05116f).margin(0.0002f));
        CHECK(minNegative == Catch::Approx(-0.04332f).margin(0.0005f));
        // Active lifetime is duration - phase = 0.34 s.
        CHECK(std::abs(evaluateCameraShakeSample(id1, 0.33f)) > 0.0f);
        CHECK(evaluateCameraShakeSample(id1, 0.341f) == 0.0f);
        CHECK(evaluateCameraShakeSample(id1, 0.50f) == 0.0f);
    }

    SECTION("ID=10 (Kodo death thud): phase=0.16 shifts start tau and shortens lifetime to 0.44 s") {
        const CameraShakeEntry id10{
            .id = 10,
            .shakeType = 1,
            .axis = 2,
            .amplitude = 3.0f,
            .frequency = 2.0f,
            .duration = 0.60f,
            .phase = 0.16f,
            .coefficient = 2.0f,
        };
        // At elapsed = 0.003 s (tau = 0.163 s), Build 8606 telemetry measured +0.053421 yd.
        CHECK(evaluateCameraShakeSample(id10, 0.003f) == Catch::Approx(0.053421f).margin(0.0005f));
        // Active at elapsed = 0.43 s (tau = 0.59 < 0.60), expired at elapsed = 0.441 s (tau >= 0.60).
        CHECK(std::abs(evaluateCameraShakeSample(id10, 0.43f)) > 0.0f);
        CHECK(evaluateCameraShakeSample(id10, 0.441f) == 0.0f);
    }

    SECTION("ID=82 (Fel Reaver footstep): 12 in amplitude = 6x Kodo footstep") {
        const CameraShakeEntry id82{
            .id = 82,
            .shakeType = 1,
            .axis = 2,
            .amplitude = 12.0f,
            .frequency = 3.0f,
            .duration = 0.60f,
            .phase = 0.06f,
            .coefficient = 1.0f,
        };
        // Build 8606 telemetry measured +0.306984 yd close peak at elapsed ~= 0.0196 s.
        CHECK(evaluateCameraShakeSample(id82, 0.0196f) == Catch::Approx(0.306984f).margin(0.0003f));
        CHECK(evaluateCameraShakeSample(id82, 0.541f) == 0.0f);
    }

    SECTION("ID=103..105 (shakeType=0 SINE): constant envelope without exponential decay") {
        const CameraShakeEntry id105{
            .id = 105,
            .shakeType = 0,
            .axis = 2,
            .amplitude = 4.0f,
            .frequency = 6.0f,
            .duration = 1.0f,
            .phase = 0.0f,
            .coefficient = 0.4f,
        };
        // Quarter-cycle peaks at t = 1/24 s and t = 17/24 s both equal 4/36 = 0.11111 yd.
        CHECK(evaluateCameraShakeSample(id105, 1.0f / 24.0f) == Catch::Approx(4.0f / 36.0f).margin(0.0002f));
        CHECK(evaluateCameraShakeSample(id105, 17.0f / 24.0f) == Catch::Approx(4.0f / 36.0f).margin(0.0002f));
        CHECK(evaluateCameraShakeSample(id105, 1.0f) == 0.0f);
    }
}

TEST_CASE("3D camera-to-source distance attenuation matches Build 8606 telemetry", "[camera_shake]") {
    using wowee::rendering::cameraShakeDistanceAttenuation;

    CHECK(cameraShakeDistanceAttenuation(0.0f) == 1.0f);
    CHECK(cameraShakeDistanceAttenuation(1.86f) == 1.0f);
    CHECK(cameraShakeDistanceAttenuation(5.34f) == 1.0f);
    CHECK(cameraShakeDistanceAttenuation(9.0f) == 1.0f);

    struct TelemetrySample {
        float distanceYards;
        float measuredAtten;
    };
    // Measured directly from Native TBC 2.4.3 CGCamera memory telemetry (pair 20):
    const TelemetrySample samples[] = {
        {10.17f, 0.955f},
        {18.22f, 0.695f},
        {20.08f, 0.648f},
        {35.05f, 0.358f},
        {43.15f, 0.261f},
        {46.98f, 0.222f},
    };
    for (const auto& s : samples) {
        INFO("distance=" << s.distanceYards);
        CHECK(cameraShakeDistanceAttenuation(s.distanceYards) ==
              Catch::Approx(s.measuredAtten).margin(0.004f));
    }

    // Beyond falloff cutoff (100 yd): strictly zero shake.
    CHECK(cameraShakeDistanceAttenuation(99.5f) > 0.0f);
    CHECK(cameraShakeDistanceAttenuation(100.0f) == 0.0f);
    CHECK(cameraShakeDistanceAttenuation(150.0f) == 0.0f);
}

TEST_CASE("CameraShakeState combines same-axis shakes by max(|disp|) and maps camera basis", "[camera_shake]") {
    using wowee::rendering::CameraShakeEntry;
    using wowee::rendering::CameraShakeState;
    using wowee::rendering::cameraShakeBasisOffset;

    const CameraShakeEntry kodoStep{
        .id = 1,
        .shakeType = 1,
        .axis = 2,
        .amplitude = 2.0f,
        .frequency = 3.0f,
        .duration = 0.40f,
        .phase = 0.06f,
        .coefficient = 1.0f,
    };

    CameraShakeState state;
    state.trigger(kodoStep, 1.0f);
    float peakZ = 0.0f;
    // Advance 34 ms (Kodo Run $RL0 at 66 ms -> $RR1 at 100 ms), then trigger second footfall.
    for (int i = 0; i < 34; ++i) {
        peakZ = std::max(peakZ, std::abs(state.step(0.001f).z));
    }
    state.trigger(kodoStep, 1.0f);
    CHECK(state.size() == 2);

    for (int i = 0; i < 310; ++i) {
        peakZ = std::max(peakZ, std::abs(state.step(0.001f).z));
    }
    // Same-axis max(|val|) keeps peak at 1x (~0.05116 yd), never summing to ~0.10 yd.
    CHECK(peakZ == Catch::Approx(0.05116f).margin(0.0003f));
    // At ~344 ms total: first shake expired (> 0.34 s active lifetime), second shake still active.
    CHECK(state.size() == 1);
    for (int i = 0; i < 40; ++i) {
        (void)state.step(0.001f);
    }
    CHECK(state.empty());

    // Camera basis mapping at yaw = +90 deg (+Y forward) and -30 deg pitch:
    // axis 0 -> +Y, axis 1 -> -X (left of +Y), axis 2 -> pure world +Z (pitch-invariant).
    const glm::vec3 pitchedForward(0.0f, std::cos(glm::radians(-30.0f)), std::sin(glm::radians(-30.0f)));
    const glm::vec3 worldOffset = cameraShakeBasisOffset(glm::vec3(0.10f, 0.20f, 0.30f), pitchedForward);
    CHECK(worldOffset.x == Catch::Approx(-0.20f).margin(1e-5f));
    CHECK(worldOffset.y == Catch::Approx(0.10f).margin(1e-5f));
    CHECK(worldOffset.z == Catch::Approx(0.30f).margin(1e-5f));
}

TEST_CASE("Shipped TBC 2.4.3 DBC and M2 camera shake chain matches Kodo Graveyard & world shakes", "[camera_shake][assets]") {
    const auto tbcRoot = resolveTbcDataDir();
    if (!tbcRoot) {
        SKIP("TBC extracted assets unavailable; set WOWEE_TBC_DATA_DIR to run asset verification");
    }

    wowee::pipeline::DBCLayout layout;
#ifdef WOWEE_SOURCE_DIR
    const fs::path layoutPath = fs::path(WOWEE_SOURCE_DIR) / "Data/expansions/tbc/dbc_layouts.json";
    if (fs::exists(layoutPath)) {
        REQUIRE(layout.loadFromJson(layoutPath.string()));
        wowee::pipeline::setActiveDBCLayout(&layout);
    }
#endif

    const auto csDbc = loadTbcDbc(*tbcRoot, "CameraShakes.dbc");
    const auto secsDbc = loadTbcDbc(*tbcRoot, "SpellEffectCameraShakes.dbc");
    const auto cmdDbc = loadTbcDbc(*tbcRoot, "CreatureModelData.dbc");
    const auto cdiDbc = loadTbcDbc(*tbcRoot, "CreatureDisplayInfo.dbc");

    wowee::rendering::CameraShakeTable table;
    REQUIRE(table.load(&csDbc, &secsDbc, &cmdDbc));
    CHECK(table.shakeCount() == 62);
    CHECK(table.spellEffectShakeCount() == 24);
    CHECK(table.footstepModelCount() == 38);
    CHECK(table.deathThudModelCount() == 67);

    // Build CreatureDisplayInfo ID -> CreatureModelData ID map.
    std::unordered_map<uint32_t, uint32_t> displayToModel;
    for (uint32_t i = 0; i < cdiDbc.getRecordCount(); ++i) {
        displayToModel[cdiDbc.getUInt32(i, 0)] = cdiDbc.getUInt32(i, 1);
    }

    // All Desolace Kodo Graveyard kodo displays resolve to CreatureModelData 27 (KodoBeast.mdx),
    // which authors FootstepShakeSize = 1 and DeathThudShakeSize = 10.
    for (const uint32_t displayId : {2767u, 2768u, 10915u,   // Ancient Kodo (4702)
                                     1308u, 1232u, 10914u,   // Aged Kodo (4700)
                                     1453u, 1241u, 10917u})  // Dying Kodo (4701)
    {
        INFO("displayId=" << displayId);
        REQUIRE(displayToModel.count(displayId) == 1);
        CHECK(displayToModel.at(displayId) == 27u);
    }
    CHECK(table.footstepShakeId(27) == 1);
    CHECK(table.deathThudShakeId(27) == 10);

    // Player Kodo mounts must NOT shake on footsteps or death.
    CHECK(table.footstepShakeId(1711) == 0);   // RidingKodo.mdx
    CHECK(table.deathThudShakeId(1711) == 0);
    CHECK(table.footstepShakeId(1914) == 0);   // KodoBeastPvPT2.mdx
    CHECK(table.deathThudShakeId(1914) == 0);

    // General TBC creature model coverage (giants, Fel Reaver, bosses).
    CHECK(table.footstepShakeId(205) == 2);    // MountainGiant
    CHECK(table.deathThudShakeId(205) == 11);
    CHECK(table.footstepShakeId(2272) == 82);  // FelReaver
    CHECK(table.deathThudShakeId(2272) == 12);
    CHECK(table.footstepShakeId(2224) == 0);   // Body of C'Thun (death-only shake)
    CHECK(table.deathThudShakeId(2224) == 38);

    // Verify shipped KodoBeast.m2 has 4 per-foot footfall events in Walk/Run and $DTH at 1800 ms in Death.
    const auto kodoM2 = loadTbcM2(*tbcRoot, "Creature/Kodobeast/KodoBeast.m2");
    const int walkSeq = primarySequenceIndex(kodoM2, 4);
    const int runSeq = primarySequenceIndex(kodoM2, 5);
    const int deathSeq = primarySequenceIndex(kodoM2, 1);
    REQUIRE(walkSeq >= 0);
    REQUIRE(runSeq >= 0);
    REQUIRE(deathSeq >= 0);

    size_t walkFootfalls = 0;
    size_t runFootfalls = 0;
    for (const auto& ev : kodoM2.footfallEvents) {
        if (static_cast<size_t>(walkSeq) < ev.times.size()) walkFootfalls += ev.times[walkSeq].size();
        if (static_cast<size_t>(runSeq) < ev.times.size()) runFootfalls += ev.times[runSeq].size();
    }
    CHECK(walkFootfalls == 4);
    CHECK(runFootfalls == 4);
    REQUIRE(static_cast<size_t>(deathSeq) < kodoM2.deathThudEventTimes.size());
    REQUIRE(kodoM2.deathThudEventTimes[deathSeq].size() == 1);
    CHECK(kodoM2.deathThudEventTimes[deathSeq][0] == 1800u);

    // Verify shipped M2 $SHK events: meteor_impact_base.m2 -> SpellEffectCameraShakes ID=2 at 1000 ms;
    // g_bombwagon.m2 -> SpellEffectCameraShakes ID=3; felreaver.m2 ($SHK data=0) -> ignored.
    const auto meteorM2 = loadTbcM2(*tbcRoot, "Spells/Meteor_Impact_Base.m2");
    REQUIRE(meteorM2.cameraShakeEvents.size() == 1);
    CHECK(meteorM2.cameraShakeEvents[0].spellEffectShakeId == 2u);
    const int meteorStand = primarySequenceIndex(meteorM2, 0);
    REQUIRE(meteorStand >= 0);
    REQUIRE(meteorM2.cameraShakeEvents[0].times[meteorStand].size() == 1);
    CHECK(meteorM2.cameraShakeEvents[0].times[meteorStand][0] == 1000u);
    const auto meteorShakes = table.findSpellEffectShakes(2u);
    CHECK((meteorShakes[0] != 0 || meteorShakes[1] != 0 || meteorShakes[2] != 0));

    const auto wagonM2 = loadTbcM2(*tbcRoot, "World/Goober/G_BombWagon.m2");
    REQUIRE_FALSE(wagonM2.cameraShakeEvents.empty());
    CHECK(wagonM2.cameraShakeEvents[0].spellEffectShakeId == 3u);

    const auto felReaverM2 = loadTbcM2(*tbcRoot, "Creature/FelReaver/FelReaver.m2");
    CHECK(felReaverM2.cameraShakeEvents.empty());

    wowee::pipeline::setActiveDBCLayout(nullptr);
}
