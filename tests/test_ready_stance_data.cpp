// The data behind a meleeing bear's idle: Bear.m2 has no ReadyUnarmed, and
// AnimationData.dbc falls ReadyUnarmed back to Stand - the plain Stand the
// original holds between swings, not the rearing Stand fidget variation.
//
//   WOWEE_TBC_DATA_DIR=/path/to/client/wowee-data/expansions/tbc
#include <catch_amalgamated.hpp>

#include "pipeline/dbc_loader.hpp"
#include "pipeline/m2_loader.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "rendering/animation/sequence_variation.hpp"
#include "tbc_data_test_util.hpp"

#include <map>
#include <random>

using namespace wowee;

TEST_CASE("AnimationData Fallback column chains ready stances to Stand", "[animation][assets]") {
    const auto raw = test::readTbcData("dbfilesclient/animationdata.dbc");
    pipeline::DBCFile dbc;
    REQUIRE(dbc.load(raw));
    const auto fallback = [&](uint32_t id) {
        const int32_t row = dbc.findRecordById(id);
        REQUIRE(row >= 0);
        return dbc.getUInt32(static_cast<uint32_t>(row), 6);  // layout AnimationData.Fallback
    };
    CHECK(fallback(rendering::anim::READY_1H) == rendering::anim::READY_UNARMED);
    CHECK(fallback(rendering::anim::READY_2H) == rendering::anim::READY_UNARMED);
    CHECK(fallback(rendering::anim::READY_UNARMED) == rendering::anim::STAND);
    CHECK(fallback(rendering::anim::WALK_BACKWARDS) == rendering::anim::WALK);
    CHECK(fallback(rendering::anim::LOOT_HOLD) == rendering::anim::LOOT);
}

TEST_CASE("Bear.m2 has no ready stance and a second Stand variation", "[animation][assets]") {
    const auto model = pipeline::M2Loader::load(test::readTbcData("creature/bear/bear.m2"));
    int stands = 0;
    for (const auto& sequence : model.sequences) {
        CHECK(sequence.id != rendering::anim::READY_UNARMED);
        stands += sequence.id == rendering::anim::STAND;
    }
    CHECK(stands == 2);  // plain Stand and the 3.8 s rearing fidget
}

TEST_CASE("DoomGuardOutland.m2 walks on two variations, Akama.m2 on one", "[animation][assets]") {
    // Native Illidari Fearbringers (22954, Black Temple) switch between
    // walking on their feet and beating their wings along a patrol.
    const auto doomguard = pipeline::M2Loader::load(test::readTbcData("creature/doomguardoutland/doomguardoutland.m2"));
    std::mt19937 random(8606);
    std::map<int, int> drawn;
    for (int i = 0; i < 2000; ++i) {
        const auto variation = rendering::drawSequenceVariation(doomguard.sequences, rendering::anim::WALK, random);
        REQUIRE(variation);
        ++drawn[variation->index];
        const auto& sequence = doomguard.sequences[variation->index];
        CHECK(sequence.id == rendering::anim::WALK);
        CHECK(variation->repeats >= sequence.replayMin);
        CHECK(variation->repeats <= sequence.replayMax);
    }
    REQUIRE(drawn.size() == 2);
    CHECK(drawn.count(0) == 1);
    CHECK(drawn.count(33) == 1);
    CHECK(drawn[0] > 800);   // frequencies 16383 : 16383
    CHECK(drawn[33] > 800);
    CHECK(doomguard.sequences[33].replayMin == 3);
    CHECK(doomguard.sequences[33].replayMax == 5);

    const auto akama = pipeline::M2Loader::load(test::readTbcData("creature/akama/akama.m2"));
    const auto walk = rendering::drawSequenceVariation(akama.sequences, rendering::anim::WALK, random);
    REQUIRE(walk);
    CHECK(walk->index == 0);
    CHECK(walk->repeats == 0);
    CHECK_FALSE(rendering::drawSequenceVariation(akama.sequences, rendering::anim::FLY_IDLE, random));
}
