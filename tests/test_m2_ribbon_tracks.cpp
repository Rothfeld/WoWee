// The animated tracks a ribbon emitter carries.
//
// A ribbon's alpha and visibility are stored the same "array of arrays" way as
// every other M2 track, but with different key types: alpha is fixed16 and
// visibility is a byte. The loader read both with its own inline copy of the
// walk rather than through parseAnimTrack, which is the shape that lets a fix
// to one reader miss the other.
//
// Neither failure is loud. Read the byte track as float and 0x01 becomes about
// 1.4e-45, which fails a `> 0.5` visibility test, so the ribbon is simply
// never drawn. Read the fixed16 track with the wrong divisor and a sword trail
// is either invisible or a solid block.
//
// This is real-asset coverage: it needs a model with ribbons extracted, and
// skips when there is none.
#include <catch_amalgamated.hpp>

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <vector>

#include "pipeline/m2_loader.hpp"

using namespace wowee::pipeline;

namespace {

std::vector<uint8_t> readFile(const std::string& relative) {
#ifdef WOWEE_SOURCE_DIR
    const std::string path = std::string(WOWEE_SOURCE_DIR) + "/" + relative;
#else
    const std::string path = relative;
#endif
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
}

/// Every value across every sequence of a track.
std::vector<float> allValues(const M2AnimationTrack& track) {
    std::vector<float> out;
    for (const auto& sequence : track.sequences) {
        out.insert(out.end(), sequence.floatValues.begin(), sequence.floatValues.end());
    }
    return out;
}

}  // namespace

TEST_CASE("a ribbon model's tracks parse into usable ranges", "[m2][ribbon]") {
    const auto data = readFile("Data/spell/lightningshield_impact_base.m2");
    if (data.empty()) {
        SUCCEED("model asset not extracted; optional real-asset coverage skipped");
        return;
    }

    const M2Model model = M2Loader::load(data);
    if (model.ribbonEmitters.empty()) {
        SUCCEED("model carries no ribbon emitters; nothing to check here");
        return;
    }

    bool sawAlpha = false;
    bool sawVisibility = false;
    for (const auto& ribbon : model.ribbonEmitters) {
        for (float value : allValues(ribbon.alphaTrack)) {
            sawAlpha = true;
            // fixed16 over 32767. A value outside 0..1 means the divisor or
            // the key width is wrong, and both read as a trail that is either
            // invisible or a solid slab.
            INFO("alpha " << value);
            CHECK(value >= -0.001f);
            CHECK(value <= 1.001f);
        }
        for (float value : allValues(ribbon.visibilityTrack)) {
            sawVisibility = true;
            // Bytes, and only ever 0 or 1. Read as float, a 1 byte becomes a
            // denormal that fails every threshold test downstream.
            INFO("visibility " << value);
            CHECK((value == 0.0f || value == 1.0f));
        }
    }

    // Not a requirement of the format, but if neither track carried anything
    // this test proved nothing and should say so rather than pass quietly.
    if (!sawAlpha && !sawVisibility) {
        WARN("ribbon emitters present but neither track carried keys");
    }
}

TEST_CASE("ribbon track sequences line up with their timestamps",
          "[m2][ribbon]") {
    const auto data = readFile("Data/spell/lightningshield_impact_base.m2");
    if (data.empty()) {
        SUCCEED("model asset not extracted; optional real-asset coverage skipped");
        return;
    }

    const M2Model model = M2Loader::load(data);
    if (model.ribbonEmitters.empty()) {
        SUCCEED("model carries no ribbon emitters; nothing to check here");
        return;
    }

    for (const auto& ribbon : model.ribbonEmitters) {
        for (const auto* track : {&ribbon.alphaTrack, &ribbon.visibilityTrack}) {
            for (const auto& sequence : track->sequences) {
                // A sequence with values but no timestamps cannot be sampled,
                // which is what reading the two sub-arrays out of step looks
                // like: the counts come from different headers.
                if (sequence.floatValues.empty()) continue;
                INFO("values " << sequence.floatValues.size()
                     << " timestamps " << sequence.timestamps.size());
                CHECK_FALSE(sequence.timestamps.empty());
            }
        }
    }
}

// Before version 264 a ribbon record is 0xDC bytes: its tracks carry a ranges
// array (28 bytes each). The loader once read only WotLK ribbons, so every TBC
// spell trail was missing; Arcane Missiles flew as a bare particle head.
TEST_CASE("a TBC missile ribbon parses from the legacy record", "[m2][ribbon][assets]") {
    const char* root = std::getenv("WOWEE_TBC_DATA_DIR");
    if (!root || !*root) SKIP("Set WOWEE_TBC_DATA_DIR to client/wowee-data/expansions/tbc");
    std::ifstream in(std::string(root) + "/spells/arcane_missile_lvl1.m2", std::ios::binary);
    const std::vector<uint8_t> data{std::istreambuf_iterator<char>(in), {}};
    REQUIRE_FALSE(data.empty());

    const M2Model model = M2Loader::load(data);
    REQUIRE(model.version == 261);
    REQUIRE(model.ribbonEmitters.size() == 1);
    const auto& ribbon = model.ribbonEmitters[0];
    CHECK(ribbon.bone == 5);
    CHECK(ribbon.edgesPerSecond == Catch::Approx(22.0f));
    CHECK(ribbon.edgeLifetime == Catch::Approx(0.5f));
    CHECK(ribbon.gravity == 0.0f);
    REQUIRE_FALSE(ribbon.heightAboveTrack.sequences.empty());
    REQUIRE_FALSE(ribbon.heightAboveTrack.sequences[0].floatValues.empty());
    CHECK(ribbon.heightAboveTrack.sequences[0].floatValues[0] == Catch::Approx(1.0f / 6.0f));
    REQUIRE_FALSE(ribbon.alphaTrack.sequences.empty());
    CHECK(ribbon.alphaTrack.sequences[0].floatValues[0] == Catch::Approx(16384.0f / 32767.0f));
    REQUIRE_FALSE(ribbon.visibilityTrack.sequences.empty());
    CHECK(ribbon.visibilityTrack.sequences[0].floatValues[0] == 1.0f);
}

// Emitters alone are a model worth placing. Blaze 181832 (display 4452) is
// StratholmeFireSmokeEmberM.m2: no vertices, three emitters (smoke, lava
// lumps, flames). Rejected as empty, Magtheridon's floor fires were absent.
TEST_CASE("a particle-only M2 has renderable content", "[m2][particle][assets]") {
    M2Model empty;
    CHECK_FALSE(empty.hasRenderableContent());
    empty.ribbonEmitters.resize(1);
    CHECK(empty.hasRenderableContent());

    const char* root = std::getenv("WOWEE_TBC_DATA_DIR");
    if (!root || !*root) SKIP("Set WOWEE_TBC_DATA_DIR to client/wowee-data/expansions/tbc");
    std::ifstream in(std::string(root) +
        "/world/lordaeron/stratholme/passivedoodads/fx/stratholmefiresmokeemberm.m2", std::ios::binary);
    const std::vector<uint8_t> data{std::istreambuf_iterator<char>(in), {}};
    REQUIRE_FALSE(data.empty());
    const M2Model model = M2Loader::load(data);
    CHECK(model.vertices.empty());
    CHECK_FALSE(model.isValid());
    CHECK(model.particleEmitters.size() == 3);
    CHECK(model.hasRenderableContent());
}

TEST_CASE("Fireball and Shadowbolt missile ribbons use bone local Y perpendicular to +X flight",
          "[m2][ribbon][assets]") {
    const char* root = std::getenv("WOWEE_TBC_DATA_DIR");
    if (!root || !*root) SKIP("Set WOWEE_TBC_DATA_DIR to client/wowee-data/expansions/tbc");

    auto boneWorldRotation = [](const M2Model& model, uint32_t boneIdx) {
        glm::quat q(1.0f, 0.0f, 0.0f, 0.0f);
        std::vector<int32_t> chain;
        int32_t cur = static_cast<int32_t>(boneIdx);
        while (cur >= 0 && static_cast<size_t>(cur) < model.bones.size() && chain.size() < 32) {
            chain.push_back(cur);
            cur = model.bones[cur].parentBone;
        }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            const auto& rot = model.bones[*it].rotation;
            if (!rot.sequences.empty() && !rot.sequences[0].quatValues.empty()) {
                q = q * rot.sequences[0].quatValues[0];
            }
        }
        return q;
    };

    SECTION("Fireball_Missile_Low.m2 emits two orthogonal ribbons along local Y (vertical + horizontal)") {
        std::ifstream in(std::string(root) + "/spells/fireball_missile_low.m2", std::ios::binary);
        const std::vector<uint8_t> data{std::istreambuf_iterator<char>(in), {}};
        REQUIRE_FALSE(data.empty());
        const M2Model model = M2Loader::load(data);
        REQUIRE(model.ribbonEmitters.size() == 2);
        CHECK(model.ribbonEmitters[0].bone == 7);
        CHECK(model.ribbonEmitters[1].bone == 8);
        CHECK(model.ribbonEmitters[0].edgesPerSecond == Catch::Approx(30.0f));
        CHECK(model.ribbonEmitters[1].edgesPerSecond == Catch::Approx(30.0f));

        const glm::vec3 y0 = boneWorldRotation(model, model.ribbonEmitters[0].bone) * glm::vec3(0.0f, 1.0f, 0.0f);
        const glm::vec3 y1 = boneWorldRotation(model, model.ribbonEmitters[1].bone) * glm::vec3(0.0f, 1.0f, 0.0f);
        // Both local Y axes are perpendicular to +X flight direction and orthogonal to each other.
        CHECK(std::abs(y0.x) < 0.05f);
        CHECK(std::abs(y1.x) < 0.05f);
        CHECK(std::abs(glm::dot(y0, y1)) < 0.05f);
    }

    SECTION("Shadowbolt_Missile.m2 ribbon local Y is perpendicular to +X flight (local Z is collinear)") {
        std::ifstream in(std::string(root) + "/spells/shadowbolt_missile.m2", std::ios::binary);
        const std::vector<uint8_t> data{std::istreambuf_iterator<char>(in), {}};
        REQUIRE_FALSE(data.empty());
        const M2Model model = M2Loader::load(data);
        REQUIRE(model.ribbonEmitters.size() == 2);
        for (const auto& rib : model.ribbonEmitters) {
            const glm::vec3 y = boneWorldRotation(model, rib.bone) * glm::vec3(0.0f, 1.0f, 0.0f);
            CHECK(std::abs(y.x) < 0.05f);
        }
    }
}
