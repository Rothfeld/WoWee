// The data behind a meleeing bear's idle: Bear.m2 has no ReadyUnarmed, and
// AnimationData.dbc falls ReadyUnarmed back to Stand - the plain Stand the
// original holds between swings, not the rearing Stand fidget variation.
//
//   WOWEE_TBC_DATA_DIR=/path/to/client/wowee-data/expansions/tbc
#include <catch_amalgamated.hpp>

#include "pipeline/dbc_loader.hpp"
#include "pipeline/m2_loader.hpp"
#include "rendering/animation/animation_ids.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>

using namespace wowee;

namespace {
std::vector<uint8_t> readData(const char* relative) {
    const char* root = std::getenv("WOWEE_TBC_DATA_DIR");
    if (!root || !*root) SKIP("Set WOWEE_TBC_DATA_DIR to client/wowee-data/expansions/tbc");
    std::ifstream in(std::filesystem::path(root) / relative, std::ios::binary);
    std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(in), {}};
    REQUIRE_FALSE(bytes.empty());
    return bytes;
}
}

TEST_CASE("AnimationData Fallback column chains ready stances to Stand", "[animation][assets]") {
    pipeline::DBCFile dbc;
    REQUIRE(dbc.load(readData("dbfilesclient/animationdata.dbc")));
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
    const auto model = pipeline::M2Loader::load(readData("creature/bear/bear.m2"));
    int stands = 0;
    for (const auto& sequence : model.sequences) {
        CHECK(sequence.id != rendering::anim::READY_UNARMED);
        stands += sequence.id == rendering::anim::STAND;
    }
    CHECK(stands == 2);  // plain Stand and the 3.8 s rearing fidget
}
