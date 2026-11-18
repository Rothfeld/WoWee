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

// SpellVisualKit SpecialEffect0 sits on M2 attachment 23 (Special1), which
// dragons carry at the mouth: Onyxia's Flame Breath 18435 (cast kit 4450,
// dragonflamebreath.m2) and Ebonroc's Shadow Flame 22539 (kit 5851,
// nefarianflamebreath.m2) come out there and turn with that bone.
TEST_CASE("Dragon Special1 attachment sits at the mouth", "[spell-visual][assets]") {
    const char* root = std::getenv("WOWEE_TBC_DATA_DIR");
    if (!root || !*root) SKIP("Set WOWEE_TBC_DATA_DIR to client/wowee-data/expansions/tbc");
    for (const char* file : {"creature/dragon/dragononyxia.m2", "creature/drake/drake.m2"}) {
        INFO(file);
        const auto bytes = readBytes(std::filesystem::path(root) / file);
        REQUIRE_FALSE(bytes.empty());
        const auto model = wowee::pipeline::M2Loader::load(bytes);
        REQUIRE(model.attachmentLookup.size() > 23);
        const uint16_t index = model.attachmentLookup[23];
        REQUIRE(index < model.attachments.size());
        const auto& special = model.attachments[index];
        CHECK(special.id == 23);
        // Ahead of the body origin, above the ground, forward of the chest (34).
        CHECK(special.position.x > 0.5f);
        CHECK(special.position.z > 1.0f);
        const uint16_t chest = model.attachmentLookup[34];
        REQUIRE(chest < model.attachments.size());
        CHECK(special.position.x > model.attachments[chest].position.x);
    }
}
