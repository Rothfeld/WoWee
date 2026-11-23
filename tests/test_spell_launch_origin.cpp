#include <catch_amalgamated.hpp>
#include "pipeline/m2_loader.hpp"
#include "rendering/missile_homing.hpp"
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

    const auto chestBeam = resolveBeamStartOrigin(&undead, 124, 1);
    REQUIRE(chestBeam.source == SpellLaunchOrigin::Source::Attachment);
    REQUIRE(chestBeam.attachmentId == kAttachChestId);

    const auto leftHandBeam = resolveBeamStartOrigin(&undead, 124, 3);
    REQUIRE(leftHandBeam.source == SpellLaunchOrigin::Source::Attachment);
    REQUIRE(leftHandBeam.attachmentId == kAttachSpellLeftHandId);

    // When the caster model lacks a release event for the cast/channel animation
    // (e.g. TitanFemale / Maiden of Virtue lacking SpellCastDirected 53), fall back
    // to Chest (34) matching Native 2.4.3 rather than HandLeft (2).
    const auto missingAnimBeam = resolveBeamStartOrigin(&undead, 53, -1);
    REQUIRE(missingAnimBeam.source == SpellLaunchOrigin::Source::Attachment);
    REQUIRE(missingAnimBeam.attachmentId == kAttachChestId);

    const auto nullModelBeam = resolveBeamStartOrigin(nullptr, 53, -1);
    REQUIRE(nullModelBeam.source == SpellLaunchOrigin::Source::Attachment);
    REQUIRE(nullModelBeam.attachmentId == kAttachChestId);
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

    SECTION("Imp.m2 SpellCastDirected (53) uses $CST @ 766ms and never 1-frame teleports Firebolt") {
        auto [model, origin] = loadAndResolve("creature/imp/imp.m2", 53);
        REQUIRE(origin.source == SpellLaunchOrigin::Source::EventBone);
        REQUIRE(origin.bone == 57);
        REQUIRE(origin.localOffset.y < 0.0f);  // right hand (-Y)
        REQUIRE(origin.launchDelaySeconds == Catch::Approx(0.766f));
        // Imp Firebolt (3110..27267) Spell.dbc Speed = 16 yd/s: at 10 yd and 15 yd,
        // flight speed caps at 2x (32 yd/s) rather than returning 0 (1-frame teleport).
        CHECK(missileFlightSpeed(10.0f, 16.0f, origin.launchDelaySeconds) == Catch::Approx(32.0f));
        CHECK(missileFlightSpeed(15.0f, 16.0f, origin.launchDelaySeconds) == Catch::Approx(32.0f));
    }

    SECTION("TrollMale.m2 SpellCastDirected (53) uses $CSL @ 266ms and never 1-frame teleports Fireball") {
        auto [model, origin] = loadAndResolve("character/troll/male/trollmale.m2", 53);
        REQUIRE(origin.source == SpellLaunchOrigin::Source::EventBone);
        REQUIRE(origin.launchDelaySeconds == Catch::Approx(0.266f));
        // Mage Fireball (133..38692) Spell.dbc Speed = 24 yd/s: at 6 yd (nominal 0.25s < 0.266s),
        // flight speed caps at 2x (48 yd/s) instead of 0.
        CHECK(missileFlightSpeed(6.0f, 24.0f, origin.launchDelaySeconds) == Catch::Approx(48.0f));
        CHECK(missileFlightSpeed(15.0f, 24.0f, origin.launchDelaySeconds) ==
              Catch::Approx(15.0f / (15.0f / 24.0f - origin.launchDelaySeconds)));
    }

    SECTION("BloodElfMale.m2 and HumanMale.m2 bow release samples raised HandLeft (2) at AttackBow (46, t=0), never behind-back HandRight (1)") {
        const auto beBytes = readFileBytes(dataDir / "character/bloodelf/male/bloodelfmale.m2");
        REQUIRE(!beBytes.empty());
        const M2Model bloodElf = M2Loader::load(beBytes);

        // Old buggy origin: HandRight (1) during ReadyBow (29) sits 0.75m behind the character's back!
        glm::mat4 readyRight(1.0f);
        REQUIRE(evaluateModelAttachmentAtAnimation(bloodElf, 1, 29, 0.0f, 0.0f, 0.0f, readyRight));
        CHECK(readyRight[3].x < -0.5f);

        // Old buggy origin during LoadBow (105, t=300ms): HandRight (1) reaches behind the head into the quiver!
        glm::mat4 loadRight(1.0f);
        REQUIRE(evaluateModelAttachmentAtAnimation(bloodElf, 1, 105, 300.0f, 0.0f, 0.0f, loadRight));
        CHECK(loadRight[3].x < 0.1f);
        CHECK(loadRight[3].z > 1.75f);

        // Unblended AttackBow (46, t=0ms) HandLeft (2) bow grip is extended +0.93m forward at shoulder/eye level (+1.47m)!
        glm::mat4 attackBowLeft(1.0f);
        REQUIRE(evaluateModelAttachmentAtAnimation(bloodElf, 2, 46, 0.0f, 0.0f, 0.0f, attackBowLeft));
        CHECK(attackBowLeft[3].x > 0.85f);
        CHECK(attackBowLeft[3].z > 1.35f);
        CHECK(attackBowLeft[3].z < 1.65f);

        const auto hmBytes = readFileBytes(dataDir / "character/human/male/humanmale.m2");
        REQUIRE(!hmBytes.empty());
        const M2Model human = M2Loader::load(hmBytes);
        glm::mat4 humanBowLeft(1.0f);
        REQUIRE(evaluateModelAttachmentAtAnimation(human, 2, 46, 0.0f, 0.0f, 0.0f, humanBowLeft));
        CHECK(humanBowLeft[3].x > 0.85f);
        CHECK(humanBowLeft[3].z > 1.45f);
    }

    SECTION("Cast beam origin resolves $CSL/$CSR on SpellCastDirected (53) and falls back to Chest (34) on TitanFemale") {
        // TitanFemale (Maiden of Virtue, displayId 16198) lacks SpellCastDirected (53);
        // Holy Wrath (32445) / Chain Lightning (421) beams originate from Chest (34), not HandLeft (2).
        const auto tfBytes = readFileBytes(dataDir / "creature/titanfemale/titanfemale.m2");
        REQUIRE(!tfBytes.empty());
        const M2Model titanFemale = M2Loader::load(tfBytes);
        const auto tfBeam = resolveBeamStartOrigin(&titanFemale, 53, -1);
        CHECK(tfBeam.source == SpellLaunchOrigin::Source::Attachment);
        CHECK(tfBeam.attachmentId == kAttachChestId);

        // ScourgeMale authors $CSR (right hand, -Y) on SpellCastDirected (53).
        const auto smBytes = readFileBytes(dataDir / "character/scourge/male/scourgemale.m2");
        REQUIRE(!smBytes.empty());
        const M2Model scourgeMale = M2Loader::load(smBytes);
        const auto smBeam = resolveBeamStartOrigin(&scourgeMale, 53, -1);
        CHECK(smBeam.source == SpellLaunchOrigin::Source::EventBone);
        CHECK(smBeam.localOffset.y < -0.1f);

        // HumanFemale authors $CSL (left hand, +Y) on SpellCastDirected (53).
        const auto hfBytes = readFileBytes(dataDir / "character/human/female/humanfemale.m2");
        REQUIRE(!hfBytes.empty());
        const M2Model humanFemale = M2Loader::load(hfBytes);
        const auto hfBeam = resolveBeamStartOrigin(&humanFemale, 53, -1);
        CHECK(hfBeam.source == SpellLaunchOrigin::Source::EventBone);
        CHECK(hfBeam.localOffset.y > 0.1f);
    }
}
