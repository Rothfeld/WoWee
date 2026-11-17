// The unit pick rule shared by the client and tools/pick_mask, on Bear.m2
// standing at the origin.
//   WOWEE_TBC_DATA_DIR=/path/to/client/wowee-data/expansions/tbc
#include <catch_amalgamated.hpp>

#include "pipeline/m2_loader.hpp"
#include "rendering/unit_pick.hpp"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>

using namespace wowee;

TEST_CASE("Bear.m2 is picked through its body and not beside it", "[unit-pick][assets]") {
    const char* root = std::getenv("WOWEE_TBC_DATA_DIR");
    if (!root || !*root) SKIP("Set WOWEE_TBC_DATA_DIR to client/wowee-data/expansions/tbc");
    std::ifstream in(std::filesystem::path(root) / "creature/bear/bear.m2", std::ios::binary);
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(in), {}};
    const auto model = pipeline::M2Loader::load(bytes);
    REQUIRE(model.isValid());
    int stand = -1;
    for (size_t i = 0; i < model.sequences.size(); ++i)
        if (model.sequences[i].id == 0 && model.sequences[i].variationIndex == 0) stand = static_cast<int>(i);
    REQUIRE(stand >= 0);
    const auto bones = rendering::poseModelBones(model, stand, 0.0f, 0.0f);
    REQUIRE(bones.size() == model.bones.size());
    const rendering::UnitPickState state{.bones = bones, .sequenceIndex = stand};
    constexpr float none = std::numeric_limits<float>::infinity();

    // From 10 yards out along +Y (the bear's side), level with its body.
    const glm::vec3 eye(0.0f, 10.0f, 0.8f);
    const float body = rendering::pickUnitModel(model, state, eye, glm::vec3(0, -1, 0), none);
    CHECK(std::isfinite(body));
    CHECK(body > 8.0f);
    CHECK(body < 10.0f);
    // Two yards above its back, and three yards behind it, nothing.
    CHECK_FALSE(std::isfinite(rendering::pickUnitModel(model, state, glm::vec3(0, 10, 4.5f),
                                                       glm::vec3(0, -1, 0), none)));
    CHECK_FALSE(std::isfinite(rendering::pickUnitModel(model, state, glm::vec3(-5, 10, 0.8f),
                                                       glm::vec3(0, -1, 0), none)));
}
