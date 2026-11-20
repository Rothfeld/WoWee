#include <catch_amalgamated.hpp>

#include "pipeline/m2_loader.hpp"
#include "ui/autocast_shine.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace wowee;
using namespace wowee::ui;
using Catch::Approx;

namespace {

std::vector<uint8_t> readFileBytes(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
}

std::filesystem::path shippedAutoCastM2() {
    return "/workspace/azerothcore/client/wowee-data/expansions/tbc/interface/buttons/ui-autocastbutton.m2";
}

} // namespace

TEST_CASE("AutoCastShineModel loads shipped TBC UI-AutoCastButton.m2", "[ui][autocast]") {
    const auto path = shippedAutoCastM2();
    REQUIRE(std::filesystem::exists(path));
    const auto bytes = readFileBytes(path);
    REQUIRE(bytes.size() == 6176);

    const auto m2 = pipeline::M2Loader::load(bytes);
    const auto model = AutoCastShineModel::fromM2(m2);
    REQUIRE(model.isValid());

    CHECK(model.durationMs == 2000);
    CHECK(model.texturePath == "Interface\\Buttons\\GlowStar.blp");
    CHECK(model.bones.size() == 4);
    CHECK(model.emitters.size() == 4);

    for (size_t i = 0; i < model.emitters.size(); ++i) {
        CHECK(model.emitters[i].boneIndex == i);
        CHECK(model.emitters[i].blendingType == 4); // Additive
        CHECK(model.emitters[i].lifespan == Approx(1.0f).margin(1e-3f));
        CHECK(model.emitters[i].emissionRate == Approx(300.0f).margin(1e-3f));
        REQUIRE(model.emitters[i].scaleValues.size() == 3);
        CHECK(model.emitters[i].scaleValues[0] > model.emitters[i].scaleValues[1]);
        CHECK(model.emitters[i].scaleValues[1] > model.emitters[i].scaleValues[2]);
    }
}

TEST_CASE("AutoCastShineModel bones trace clockwise square perimeter with 2000ms loop", "[ui][autocast]") {
    const auto bytes = readFileBytes(shippedAutoCastM2());
    REQUIRE(!bytes.empty());
    const auto model = AutoCastShineModel::fromM2(pipeline::M2Loader::load(bytes));
    REQUIRE(model.isValid());

    // Bone 0 starts at bottom-left (0, 1) -> top-left (0, 0) -> top-right (1, 0) -> bottom-right (1, 1) -> bottom-left (0, 1)
    const glm::vec2 p0    = model.evaluateNormalizedPoint(0, 0.0f);
    const glm::vec2 p500  = model.evaluateNormalizedPoint(0, 500.0f);
    const glm::vec2 p1000 = model.evaluateNormalizedPoint(0, 1000.0f);
    const glm::vec2 p1500 = model.evaluateNormalizedPoint(0, 1500.0f);
    const glm::vec2 p2000 = model.evaluateNormalizedPoint(0, 2000.0f);

    CHECK(p0.x == Approx(0.0f).margin(0.05f));
    CHECK(p0.y == Approx(1.0f).margin(0.05f));

    CHECK(p500.x == Approx(0.0f).margin(0.05f));
    CHECK(p500.y == Approx(0.0f).margin(0.05f));

    CHECK(p1000.x == Approx(1.0f).margin(0.05f));
    CHECK(p1000.y == Approx(0.0f).margin(0.05f));

    CHECK(p1500.x == Approx(1.0f).margin(0.05f));
    CHECK(p1500.y == Approx(1.0f).margin(0.05f));

    CHECK(p2000.x == Approx(p0.x).margin(1e-4f));
    CHECK(p2000.y == Approx(p0.y).margin(1e-4f));

    // Mid-top-edge (750ms): u ~ 0.5, v ~ 0.0
    const glm::vec2 p750 = model.evaluateNormalizedPoint(0, 750.0f);
    CHECK(p750.x == Approx(0.5f).margin(0.05f));
    CHECK(p750.y == Approx(0.0f).margin(0.05f));
}

TEST_CASE("AutoCastShineModel adapts sprite bounds and scale for PetActionButton and SpellButton", "[ui][autocast]") {
    const auto bytes = readFileBytes(shippedAutoCastM2());
    REQUIRE(!bytes.empty());
    const auto model = AutoCastShineModel::fromM2(pipeline::M2Loader::load(bytes));
    REQUIRE(model.isValid());

    std::vector<AutoCastShineSprite> petSprites;
    model.evaluateSprites(1.234, 100.0f, 200.0f, 130.0f, 230.0f, 1.2f, 1.0f, petSprites);
    REQUIRE(petSprites.size() >= 128);

    float maxPetHalf = 0.0f;
    for (const auto& sp : petSprites) {
        // Every sprite center must lie on or within the button perimeter rect
        CHECK(sp.x >= 99.9f);
        CHECK(sp.x <= 130.1f);
        CHECK(sp.y >= 199.9f);
        CHECK(sp.y <= 230.1f);
        // Must lie near one of the 4 edges (square perimeter, never interior circle)
        const float distToEdge = std::min({sp.x - 100.0f, 130.0f - sp.x, sp.y - 200.0f, 230.0f - sp.y});
        CHECK(distToEdge <= 1.5f);
        maxPetHalf = std::max(maxPetHalf, sp.halfSize);
    }

    std::vector<AutoCastShineSprite> spellSprites;
    model.evaluateSprites(1.234, 10.0f, 20.0f, 46.0f, 56.0f, 1.22f, 1.0f, spellSprites);
    REQUIRE(spellSprites.size() == petSprites.size());

    float maxSpellHalf = 0.0f;
    for (const auto& sp : spellSprites) {
        maxSpellHalf = std::max(maxSpellHalf, sp.halfSize);
    }
    // 36x36 @ scale 1.22 vs 30x30 @ scale 1.2 => ratio (36/30) * (1.22/1.2) = 1.22
    CHECK(maxSpellHalf / maxPetHalf == Approx((36.0f / 30.0f) * (1.22f / 1.2f)).margin(1e-3f));
}
