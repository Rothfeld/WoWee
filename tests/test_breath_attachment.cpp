// A SpellVisualKit BreathEffect is placed on M2 attachment 17 (Breath) and
// aimed along it. Bug #76: Flame Spray (spell 10733, visual 2049, cast kit
// 1444) from the Inferno Elemental (display 5488) shot sideways because the
// effect was spawned unrotated at the caster's feet.
//
// Checks the authored data that rule rests on, from an extracted TBC tree:
//   WOWEE_TBC_DATA_DIR=/path/to/client/wowee-data/expansions/tbc
#include <catch_amalgamated.hpp>

#include "pipeline/m2_loader.hpp"
#include "rendering/m2_track_sampler.hpp"
#include "rendering/spell_visual_system.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>
#include <glm/gtc/quaternion.hpp>

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

TEST_CASE("SpellVisualSystem::slotFollowsBone covers Breath, SpellLeftHand/RightHand, and Special1-3",
          "[spell-visual]") {
    using wowee::rendering::SpellVisualSystem;
    // Directional / hand-mounted slots follow the attachment bone's full orientation.
    CHECK(SpellVisualSystem::slotFollowsBone(17)); // Breath
    CHECK(SpellVisualSystem::slotFollowsBone(21)); // SpellLeftHand (Cone of Cold, etc.)
    CHECK(SpellVisualSystem::slotFollowsBone(22)); // SpellRightHand
    CHECK(SpellVisualSystem::slotFollowsBone(23)); // Special1 (Dragon Flame Breath, etc.)
    CHECK(SpellVisualSystem::slotFollowsBone(24)); // Special2
    CHECK(SpellVisualSystem::slotFollowsBone(25)); // Special3

    // Upright / world-aligned slots keep unit yaw only.
    CHECK_FALSE(SpellVisualSystem::slotFollowsBone(0));  // World
    CHECK_FALSE(SpellVisualSystem::slotFollowsBone(18)); // Head (overhead anchor / Dragon's Breath)
    CHECK_FALSE(SpellVisualSystem::slotFollowsBone(19)); // Base
    CHECK_FALSE(SpellVisualSystem::slotFollowsBone(34)); // Chest
}

// Cone of Cold (spell 120, visual 1007, cast kit 1027) attaches
// Spells\ConeofCold_Hand.m2 to LeftHandEffect (attachment 21, SpellLeftHand)
// during SpellCastDirected (anim 53). All 11 emitters fire along local -Z in
// model space, which SpellLeftHand aims forward (+X) during SpellCastDirected.
TEST_CASE("Cone of Cold emits along local -Z and SpellLeftHand aims -Z forward in SpellCastDirected",
          "[spell-visual][assets]") {
    const char* root = std::getenv("WOWEE_TBC_DATA_DIR");
    if (!root || !*root) SKIP("Set WOWEE_TBC_DATA_DIR to client/wowee-data/expansions/tbc");

    // 1. Verify ConeofCold_Hand.m2 authored emission axis is local -Z.
    {
        const auto bytes = readBytes(std::filesystem::path(root) / "spells/coneofcold_hand.m2");
        REQUIRE_FALSE(bytes.empty());
        const auto model = wowee::pipeline::M2Loader::load(bytes);
        REQUIRE(model.particleEmitters.size() == 11);

        // Emitter 0 (sphere, rate=100, speed=+27.78) sits on bone 11 -> parent bone 0,
        // whose +Y axis (azimuth=0, elevation=0 emission direction) points along (0, 0, -1).
        const auto& em0 = model.particleEmitters[0];
        CHECK(em0.emitterType == 2);
        REQUIRE(em0.bone < model.bones.size());
        const int16_t parent0 = model.bones[em0.bone].parentBone;
        REQUIRE(parent0 >= 0);
        REQUIRE(static_cast<size_t>(parent0) < model.bones.size());
        REQUIRE_FALSE(model.bones[parent0].rotation.sequences.empty());
        REQUIRE_FALSE(model.bones[parent0].rotation.sequences[0].quatValues.empty());
        const glm::quat q0 = model.bones[parent0].rotation.sequences[0].quatValues[0];
        const glm::vec3 em0Axis = glm::mat3_cast(q0) * glm::vec3(0.0f, 1.0f, 0.0f);
        CHECK(em0Axis.x == Catch::Approx(0.0f).margin(0.05f));
        CHECK(em0Axis.y == Catch::Approx(0.0f).margin(0.05f));
        CHECK(em0Axis.z == Catch::Approx(-1.0f).margin(0.05f));

        // Emitters 1..10 (plane) all author negative emission speed (~-27.78) along +Z -> local -Z.
        for (size_t i = 1; i < model.particleEmitters.size(); ++i) {
            const auto& em = model.particleEmitters[i];
            CHECK(em.emitterType == 1);
            REQUIRE_FALSE(em.emissionSpeed.sequences.empty());
            REQUIRE_FALSE(em.emissionSpeed.sequences[0].floatValues.empty());
            CHECK(em.emissionSpeed.sequences[0].floatValues[0] < -20.0f);
        }
    }

    // 2. Verify TrollMale and HumanMale carry SpellLeftHand (21) on a hand bone
    // whose -Z points forward (+X) mid-way through SpellCastDirected (anim 53).
    for (const char* charFile : {"character/troll/male/trollmale.m2",
                                 "character/human/male/humanmale.m2"}) {
        INFO(charFile);
        const auto bytes = readBytes(std::filesystem::path(root) / charFile);
        REQUIRE_FALSE(bytes.empty());
        const auto model = wowee::pipeline::M2Loader::load(bytes);
        REQUIRE(model.attachmentLookup.size() > 21);
        const uint16_t attIdx = model.attachmentLookup[21];
        REQUIRE(attIdx < model.attachments.size());
        const uint32_t handBone = model.attachments[attIdx].bone;
        REQUIRE(handBone < model.bones.size());
        CHECK(model.bones[handBone].parentBone >= 0);

        // Locate SpellCastDirected (anim 53, sub 0).
        int seqIdx = -1;
        for (size_t s = 0; s < model.sequences.size(); ++s) {
            if (model.sequences[s].id == 53 && model.sequences[s].variationIndex == 0) {
                seqIdx = static_cast<int>(s);
                break;
            }
        }
        REQUIRE(seqIdx >= 0);
        const float halfDur = static_cast<float>(model.sequences[seqIdx].duration) * 0.5f;

        // Compose bone rotation chain from root down to SpellLeftHand at mid-cast.
        std::vector<uint32_t> chain;
        for (int32_t b = static_cast<int32_t>(handBone); b >= 0; b = model.bones[b].parentBone) {
            chain.push_back(static_cast<uint32_t>(b));
        }
        glm::mat3 rot(1.0f);
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            const glm::quat q = wowee::rendering::m2_track::sampleQuat(
                model.bones[*it].rotation, seqIdx, halfDur, 0.0f, model.globalSequenceDurations);
            rot = rot * glm::mat3_cast(q);
        }
        const glm::vec3 negZ = rot * glm::vec3(0.0f, 0.0f, -1.0f);
        CHECK(negZ.x > 0.85f);
        CHECK(std::abs(negZ.z) < 0.35f);
    }
}
