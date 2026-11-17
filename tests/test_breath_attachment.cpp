// A SpellVisualKit BreathEffect is placed on M2 attachment 17 (Breath) and
// aimed along it. Bug #76: Flame Spray (spell 10733, visual 2049, cast kit
// 1444) from the Inferno Elemental (display 5488) shot sideways because the
// effect was spawned unrotated at the caster's feet.
//
// Checks the authored data that rule rests on, from an extracted TBC tree:
//   WOWEE_TBC_DATA_DIR=/path/to/client/wowee-data/expansions/tbc
#include <catch_amalgamated.hpp>

#include "pipeline/m2_loader.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace {

std::vector<uint8_t> readBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

} // namespace

TEST_CASE("Fire elemental breath attachment sits in front of the face", "[spell-visual][assets]") {
    const char* root = std::getenv("WOWEE_TBC_DATA_DIR");
    if (!root || !*root) SKIP("Set WOWEE_TBC_DATA_DIR to client/wowee-data/expansions/tbc");
    const auto bytes = readBytes(std::filesystem::path(root) / "creature/fireelemental/fireelemental.m2");
    REQUIRE_FALSE(bytes.empty());
    const auto model = wowee::pipeline::M2Loader::load(bytes);
    REQUIRE(model.attachmentLookup.size() > 17);
    const uint16_t index = model.attachmentLookup[17];
    REQUIRE(index < model.attachments.size());
    const auto& breath = model.attachments[index];
    CHECK(breath.id == 17);
    REQUIRE(breath.bone < model.bones.size());
    // Model +X is the unit's forward; the mouth is ahead of the body and up.
    const glm::vec3 bindPosition = breath.position;
    CHECK(bindPosition.x > 0.5f);
    CHECK(bindPosition.z > 1.0f);
}

TEST_CASE("Dragon flame breath sprays along its model +X", "[spell-visual][assets]") {
    const char* root = std::getenv("WOWEE_TBC_DATA_DIR");
    if (!root || !*root) SKIP("Set WOWEE_TBC_DATA_DIR to client/wowee-data/expansions/tbc");
    const auto bytes = readBytes(std::filesystem::path(root) / "spells/dragonflamebreath.m2");
    REQUIRE_FALSE(bytes.empty());
    const auto model = wowee::pipeline::M2Loader::load(bytes);
    // The authored bounds run far ahead along +X and only a little behind.
    CHECK(model.vertexBoxMax.x > 50.0f);
    CHECK(model.vertexBoxMin.x > -15.0f);
    CHECK(model.vertexBoxMax.y < model.vertexBoxMax.x);
}
