// Footprints come from the authored per-foot footfall events of each model
// ($FL0/$FR1/$RL0/... at the foot, z = 0), one print per event.
//
// The asset cases read an extracted TBC tree:
//   WOWEE_TBC_DATA_DIR=/path/to/client/wowee-data/expansions/tbc
#include <catch_amalgamated.hpp>

#include "pipeline/m2_loader.hpp"
#include "rendering/footfall_window.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

using wowee::rendering::footfallPassed;

namespace {

std::vector<uint8_t> readBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

wowee::pipeline::M2Model loadModel(const char* relative) {
    const char* root = std::getenv("WOWEE_TBC_DATA_DIR");
    if (!root || !*root) SKIP("Set WOWEE_TBC_DATA_DIR to client/wowee-data/expansions/tbc");
    const auto bytes = readBytes(std::filesystem::path(root) / relative);
    REQUIRE_FALSE(bytes.empty());
    return wowee::pipeline::M2Loader::load(bytes);
}

int sequenceOf(const wowee::pipeline::M2Model& model, uint32_t animationId) {
    for (size_t i = 0; i < model.sequences.size(); ++i)
        if (model.sequences[i].id == animationId && model.sequences[i].variationIndex == 0)
            return static_cast<int>(i);
    return -1;
}

} // namespace

TEST_CASE("A footfall fires once when a frame passes it", "[footfall]") {
    CHECK(footfallPassed(134, 100.0f, 150.0f));
    CHECK_FALSE(footfallPassed(134, 134.0f, 150.0f));  // already printed last frame
    CHECK(footfallPassed(0, -1.0f, 16.0f));            // event on a new sequence's first frame
    CHECK(footfallPassed(10, 1320.0f, 20.0f));         // loop wrapped past it
    CHECK(footfallPassed(1330, 1320.0f, 20.0f));
    CHECK_FALSE(footfallPassed(500, 1320.0f, 20.0f));
}

TEST_CASE("Bear walk prints each of four paws at its own place", "[footfall][assets]") {
    const auto model = loadModel("creature/bear/bear.m2");
    const int walk = sequenceOf(model, 4);
    REQUIRE(walk >= 0);
    std::vector<const wowee::pipeline::M2Model::FootfallEvent*> walking;
    for (const auto& event : model.footfallEvents)
        if (!event.times.at(walk).empty()) walking.push_back(&event);
    REQUIRE(walking.size() == 4);
    int front = 0, left = 0;
    for (const auto* event : walking) {
        CHECK(event->position.z == Catch::Approx(0.0f).margin(0.01f));
        front += event->position.x > 0.0f;
        left += event->left;
    }
    CHECK(front == 2);  // two fore paws ahead of the origin, two hind behind
    CHECK(left == 2);
}

TEST_CASE("Footfall side letter agrees with the foot's side", "[footfall][assets]") {
    // Model +Y is the unit's left. Checked over quadrupeds and bipeds.
    for (const char* path : {"creature/bear/bear.m2", "creature/wolf/wolf.m2", "creature/tiger/tiger.m2",
                             "creature/horse/horse.m2", "character/human/male/humanmale.m2",
                             "character/gnome/male/gnomemale.m2"}) {
        INFO(path);
        const auto model = loadModel(path);
        REQUIRE_FALSE(model.footfallEvents.empty());
        for (const auto& event : model.footfallEvents)
            CHECK(event.left == (event.position.y > 0.0f));
    }
}

TEST_CASE("Footprint opacity holds 0.5x for 3s then fades linearly to 0 at 6s", "[footfall][footprint]") {
    using wowee::rendering::footprintOpacity;
    using wowee::rendering::kFootprintPeakOpacity;
    CHECK(footprintOpacity(0.0f) == Catch::Approx(0.5f));
    CHECK(footprintOpacity(1.5f) == Catch::Approx(kFootprintPeakOpacity));
    CHECK(footprintOpacity(3.0f) == Catch::Approx(0.5f));
    CHECK(footprintOpacity(3.75f) == Catch::Approx(0.375f));
    CHECK(footprintOpacity(4.5f) == Catch::Approx(0.25f));
    CHECK(footprintOpacity(5.25f) == Catch::Approx(0.125f));
    CHECK(footprintOpacity(6.0f) == Catch::Approx(0.0f));
    CHECK(footprintOpacity(10.0f) == Catch::Approx(0.0f));
}
