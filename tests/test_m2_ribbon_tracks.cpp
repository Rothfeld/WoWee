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
#include "rendering/particle_emission.hpp"

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

TEST_CASE("ribbonGravityStepZ integrates z(age) = gravity * age^2 frame-rate independently",
          "[m2][ribbon]") {
    using wowee::rendering::ribbonGravityStepZ;
    // WaterElementalTotem: gravity=+5, edgeLifetime=0.51s -> rises +1.3005 yd along +Z.
    const float gravity = 5.0f;
    const float lifetime = 0.51f;

    float z60 = 0.0f;
    for (int i = 0; i < 30; ++i) {
        const float t0 = lifetime * static_cast<float>(i) / 30.0f;
        const float t1 = lifetime * static_cast<float>(i + 1) / 30.0f;
        z60 += ribbonGravityStepZ(gravity, t0, t1);
    }
    float z10 = 0.0f;
    for (int i = 0; i < 5; ++i) {
        const float t0 = lifetime * static_cast<float>(i) / 5.0f;
        const float t1 = lifetime * static_cast<float>(i + 1) / 5.0f;
        z10 += ribbonGravityStepZ(gravity, t0, t1);
    }
    const float expected = gravity * lifetime * lifetime;
    CHECK(z60 == Catch::Approx(expected).margin(1e-5f));
    CHECK(z10 == Catch::Approx(expected).margin(1e-5f));
    CHECK(z60 > 1.25f);
    // Negative gravity falls along -Z; scaleZ scales displacement proportionally.
    CHECK(ribbonGravityStepZ(-1.0f, 0.0f, 0.5f, 2.0f) == Catch::Approx(-0.5f));
}

TEST_CASE("TBC Shaman elemental totems parse rising ribbons and projected base glow",
          "[m2][ribbon][assets]") {
    const char* root = std::getenv("WOWEE_TBC_DATA_DIR");
    if (!root || !*root) SKIP("Set WOWEE_TBC_DATA_DIR to client/wowee-data/expansions/tbc");

    for (const char* rel : {
             "/creature/spells/waterelementaltotem.m2",
             "/creature/spells/airelementaltotem.m2",
             "/creature/spells/earthelementaltotem.m2",
             "/creature/spells/fireelementaltotem.m2",
         }) {
        INFO("totem=" << rel);
        std::ifstream in(std::string(root) + rel, std::ios::binary);
        const std::vector<uint8_t> data{std::istreambuf_iterator<char>(in), {}};
        REQUIRE_FALSE(data.empty());

        const M2Model model = M2Loader::load(data);
        REQUIRE(model.ribbonEmitters.size() >= 2);
        for (const auto& rib : model.ribbonEmitters) {
            CHECK(rib.gravity == Catch::Approx(5.0f).margin(0.01f));
            CHECK(rib.edgesPerSecond >= 20.0f);
            CHECK(rib.edgeLifetime >= 0.5f);
            // Stand sequence (index 1) has visibility=1; Birth (index 0) has visibility=0.
            REQUIRE(rib.visibilityTrack.sequences.size() >= 2);
            REQUIRE_FALSE(rib.visibilityTrack.sequences[0].floatValues.empty());
            REQUIRE_FALSE(rib.visibilityTrack.sequences[1].floatValues.empty());
            CHECK(rib.visibilityTrack.sequences[0].floatValues[0] == 0.0f);
            CHECK(rib.visibilityTrack.sequences[1].floatValues[0] == 1.0f);
        }
    }
}

TEST_CASE("TBC ChargeTrail and InterveneTrail ribbons orient vertically along bone local Y",
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

    SECTION("Spells\\ChargeTrail.m2 (SpellVisualKit 44 for Charge and Intercept)") {
        std::ifstream in(std::string(root) + "/spells/chargetrail.m2", std::ios::binary);
        const std::vector<uint8_t> data{std::istreambuf_iterator<char>(in), {}};
        REQUIRE_FALSE(data.empty());

        const M2Model model = M2Loader::load(data);
        REQUIRE(model.version == 260);
        REQUIRE(model.ribbonEmitters.size() == 1);
        REQUIRE(model.particleEmitters.size() == 1);
        REQUIRE(model.materials.size() == 1);
        CHECK(model.materials[0].blendMode == 4);
        CHECK((model.materials[0].flags & 0x01u) == 0); // lit by scene lighting (flags=0x16)

        const auto& rib = model.ribbonEmitters[0];
        CHECK(rib.bone == 1);
        CHECK(rib.edgesPerSecond == Catch::Approx(50.0f));
        CHECK(rib.edgeLifetime == Catch::Approx(1.0f));
        CHECK(rib.gravity == 0.0f);

        // Bone 1 rotates local +Y ~10 deg off -Z (downward), so the vertical
        // ribbon spans ~0.94 yd tall with -localY at the top (+Z, v=0 bright
        // edge of GRAD3A.BLP) and +localY at the bottom (-Z, v=1 faded edge).
        const glm::vec3 widthAxis = boneWorldRotation(model, rib.bone) * glm::vec3(0.0f, 1.0f, 0.0f);
        CHECK(widthAxis.x == Catch::Approx(-0.1736f).margin(0.01f));
        CHECK(std::abs(widthAxis.y) < 0.01f);
        CHECK(widthAxis.z == Catch::Approx(-0.9848f).margin(0.01f));

        REQUIRE_FALSE(rib.heightAboveTrack.sequences.empty());
        REQUIRE_FALSE(rib.heightBelowTrack.sequences.empty());
        CHECK(rib.heightAboveTrack.sequences[0].floatValues[0] == Catch::Approx(0.472222f).margin(1e-4f));
        CHECK(rib.heightBelowTrack.sequences[0].floatValues[0] == Catch::Approx(0.472222f).margin(1e-4f));

        REQUIRE_FALSE(rib.colorTrack.sequences.empty());
        REQUIRE_FALSE(rib.colorTrack.sequences[0].vec3Values.empty());
        const glm::vec3 col = rib.colorTrack.sequences[0].vec3Values[0];
        CHECK(col.r == Catch::Approx(0.811765f).margin(1e-3f));
        CHECK(col.g == Catch::Approx(0.0f).margin(1e-3f));
        CHECK(col.b == Catch::Approx(0.0f).margin(1e-3f));

        REQUIRE_FALSE(rib.alphaTrack.sequences.empty());
        CHECK(rib.alphaTrack.sequences[0].floatValues[0] == Catch::Approx(26214.0f / 32767.0f).margin(1e-3f));
        REQUIRE_FALSE(rib.visibilityTrack.sequences.empty());
        CHECK(rib.visibilityTrack.sequences[0].floatValues[0] == 1.0f);
    }

    SECTION("Spells\\InterveneTrail.m2 (SpellVisualKit 7726 for Intervene)") {
        std::ifstream in(std::string(root) + "/spells/intervenetrail.m2", std::ios::binary);
        const std::vector<uint8_t> data{std::istreambuf_iterator<char>(in), {}};
        REQUIRE_FALSE(data.empty());

        const M2Model model = M2Loader::load(data);
        REQUIRE(model.ribbonEmitters.size() == 1);
        const auto& rib = model.ribbonEmitters[0];
        CHECK(rib.bone == 1);
        CHECK(rib.edgesPerSecond == Catch::Approx(50.0f));
        CHECK(rib.edgeLifetime == Catch::Approx(0.5f));
        REQUIRE_FALSE(rib.heightAboveTrack.sequences.empty());
        REQUIRE_FALSE(rib.heightBelowTrack.sequences.empty());
        CHECK(rib.heightAboveTrack.sequences[0].floatValues[0] == Catch::Approx(0.0f).margin(1e-4f));
        CHECK(rib.heightBelowTrack.sequences[0].floatValues[0] == Catch::Approx(0.611111f).margin(1e-4f));
        const glm::vec3 widthAxis = boneWorldRotation(model, rib.bone) * glm::vec3(0.0f, 1.0f, 0.0f);
        CHECK(glm::length(widthAxis) == Catch::Approx(1.0f).margin(1e-3f));
    }
}

TEST_CASE("Short-pulse M2 emitters arm accumulator at 1.0 across idle phases (S2: ArcaneForceShield_Dark.m2)",
          "[m2][particle][assets]") {
    using wowee::rendering::emitterHasIdlePhase;
    using wowee::rendering::initialContinuousEmitterAccumulator;
    using wowee::rendering::particleContinuousEmissionIncrement;

    const char* root = std::getenv("WOWEE_TBC_DATA_DIR");
    if (!root || !*root) SKIP("Set WOWEE_TBC_DATA_DIR to client/wowee-data/expansions/tbc");

    // 33810 Rock Shell (BaseEffect 3622, Spells\ArcaneForceShield_Dark.m2):
    // emitters 0..4 pulse 0 -> 10.02 -> 0 over 133-167 ms (integral 0.67-0.84),
    // including emitter 2 (T_VFX_FLARE06.BLP iridescent rainbow shield).
    std::ifstream in(std::string(root) + "/spells/arcaneforceshield_dark.m2", std::ios::binary);
    const std::vector<uint8_t> data{std::istreambuf_iterator<char>(in), {}};
    REQUIRE_FALSE(data.empty());
    const M2Model model = M2Loader::load(data);
    REQUIRE(model.particleEmitters.size() == 6);

    const std::vector<uint32_t>& gsd = model.globalSequenceDurations;
    for (size_t ei = 0; ei < 5; ++ei) {
        INFO("emitter=" << ei);
        const auto& em = model.particleEmitters[ei];
        CHECK(emitterHasIdlePhase(em.emissionRate, em.enabledIn, 0));

        float accum = initialContinuousEmitterAccumulator(true, 0.25f);
        CHECK(accum == Catch::Approx(1.0f));
        uint32_t spawned = 0;
        // Simulate 1.0 s of Stand (seq 0) at 60 Hz with median random01 = 0.5.
        for (int step = 0; step < 60; ++step) {
            const float animMs = static_cast<float>(step) * (1000.0f / 60.0f);
            const float rate = wowee::rendering::m2_track::sampleFloat(
                em.emissionRate, 0, animMs, animMs, gsd, 0.0f);
            if (rate <= 0.0f) {
                accum = 1.0f;
                continue;
            }
            accum += particleContinuousEmissionIncrement(rate, 1.0f / 60.0f, 0.5f);
            while (accum >= 1.0f) {
                accum -= 1.0f;
                ++spawned;
            }
        }
        // Every pulse layer (including #2 T_VFX_FLARE06.BLP) spawns its 1 leading particle.
        CHECK(spawned == 1u);
    }

    // Continuous steady-state smoke (Blacksmith_smoke.m2) has no <= 0 rate key and
    // initializes in [0, 1) without arming to 1.0.
    std::ifstream smokeIn(std::string(root) +
        "/world/generic/human/passive doodads/chimneysmoke/blacksmith_smoke.m2", std::ios::binary);
    const std::vector<uint8_t> smokeData{std::istreambuf_iterator<char>(smokeIn), {}};
    if (!smokeData.empty()) {
        const M2Model smoke = M2Loader::load(smokeData);
        REQUIRE_FALSE(smoke.particleEmitters.empty());
        const auto& sem = smoke.particleEmitters[0];
        CHECK_FALSE(emitterHasIdlePhase(sem.emissionRate, sem.enabledIn, 0));
        CHECK(initialContinuousEmitterAccumulator(false, 0.37f) == Catch::Approx(0.37f));
    }
}

TEST_CASE("Outdoor entity particle light applies AreaTable AmbientMultiplier gain at night (S5)",
          "[m2][particle]") {
    using wowee::rendering::outdoorEntityParticleLight;
    using wowee::rendering::outdoorParticleLight;

    // Mirage Raceway at 21:58 night (Light 230 / param 239):
    // Ambient = (0.000, 0.118, 0.235), Direct = (0.279, 0.421, 0.709), -lightDir.z = 0.518,
    // Thousand Needles AmbientMultiplier = 0.5 -> unitAmbGain = 2.0.
    const glm::vec3 amb(0.0f, 0.118f, 0.235f);
    const glm::vec3 dir(0.279f, 0.421f, 0.709f);
    const glm::vec3 ldir(0.0f, 0.855f, -0.518f);

    const glm::vec3 doodadLight = outdoorParticleLight(amb, dir, ldir);
    const glm::vec3 entityLight = outdoorEntityParticleLight(amb, dir, ldir, 2.0f);

    // Static map doodads keep the unboosted isotropic spherical-average formula.
    CHECK(doodadLight.r == Catch::Approx(0.279f * 0.35f * 0.518f).margin(1e-3f));
    CHECK(doodadLight.b < 0.50f);

    // Dynamic GameObject / Creature smoke (Blacksmith_smoke GO 1604, SandVortex NPC 19922)
    // matches Native 2.4.3 cool blue-grey night luminance (~0.29, 0.67, 1.00).
    CHECK(entityLight.r == Catch::Approx(0.289f).margin(0.01f));
    CHECK(entityLight.g == Catch::Approx(0.672f).margin(0.01f));
    CHECK(entityLight.b == Catch::Approx(1.0f).margin(0.01f));
}

TEST_CASE("WarpStorm.m2 sub-stepped ribbon bone evaluation and age-based U (S6)",
          "[m2][ribbon][assets]") {
    using wowee::rendering::evaluateRibbonBoneTransform;
    using wowee::rendering::interpolateSequenceTimeMs;
    using wowee::rendering::ribbonEdgeU;

    CHECK(ribbonEdgeU(0.0f, 0.5f) == Catch::Approx(0.0f));
    CHECK(ribbonEdgeU(0.25f, 0.5f) == Catch::Approx(0.5f));
    CHECK(ribbonEdgeU(0.5f, 0.5f) == Catch::Approx(1.0f));
    CHECK(ribbonEdgeU(0.75f, 0.5f) == Catch::Approx(1.0f));
    CHECK(ribbonEdgeU(0.05f, 0.2f) == Catch::Approx(0.25f));
    CHECK(ribbonEdgeU(0.1f, 0.0f) == Catch::Approx(0.0f));

    // Loop wraparound across 3333 ms Stand sequence advances forward across 0, not backward.
    CHECK(interpolateSequenceTimeMs(3300.0f, 33.0f, 3333.0f, 0.5f) == Catch::Approx(0.0f).margin(0.6f));

    const char* root = std::getenv("WOWEE_TBC_DATA_DIR");
    if (!root || !*root) SKIP("Set WOWEE_TBC_DATA_DIR to client/wowee-data/expansions/tbc");

    std::ifstream in(std::string(root) + "/creature/warpstorm/warpstorm.m2", std::ios::binary);
    const std::vector<uint8_t> data{std::istreambuf_iterator<char>(in), {}};
    REQUIRE_FALSE(data.empty());
    const M2Model model = M2Loader::load(data);
    REQUIRE(model.ribbonEmitters.size() == 6);
    REQUIRE_FALSE(model.sequences.empty());

    const auto& rib0 = model.ribbonEmitters[0];
    CHECK(rib0.edgesPerSecond >= 45.0f);
    CHECK(rib0.edgeLifetime == Catch::Approx(0.4f));

    // Over one 50 ms frame (20 fps), rib0 emits ~2.75 edges while orbiting at ~635 deg/s.
    // Sub-stepping across [100ms, 150ms] yields distinct intermediate arc positions
    // rather than duplicate copies of the end-of-frame pose.
    const glm::vec3 camWorld(10.0f, 0.0f, 1.5f);
    const glm::mat4 invModel(1.0f);
    const float dur = static_cast<float>(model.sequences[0].duration);
    const glm::vec3 p0 = glm::vec3(evaluateRibbonBoneTransform(
        model.bones, model.globalSequenceDurations, rib0.bone, 0,
        interpolateSequenceTimeMs(100.0f, 150.0f, dur, 0.0f), 100.0f, invModel, &camWorld) *
        glm::vec4(rib0.position, 1.0f));
    const glm::vec3 pMid = glm::vec3(evaluateRibbonBoneTransform(
        model.bones, model.globalSequenceDurations, rib0.bone, 0,
        interpolateSequenceTimeMs(100.0f, 150.0f, dur, 0.5f), 125.0f, invModel, &camWorld) *
        glm::vec4(rib0.position, 1.0f));
    const glm::vec3 p1 = glm::vec3(evaluateRibbonBoneTransform(
        model.bones, model.globalSequenceDurations, rib0.bone, 0,
        interpolateSequenceTimeMs(100.0f, 150.0f, dur, 1.0f), 150.0f, invModel, &camWorld) *
        glm::vec4(rib0.position, 1.0f));

    CHECK(glm::distance(p0, pMid) > 0.05f);
    CHECK(glm::distance(pMid, p1) > 0.05f);
    // Midpoint deviates outward along the circular perimeter from the straight chord (p0 + p1) * 0.5.
    const glm::vec3 chordMid = 0.5f * (p0 + p1);
    CHECK(glm::distance(pMid, chordMid) > 0.01f);
}

TEST_CASE("ArrowFlight_01.m2 ribbon tracks and age-based U mapping", "[m2][ribbon][assets]") {
    using wowee::rendering::ribbonEdgeU;
    const char* root = std::getenv("WOWEE_TBC_DATA_DIR");
    if (!root || !*root) SKIP("Set WOWEE_TBC_DATA_DIR to client/wowee-data/expansions/tbc");

    std::ifstream in(std::string(root) + "/item/objectcomponents/ammo/arrowflight_01.m2", std::ios::binary);
    const std::vector<uint8_t> data{std::istreambuf_iterator<char>(in), {}};
    REQUIRE_FALSE(data.empty());
    const M2Model model = M2Loader::load(data);
    REQUIRE(model.ribbonEmitters.size() == 1);
    const auto& rib = model.ribbonEmitters[0];
    CHECK(rib.bone == 2);
    CHECK(rib.edgesPerSecond == Catch::Approx(50.0f));
    CHECK(rib.edgeLifetime == Catch::Approx(0.2f));
    REQUIRE(rib.materialIndex < model.materials.size());
    CHECK(model.materials[rib.materialIndex].blendMode == 4);
    CHECK(ribbonEdgeU(0.0f, rib.edgeLifetime) == Catch::Approx(0.0f));
    CHECK(ribbonEdgeU(0.05f, rib.edgeLifetime) == Catch::Approx(0.25f));
    CHECK(ribbonEdgeU(0.10f, rib.edgeLifetime) == Catch::Approx(0.50f));
    CHECK(ribbonEdgeU(0.20f, rib.edgeLifetime) == Catch::Approx(1.0f));
}
