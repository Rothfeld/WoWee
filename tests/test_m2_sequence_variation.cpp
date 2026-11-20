#include <catch_amalgamated.hpp>

#include "core/game_object_animation_policy.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/m2_loader.hpp"
#include "rendering/m2_sequence_variation.hpp"
#include "rendering/m2_track_sampler.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using wowee::pipeline::DBCFile;
using wowee::pipeline::M2Loader;
using wowee::pipeline::M2Model;
using wowee::pipeline::M2Sequence;
using wowee::rendering::collectSequenceVariations;
using wowee::rendering::selectSequenceVariation;
namespace m2_track = wowee::rendering::m2_track;

namespace {

std::vector<uint8_t> readBinaryFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    REQUIRE(in.is_open());
    const auto size = in.tellg();
    REQUIRE(size > 0);
    std::vector<uint8_t> buf(static_cast<size_t>(size));
    in.seekg(0, std::ios::beg);
    in.read(reinterpret_cast<char*>(buf.data()), size);
    REQUIRE(in.good());
    return buf;
}

std::filesystem::path tbcDataRoot() {
    const char* env = std::getenv("WOWEE_TBC_DATA_DIR");
    if (!env || !*env) return {};
    return std::filesystem::path(env);
}

M2Sequence makeSeq(uint32_t id, uint32_t duration, int16_t freq, int16_t nextAnim) {
    M2Sequence s{};
    s.id = id;
    s.duration = duration;
    s.frequency = freq;
    s.nextAnimation = nextAnim;
    return s;
}

} // namespace

TEST_CASE("selectSequenceVariation honors nextAnimation chain and frequency weights", "[m2-variation]") {
    std::vector<M2Sequence> seqs = {
        makeSeq(0, 3300, 16384, 1),
        makeSeq(0, 3334, 16383, -1),
    };

    const auto chain = collectSequenceVariations(seqs, 0);
    REQUIRE(chain == std::vector<int>({0, 1}));

    // Rolls 0..16383 pick seq 0; 16384..32766 pick seq 1.
    CHECK(selectSequenceVariation(seqs, 0, 0) == 0);
    CHECK(selectSequenceVariation(seqs, 0, 16383) == 0);
    CHECK(selectSequenceVariation(seqs, 0, 16384) == 1);
    CHECK(selectSequenceVariation(seqs, 0, 32766) == 1);
    // Wraps cleanly modulo total weight (32767).
    CHECK(selectSequenceVariation(seqs, 0, 32767) == 0);
}

TEST_CASE("selectSequenceVariation guards against cycles and zero weights", "[m2-variation]") {
    SECTION("cycle in nextAnimation terminates") {
        std::vector<M2Sequence> seqs = {
            makeSeq(0, 1000, 100, 1),
            makeSeq(0, 1000, 300, 0),
        };
        const auto chain = collectSequenceVariations(seqs, 0);
        REQUIRE(chain == std::vector<int>({0, 1}));
        CHECK(selectSequenceVariation(seqs, 0, 99) == 0);
        CHECK(selectSequenceVariation(seqs, 0, 100) == 1);
    }

    SECTION("unlinked same-id variations fall back to scanning all matching sequences") {
        std::vector<M2Sequence> seqs = {
            makeSeq(0, 2000, 300, -1),
            makeSeq(148, 1000, 32767, -1),
            makeSeq(0, 2500, 100, -1),
        };
        const auto chain = collectSequenceVariations(seqs, 0);
        REQUIRE(chain == std::vector<int>({0, 2}));
        CHECK(selectSequenceVariation(seqs, 0, 299) == 0);
        CHECK(selectSequenceVariation(seqs, 0, 300) == 2);
    }

    SECTION("zero frequency weights fall back to uniform selection") {
        std::vector<M2Sequence> seqs = {
            makeSeq(0, 1000, 0, 1),
            makeSeq(0, 1000, 0, -1),
        };
        CHECK(selectSequenceVariation(seqs, 0, 0) == 0);
        CHECK(selectSequenceVariation(seqs, 0, 1) == 1);
        CHECK(selectSequenceVariation(seqs, 0, 2) == 0);
    }
}

TEST_CASE("Real TBC BloodOfHeroes.m2 resolves from GameObjectDisplayInfo and selects bubbling variation", "[m2-variation][tbc-data]") {
    const auto root = tbcDataRoot();
    if (root.empty() || !std::filesystem::exists(root)) {
        SKIP("WOWEE_TBC_DATA_DIR not set");
    }

    // 1. Resolve displayId 4211 (Blood of Heroes, entry 176213, type 3 CHEST)
    //    via GameObjectDisplayInfo.dbc.
    const auto dbcPath = root / "dbfilesclient" / "gameobjectdisplayinfo.dbc";
    REQUIRE(std::filesystem::exists(dbcPath));
    DBCFile dbc;
    REQUIRE(dbc.load(readBinaryFile(dbcPath)));

    std::string modelRel;
    for (uint32_t r = 0; r < dbc.getRecordCount(); ++r) {
        if (dbc.getUInt32(r, 0) == 4211u) {
            modelRel = dbc.getString(r, 1);
            break;
        }
    }
    REQUIRE_FALSE(modelRel.empty());

    std::string normalized = modelRel;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    if (normalized.ends_with(".mdx") || normalized.ends_with(".mdl")) {
        normalized.replace(normalized.size() - 4, 4, ".m2");
    }

    const auto m2Path = root / normalized;
    REQUIRE(std::filesystem::exists(m2Path));

    const M2Model model = M2Loader::load(readBinaryFile(m2Path));
    REQUIRE(model.sequences.size() == 2);

    // No state sequences (Close/Closed/Open/Opened/Destroy) — CHEST keeps Stand running.
    for (uint32_t stateId : {146u, 147u, 148u, 149u, 150u}) {
        const bool hasState = std::any_of(model.sequences.begin(), model.sequences.end(),
                                          [stateId](const M2Sequence& s) { return s.id == stateId; });
        CHECK_FALSE(hasState);
    }
    CHECK_FALSE(wowee::core::gameObjectBindPoseShowsReady(3u));

    // Authored Stand variations: seq 0 (quiet 3300ms, weight 16384) -> seq 1 (bubbling 3334ms, weight 16383).
    CHECK(model.sequences[0].id == 0u);
    CHECK(model.sequences[1].id == 0u);
    CHECK(model.sequences[0].duration == 3300u);
    CHECK(model.sequences[1].duration == 3334u);
    CHECK(model.sequences[0].nextAnimation == 1);
    CHECK(model.sequences[1].nextAnimation == -1);
    CHECK(model.sequences[0].frequency == 16384);
    CHECK(model.sequences[1].frequency == 16383);

    const auto vars = collectSequenceVariations(model.sequences, 0);
    REQUIRE(vars == std::vector<int>({0, 1}));
    CHECK(selectSequenceVariation(model.sequences, 0, 0) == 0);
    CHECK(selectSequenceVariation(model.sequences, 0, 16383) == 0);
    CHECK(selectSequenceVariation(model.sequences, 0, 16384) == 1);
    CHECK(selectSequenceVariation(model.sequences, 0, 32766) == 1);

    // Verify seq 0 is flat while seq 1 drives both bone scale (> 2.5x) and UV V shift (~0.605).
    REQUIRE(model.textureTransforms.size() == 5);
    const glm::vec3 uvSeq0 = m2_track::sampleVec3(
        model.textureTransforms[0].translation, 0, 200.0f, 0.0f,
        model.globalSequenceDurations, glm::vec3(0.0f));
    const glm::vec3 uvSeq1 = m2_track::sampleVec3(
        model.textureTransforms[0].translation, 1, 200.0f, 0.0f,
        model.globalSequenceDurations, glm::vec3(0.0f));
    CHECK(uvSeq0.y == Catch::Approx(0.0f).margin(1e-4f));
    CHECK(uvSeq1.y == Catch::Approx(0.6050565f).margin(1e-3f));

    REQUIRE(!model.bones.empty());
    const glm::vec3 scaleSeq0 = m2_track::sampleVec3(
        model.bones[0].scale, 0, 834.0f, 0.0f,
        model.globalSequenceDurations, glm::vec3(1.0f));
    const glm::vec3 scaleSeq1 = m2_track::sampleVec3(
        model.bones[0].scale, 1, 834.0f, 0.0f,
        model.globalSequenceDurations, glm::vec3(1.0f));
    CHECK(scaleSeq0.x == Catch::Approx(1.0f).margin(1e-3f));
    CHECK(scaleSeq1.x > 2.5f);
}

TEST_CASE("Real TBC weighted Stand variation models follow authored weights", "[m2-variation][tbc-data]") {
    const auto root = tbcDataRoot();
    if (root.empty() || !std::filesystem::exists(root)) {
        SKIP("WOWEE_TBC_DATA_DIR not set");
    }

    const auto fishPath = root / "world" / "skillactivated" / "tradeskillnodes" / "tradeskill_fishschool_01.m2";
    if (std::filesystem::exists(fishPath)) {
        const M2Model fish = M2Loader::load(readBinaryFile(fishPath));
        const auto vars = collectSequenceVariations(fish.sequences, 0);
        REQUIRE(vars.size() == 2);
        // 24576 / 32767 (~75%) vs 8191 / 32767 (~25%)
        CHECK(selectSequenceVariation(fish.sequences, 0, 24575) == vars[0]);
        CHECK(selectSequenceVariation(fish.sequences, 0, 24576) == vars[1]);
    }
}

TEST_CASE("Real TBC Silithyst Geyser (GOOBER 181598) keeps Stand running and erupts in variation 1", "[m2-variation][tbc-data]") {
    const auto root = tbcDataRoot();
    if (root.empty() || !std::filesystem::exists(root)) {
        SKIP("WOWEE_TBC_DATA_DIR not set");
    }

    // Resolve displayId 6862 (Silithyst Geyser, entry 181598, type 10 GOOBER)
    // via GameObjectDisplayInfo.dbc.
    const auto dbcPath = root / "dbfilesclient" / "gameobjectdisplayinfo.dbc";
    REQUIRE(std::filesystem::exists(dbcPath));
    DBCFile dbc;
    REQUIRE(dbc.load(readBinaryFile(dbcPath)));

    std::string modelRel;
    for (uint32_t r = 0; r < dbc.getRecordCount(); ++r) {
        if (dbc.getUInt32(r, 0) == 6862u) {
            modelRel = dbc.getString(r, 1);
            break;
        }
    }
    REQUIRE_FALSE(modelRel.empty());

    std::string normalized = modelRel;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    if (normalized.ends_with(".mdx") || normalized.ends_with(".mdl")) {
        normalized.replace(normalized.size() - 4, 4, ".m2");
    }

    const auto m2Path = root / normalized;
    REQUIRE(std::filesystem::exists(m2Path));

    const M2Model model = M2Loader::load(readBinaryFile(m2Path));
    REQUIRE(model.sequences.size() == 2);
    REQUIRE(model.particleEmitters.size() == 4);

    // No state/destroy sequences — GOOBER keeps Stand running in READY.
    for (uint32_t stateId : {146u, 147u, 148u, 149u, 150u}) {
        const bool hasState = std::any_of(model.sequences.begin(), model.sequences.end(),
                                          [stateId](const M2Sequence& s) { return s.id == stateId; });
        CHECK_FALSE(hasState);
    }
    CHECK_FALSE(wowee::core::gameObjectBindPoseShowsReady(10u));

    // Authored Stand variations: seq 0 (quiet 3000ms, weight 22937 ~70%) ->
    // seq 1 (spouting 3334ms, weight 9830 ~30%).
    CHECK(model.sequences[0].id == 0u);
    CHECK(model.sequences[1].id == 0u);
    CHECK(model.sequences[0].duration == 3000u);
    CHECK(model.sequences[1].duration == 3334u);
    CHECK(model.sequences[0].nextAnimation == 1);
    CHECK(model.sequences[1].nextAnimation == -1);
    CHECK(model.sequences[0].frequency == 22937);
    CHECK(model.sequences[1].frequency == 9830);

    const auto vars = collectSequenceVariations(model.sequences, 0);
    REQUIRE(vars == std::vector<int>({0, 1}));
    CHECK(selectSequenceVariation(model.sequences, 0, 0) == 0);
    CHECK(selectSequenceVariation(model.sequences, 0, 22936) == 0);
    CHECK(selectSequenceVariation(model.sequences, 0, 22937) == 1);
    CHECK(selectSequenceVariation(model.sequences, 0, 32766) == 1);

    // In seq 0 (quiet), only emitter 1 (global sequence 0 ambient wisp) emits;
    // emitters 0, 2, 3 are silent (<= 0).
    CHECK(m2_track::sampleFloat(model.particleEmitters[0].emissionRate, 0, 1000.0f, 0.0f,
                                model.globalSequenceDurations, 0.0f) <= 0.0f);
    CHECK(m2_track::sampleFloat(model.particleEmitters[1].emissionRate, 0, 1000.0f, 100.0f,
                                model.globalSequenceDurations, 0.0f) > 0.0f);
    CHECK(m2_track::sampleFloat(model.particleEmitters[2].emissionRate, 0, 1000.0f, 0.0f,
                                model.globalSequenceDurations, 0.0f) <= 0.0f);
    CHECK(m2_track::sampleFloat(model.particleEmitters[3].emissionRate, 0, 1000.0f, 0.0f,
                                model.globalSequenceDurations, 0.0f) <= 0.0f);

    // In seq 1 (spouting, timeOffset=3333ms):
    //   t=567ms (3900ms): emitter 0 = 5/s, emitter 2 = 100/s
    //   t=634ms (3967ms): emitter 3 = 80/s
    CHECK(m2_track::sampleFloat(model.particleEmitters[0].emissionRate, 1, 567.0f, 0.0f,
                                model.globalSequenceDurations, 0.0f) == Catch::Approx(5.0f).margin(0.1f));
    CHECK(m2_track::sampleFloat(model.particleEmitters[2].emissionRate, 1, 567.0f, 0.0f,
                                model.globalSequenceDurations, 0.0f) == Catch::Approx(100.0f).margin(1.0f));
    CHECK(m2_track::sampleFloat(model.particleEmitters[3].emissionRate, 1, 634.0f, 0.0f,
                                model.globalSequenceDurations, 0.0f) == Catch::Approx(80.0f).margin(1.0f));
}

TEST_CASE("Real TBC world geyser doodads (Un'Goro & SteamGeyser) erupt in Stand variations", "[m2-variation][tbc-data]") {
    const auto root = tbcDataRoot();
    if (root.empty() || !std::filesystem::exists(root)) {
        SKIP("WOWEE_TBC_DATA_DIR not set");
    }

    const auto ungoroPath = root / "world" / "kalimdor" / "ungoro" / "passivedoodads" / "geyser" / "ungorogeyser01.m2";
    REQUIRE(std::filesystem::exists(ungoroPath));
    const M2Model ungoro = M2Loader::load(readBinaryFile(ungoroPath));
    REQUIRE(ungoro.sequences.size() == 3);
    REQUIRE(ungoro.particleEmitters.size() == 6);
    CHECK(collectSequenceVariations(ungoro.sequences, 0) == std::vector<int>({0, 1, 2}));
    CHECK(selectSequenceVariation(ungoro.sequences, 0, 22937) == 0);
    CHECK(selectSequenceVariation(ungoro.sequences, 0, 22938) == 1);
    CHECK(selectSequenceVariation(ungoro.sequences, 0, 22938 + 3276) == 2);
    CHECK(m2_track::sampleFloat(ungoro.particleEmitters[2].emissionRate, 0, 1000.0f, 0.0f,
                                ungoro.globalSequenceDurations, 0.0f) <= 0.0f);
    CHECK(m2_track::sampleFloat(ungoro.particleEmitters[2].emissionRate, 1, 567.0f, 0.0f,
                                ungoro.globalSequenceDurations, 0.0f) == Catch::Approx(100.0f).margin(1.0f));

    const auto steamPath = root / "world" / "generic" / "orc" / "passive doodads" / "geyser" / "steamgeyser.m2";
    REQUIRE(std::filesystem::exists(steamPath));
    const M2Model steam = M2Loader::load(readBinaryFile(steamPath));
    REQUIRE(steam.sequences.size() == 2);
    REQUIRE(steam.particleEmitters.size() == 3);
    CHECK(collectSequenceVariations(steam.sequences, 0) == std::vector<int>({0, 1}));
    CHECK(selectSequenceVariation(steam.sequences, 0, 29490) == 0);
    CHECK(selectSequenceVariation(steam.sequences, 0, 29491) == 1);
    CHECK(m2_track::sampleFloat(steam.particleEmitters[2].emissionRate, 0, 500.0f, 0.0f,
                                steam.globalSequenceDurations, 0.0f) <= 0.0f);
    CHECK(m2_track::sampleFloat(steam.particleEmitters[2].emissionRate, 1, 1900.0f, 0.0f,
                                steam.globalSequenceDurations, 0.0f) == Catch::Approx(200.0f).margin(1.0f));
}
