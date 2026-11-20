#include <catch_amalgamated.hpp>
#include "pipeline/m2_loader.hpp"
#include "rendering/spell_launch_origin.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <glm/gtc/matrix_transform.hpp>

using wowee::pipeline::M2Bone;
using wowee::pipeline::M2Loader;
using wowee::pipeline::M2Model;
using wowee::pipeline::M2Sequence;
using namespace wowee::rendering;

namespace {

M2Model makeModelWithReleaseEvent(uint32_t animId, uint32_t timeMs, uint16_t bone,
                                  const glm::vec3& pos, char side, size_t boneCount = 140) {
    M2Model model;
    model.bones.resize(boneCount);
    M2Sequence seq;
    seq.id = animId;
    seq.variationIndex = 0;
    seq.duration = 1000;
    model.sequences.push_back(seq);
    model.spellReleaseEvents.resize(1);
    model.spellReleaseEvents[0].push_back({timeMs, bone, pos, side});
    return model;
}

std::vector<uint8_t> readFileBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return {};
    const auto sz = in.tellg();
    if (sz <= 0) return {};
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(buf.data()), sz);
    return buf;
}

}  // namespace

TEST_CASE("resolveSpellLaunchOrigin picks $CSR/$CSL/$CST event bone and release delay", "[spell_origin]") {
    SECTION("GnomeMale SpellCastDirected uses $CSR right hand bone at 200ms") {
        const M2Model gnome = makeModelWithReleaseEvent(53, 200, 115, {-0.042f, -0.319f, 0.347f}, 'R');
        const auto origin = resolveSpellLaunchOrigin(&gnome, 53, -1, false);
        REQUIRE(origin.source == SpellLaunchOrigin::Source::EventBone);
        REQUIRE(origin.bone == 115);
        REQUIRE(origin.localOffset.y == Catch::Approx(-0.319f));
        REQUIRE(origin.launchDelaySeconds == Catch::Approx(0.200f));
    }

    SECTION("HumanMale SpellCastDirected uses $CSL left hand bone at 200ms") {
        const M2Model human = makeModelWithReleaseEvent(53, 200, 117, {-0.059f, 0.471f, 0.904f}, 'L');
        const auto origin = resolveSpellLaunchOrigin(&human, 53, -1, false);
        REQUIRE(origin.source == SpellLaunchOrigin::Source::EventBone);
        REQUIRE(origin.bone == 117);
        REQUIRE(origin.localOffset.y == Catch::Approx(0.471f));
        REQUIRE(origin.launchDelaySeconds == Catch::Approx(0.200f));
    }

    SECTION("Imp SpellCastDirected uses $CST bone at 766ms") {
        const M2Model imp = makeModelWithReleaseEvent(53, 766, 57, {-0.132f, -0.459f, 0.319f}, 'T', 64);
        const auto origin = resolveSpellLaunchOrigin(&imp, 53, -1, false);
        REQUIRE(origin.source == SpellLaunchOrigin::Source::EventBone);
        REQUIRE(origin.bone == 57);
        REQUIRE(origin.launchDelaySeconds == Catch::Approx(0.766f));
    }

    SECTION("Reflected missile skips release delay and uses ChestBloodFront (15)") {
        const M2Model gnome = makeModelWithReleaseEvent(53, 200, 115, {-0.042f, -0.319f, 0.347f}, 'R');
        const auto origin = resolveSpellLaunchOrigin(&gnome, 53, -1, /*skipReleaseDelay=*/true);
        REQUIRE(origin.source == SpellLaunchOrigin::Source::Attachment);
        REQUIRE(origin.attachmentId == kAttachChestBloodFrontId);
        REQUIRE(origin.launchDelaySeconds == Catch::Approx(0.0f));
    }

    SECTION("Missing model or animation falls back to hand attachment 2 with zero delay") {
        const auto noModel = resolveSpellLaunchOrigin(nullptr, 53, -1, false);
        REQUIRE(noModel.source == SpellLaunchOrigin::Source::Attachment);
        REQUIRE(noModel.attachmentId == kAttachLaunchHandFallback);
        REQUIRE(noModel.launchDelaySeconds == Catch::Approx(0.0f));

        const M2Model gnome = makeModelWithReleaseEvent(53, 200, 115, {-0.042f, -0.319f, 0.347f}, 'R');
        const auto wrongAnim = resolveSpellLaunchOrigin(&gnome, 999, -1, false);
        REQUIRE(wrongAnim.source == SpellLaunchOrigin::Source::Attachment);
        REQUIRE(wrongAnim.attachmentId == kAttachLaunchHandFallback);
        REQUIRE(wrongAnim.launchDelaySeconds == Catch::Approx(0.0f));
    }

    SECTION("Out-of-bounds event bone falls back to side-appropriate spell hand attachment") {
        const M2Model badBone = makeModelWithReleaseEvent(53, 200, 200, {0.0f, -0.3f, 0.3f}, 'R', 100);
        const auto origin = resolveSpellLaunchOrigin(&badBone, 53, -1, false);
        REQUIRE(origin.source == SpellLaunchOrigin::Source::Attachment);
        REQUIRE(origin.attachmentId == kAttachSpellRightHandId);
        REQUIRE(origin.launchDelaySeconds == Catch::Approx(0.200f));
    }
}

TEST_CASE("resolveBeamStartOrigin respects MissileAttachment and race $CSL/$CSR", "[spell_origin]") {
    const M2Model undead = makeModelWithReleaseEvent(124, 0, 134, {0.021f, -0.268f, 0.900f}, 'R', 150);
    const auto handBeam = resolveBeamStartOrigin(&undead, 124, -1);
    REQUIRE(handBeam.source == SpellLaunchOrigin::Source::EventBone);
    REQUIRE(handBeam.bone == 134);

    const auto chestBeam = resolveBeamStartOrigin(&undead, 124, 3);
    REQUIRE(chestBeam.source == SpellLaunchOrigin::Source::Attachment);
    REQUIRE(chestBeam.attachmentId == kAttachChestId);
}

TEST_CASE("computeBonePointWorldTransform combines position, yaw, scale, bone matrix, and offset", "[spell_origin]") {
    const glm::vec3 worldPos(100.0f, -50.0f, 12.0f);
    const float yaw = glm::radians(90.0f);
    const float scale = 0.5f;
    glm::mat4 modelMat = glm::translate(glm::mat4(1.0f), worldPos);
    modelMat = glm::rotate(modelMat, yaw, glm::vec3(0.0f, 0.0f, 1.0f));
    modelMat = glm::scale(modelMat, glm::vec3(scale));

    // Animated bone extends hand +1.0 along model +X (forward)
    const glm::mat4 boneMat = glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 0.0f, 0.2f));
    const glm::vec3 offset(0.0f, -0.4f, 0.4f);

    const glm::mat4 world = computeBonePointWorldTransform(modelMat, boneMat, offset);
    const glm::vec3 p(world[3]);
    // Local point = (1.0, -0.4, 0.6), scaled 0.5 -> (0.5, -0.2, 0.3), rotated +90 deg Z -> (+0.2, +0.5, +0.3)
    REQUIRE(p.x == Catch::Approx(100.2f).margin(1e-4f));
    REQUIRE(p.y == Catch::Approx(-49.5f).margin(1e-4f));
    REQUIRE(p.z == Catch::Approx(12.3f).margin(1e-4f));
}

TEST_CASE("M2Loader parses TBC 2.4.3 $CSL/$CSR/$CST release events across races and creatures", "[spell_origin]") {
    const char* dataEnv = std::getenv("WOWEE_TBC_DATA_DIR");
    if (!dataEnv || !*dataEnv) {
        SUCCEED("WOWEE_TBC_DATA_DIR not set; skipping real TBC M2 data assertions");
        return;
    }
    const std::filesystem::path dataDir(dataEnv);

    auto loadAndResolve = [&](const char* relPath, uint32_t animId) -> std::pair<M2Model, SpellLaunchOrigin> {
        const auto bytes = readFileBytes(dataDir / relPath);
        REQUIRE(!bytes.empty());
        M2Model model = M2Loader::load(bytes);
        auto origin = resolveSpellLaunchOrigin(&model, animId, -1, false);
        return {std::move(model), origin};
    };

    SECTION("GnomeMale.m2 SpellCastDirected (53) uses right-hand $CSR @ 200ms") {
        auto [model, origin] = loadAndResolve("character/gnome/male/gnomemale.m2", 53);
        REQUIRE(origin.source == SpellLaunchOrigin::Source::EventBone);
        REQUIRE(origin.bone == 115);
        REQUIRE(origin.localOffset.y < 0.0f);  // right hand (-Y)
        REQUIRE(origin.launchDelaySeconds == Catch::Approx(0.200f));
    }

    SECTION("HumanMale.m2 SpellCastDirected (53) uses left-hand $CSL @ 200ms") {
        auto [model, origin] = loadAndResolve("character/human/male/humanmale.m2", 53);
        REQUIRE(origin.source == SpellLaunchOrigin::Source::EventBone);
        REQUIRE(origin.bone == 117);
        REQUIRE(origin.localOffset.y > 0.0f);  // left hand (+Y)
        REQUIRE(origin.launchDelaySeconds == Catch::Approx(0.200f));
    }

    SECTION("ScourgeMale.m2 SpellCastDirected (53) uses right-hand $CSR @ 200ms") {
        auto [model, origin] = loadAndResolve("character/scourge/male/scourgemale.m2", 53);
        REQUIRE(origin.source == SpellLaunchOrigin::Source::EventBone);
        REQUIRE(origin.bone == 134);
        REQUIRE(origin.localOffset.y < 0.0f);  // right hand (-Y)
        REQUIRE(origin.launchDelaySeconds == Catch::Approx(0.200f));
    }

    SECTION("Imp.m2 SpellCastDirected (53) uses $CST @ 766ms") {
        auto [model, origin] = loadAndResolve("creature/imp/imp.m2", 53);
        REQUIRE(origin.source == SpellLaunchOrigin::Source::EventBone);
        REQUIRE(origin.bone == 57);
        REQUIRE(origin.localOffset.y < 0.0f);  // right hand (-Y)
        REQUIRE(origin.launchDelaySeconds == Catch::Approx(0.766f));
    }
}
