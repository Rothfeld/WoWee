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
#include "rendering/spell_visual_kit_policy.hpp"
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
    CHECK_FALSE(SpellVisualSystem::slotFollowsBone(18)); // HeadTop / Nameplate anchor
    CHECK_FALSE(SpellVisualSystem::slotFollowsBone(19)); // Base
    CHECK_FALSE(SpellVisualSystem::slotFollowsBone(20)); // Head (eyes/head anchor)
    CHECK_FALSE(SpellVisualSystem::slotFollowsBone(34)); // Chest
}

// Illidan.m2 authors M2 attachment 20 (Head, parent = Head keyBone 6) right at
// his eyes (~z=4.99), whereas attachment 18 (Name/HeadTop, parent = -1) floats
// ~1.36 yd above his head (~z=6.35) as the nameplate anchor. Eye Blast (39908,
// SpellVisual 9051, MissileAttachment=0) must resolve to attachment 20, not 18.
TEST_CASE("Illidan M2 Head attachment 20 sits at eyes below overhead Name anchor 18",
          "[spell-visual][assets]") {
    const char* root = std::getenv("WOWEE_TBC_DATA_DIR");
    if (!root || !*root) SKIP("Set WOWEE_TBC_DATA_DIR to client/wowee-data/expansions/tbc");
    const auto bytes = readBytes(std::filesystem::path(root) / "creature/illidan/illidan.m2");
    REQUIRE_FALSE(bytes.empty());
    const auto model = wowee::pipeline::M2Loader::load(bytes);
    REQUIRE(model.attachmentLookup.size() > 20);
    const uint16_t headIdx = model.attachmentLookup[20];
    const uint16_t topIdx = model.attachmentLookup[18];
    REQUIRE(headIdx < model.attachments.size());
    REQUIRE(topIdx < model.attachments.size());
    const auto& headAtt = model.attachments[headIdx];
    const auto& topAtt = model.attachments[topIdx];
    CHECK(headAtt.id == 20);
    CHECK(topAtt.id == 18);
    CHECK(topAtt.position.z - headAtt.position.z > 1.0f);
    REQUIRE(headAtt.bone < model.bones.size());
    const int16_t headParent = model.bones[headAtt.bone].parentBone;
    REQUIRE(headParent >= 0);
    REQUIRE(static_cast<size_t>(headParent) < model.bones.size());
    CHECK(model.bones[headParent].keyBoneId == 6); // Head keyBone
    REQUIRE(topAtt.bone < model.bones.size());
    CHECK(model.bones[topAtt.bone].parentBone == -1); // Overhead nameplate anchor
}

TEST_CASE("SpellVisualSystem::precastDurationSeconds includes grace window for all positive cast times",
          "[spell-visual]") {
    using wowee::rendering::SpellVisualSystem;
    CHECK(SpellVisualSystem::precastDurationSeconds(0) < 0.0f);
    CHECK(SpellVisualSystem::precastDurationSeconds(300) ==
          Catch::Approx(0.3f + SpellVisualSystem::kPrecastGraceSec));
    CHECK(SpellVisualSystem::precastDurationSeconds(1500) ==
          Catch::Approx(1.5f + SpellVisualSystem::kPrecastGraceSec));
    CHECK(SpellVisualSystem::precastDurationSeconds(35000) ==
          Catch::Approx(35.0f + SpellVisualSystem::kPrecastGraceSec));
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

TEST_CASE("SpellVisualKit weapon slots follow hand bone orientation", "[spell-visual]") {
    using wowee::rendering::SpellVisualSystem;
    CHECK(SpellVisualSystem::slotFollowsBone(SpellVisualSystem::kAttachRightWeapon));
    CHECK(SpellVisualSystem::slotFollowsBone(SpellVisualSystem::kAttachLeftWeapon));
    CHECK(SpellVisualSystem::kAttachRightWeapon == 1u);
    CHECK(SpellVisualSystem::kAttachLeftWeapon == 2u);
}

TEST_CASE("SpellVisualKit CharProc 15 opacity fade and CharProc 17 equip override parsers",
          "[spell-visual][charproc]") {
    using namespace wowee::rendering;

    // CharProc 15: kit 6795 Teleport Test (0 -> 1 over 1000 ms),
    // kit 6978 Coalesce (1 -> 0 over 1 s), kit 3269 Jeff Dummy 1 (0.6 -> 2.0 clamped to 1.0 over 500 ms).
    const auto teleport = parseKitOpacityFadeCharProc(0.0f, 1.0f, 1000.0f);
    REQUIRE(teleport.has_value());
    CHECK(teleport->startAlpha == Catch::Approx(0.0f));
    CHECK(teleport->endAlpha == Catch::Approx(1.0f));
    CHECK(teleport->durationSec == Catch::Approx(1.0f));

    const auto coalesce = parseKitOpacityFadeCharProc(1.0f, 0.0f, 1.0f);
    REQUIRE(coalesce.has_value());
    CHECK(coalesce->startAlpha == Catch::Approx(1.0f));
    CHECK(coalesce->endAlpha == Catch::Approx(0.0f));
    CHECK(coalesce->durationSec == Catch::Approx(1.0f));

    const auto clamped = parseKitOpacityFadeCharProc(0.6f, 2.0f, 500.0f);
    REQUIRE(clamped.has_value());
    CHECK(clamped->endAlpha == Catch::Approx(1.0f));
    CHECK(clamped->durationSec == Catch::Approx(0.5f));

    CHECK_FALSE(parseKitOpacityFadeCharProc(0.0f, 1.0f, 0.0f).has_value());

    // CharProc 17: kit 8381 Jack-o'-Lanterned! (44185) & kit 8081 Conflagration (42380)
    // slot 0 (Head), Item.dbc entry 33292 (Hallowed Helm -> ItemDisplayInfo 46514).
    const auto pumpkin = parseKitEquipOverrideCharProc(0.0f, 33292.0f);
    REQUIRE(pumpkin.has_value());
    CHECK(pumpkin->slot == 0u);
    CHECK(pumpkin->itemId == 33292u);
    CHECK_FALSE(parseKitEquipOverrideCharProc(0.0f, 0.0f).has_value());
    CHECK_FALSE(parseKitEquipOverrideCharProc(-1.0f, 33292.0f).has_value());

    // Character model name -> TBC race/gender IDs for per-race helm cut resolution.
    const auto hum = raceGenderFromCharacterModelName("HumanMale");
    REQUIRE(hum.has_value());
    CHECK(hum->raceId == 1u);
    CHECK(hum->genderId == 0u);

    const auto bef = raceGenderFromCharacterModelName("BloodElfFemale");
    REQUIRE(bef.has_value());
    CHECK(bef->raceId == 10u);
    CHECK(bef->genderId == 1u);

    const auto scm = raceGenderFromCharacterModelName("ScourgeMale");
    REQUIRE(scm.has_value());
    CHECK(scm->raceId == 5u);
    CHECK(scm->genderId == 0u);

    CHECK_FALSE(raceGenderFromCharacterModelName("DragonOnyxia").has_value());
}

TEST_CASE("SpellVisualKit policies for InstantAreaKit destination, CharProc 1 effect tinting, and Drunken Master AnimID (S1/S3/S4)",
          "[spell-visual][charproc]") {
    using namespace wowee::rendering;

    SECTION("S1: Spell.dbc destination classification distinguishes 4929 Summon Burning Exile from 42052 Volcanic Geyser") {
        // 4929 Summon Burning Exile (SV 33, InstantAreaKit 9173): Targets=0, tA={32,0,0}, tB={0,0,0} -> destination!
        CHECK(spellHasInstantAreaDestination(0x0u, {32u, 0u, 0u}, {0u, 0u, 0u}));
        // 2120 Flamestrike: Targets=0x40 (TARGET_FLAG_DEST_LOCATION), tA={16,28,0} -> destination!
        CHECK(spellHasInstantAreaDestination(0x40u, {16u, 28u, 0u}, {0u, 0u, 0u}));
        // 42052 Volcanic Geyser (SV 33, InstantAreaKit 9173): Targets=0, tA={22,0,0} (SRC), tB={15,0,0} -> NOT destination!
        CHECK_FALSE(spellHasInstantAreaDestination(0x0u, {22u, 0u, 0u}, {15u, 0u, 0u}));
    }

    SECTION("S3: Non-black CharProc 1 tints attached spell M2s (35409 Brain Damage), while 0x000000 leaves Rock Shell shield untinted") {
        // 35409 Brain Damage (kit 7095): 0x800000 -> tints arcanepower_state_chest.m2 red.
        CHECK(kitCharProcTintsAttachedEffects(glm::vec3(128.0f / 255.0f, 0.0f, 0.0f)));
        // 33810 Rock Shell (kit 6926) & Vertex Color Black (kit 7641): 0x000000 -> body silhouette only.
        CHECK_FALSE(kitCharProcTintsAttachedEffects(glm::vec3(0.0f, 0.0f, 0.0f)));
    }

    SECTION("S4: 42696 Drunken Master Whirlwind (126) takes priority over Stun (14) and blocks periodic Knockdown (121)") {
        constexpr uint32_t kNoAnim = SpellVisualSystem::kNoAnimation;
        constexpr uint32_t kStand = 0u;
        constexpr uint32_t kStun = 14u;
        constexpr uint32_t kWhirlwind = 126u;

        // Even when co-applied stun aura 42695 (kit 8163, AnimID 14) sits ahead of 42696 (kit 8158, AnimID 126)
        // in newest-first order, Whirlwind (126) wins over generic Stun (14).
        const uint32_t candidates[] = {kStun, kWhirlwind};
        const uint32_t chosen = selectAuraStateAnimation(candidates, kNoAnim, kStand, kStun);
        CHECK(chosen == kWhirlwind);
        CHECK(auraStateSuppressesOneShotKitAnim(chosen, kNoAnim, kStand, kStun));

        // Pure Stun (14) alone still plays Stun over Stand (0) and does NOT suppress ImpactKits.
        const uint32_t stunOnly[] = {kStun, kStand};
        const uint32_t chosenStun = selectAuraStateAnimation(stunOnly, kNoAnim, kStand, kStun);
        CHECK(chosenStun == kStun);
        CHECK_FALSE(auraStateSuppressesOneShotKitAnim(chosenStun, kNoAnim, kStand, kStun));
    }

    SECTION("CharProc 9 persistent area policy skips duplicate center BaseEffect and scales rate/radius") {
        // Volley (kit 9356), Blizzard (kit 9350), Rain of Fire (kit 9352): BaseEffect matches CharProc 9 model.
        CHECK(persistentAreaSkipsDuplicateBaseEffect("spells\\arcaneshot_area.m2", "Spells\\ArcaneShot_Area.m2"));
        CHECK(persistentAreaSkipsDuplicateBaseEffect("Spells/Blizzard_Impact_Base.m2", "Spells\\Blizzard_Impact_Base.m2"));
        CHECK(persistentAreaSkipsDuplicateBaseEffect("Spells\\RainOfFire_Impact_Base.m2", "Spells\\RainOfFire_Impact_Base.m2"));
        // Hurricane (kit 9354) and Cave In (kit 9373): distinct center/cloud BaseEffect stays enabled.
        CHECK_FALSE(persistentAreaSkipsDuplicateBaseEffect("Spells\\LightningStorm_Cloud_State.m2", "Spells\\CallLightning_Impact.m2"));
        CHECK_FALSE(persistentAreaSkipsDuplicateBaseEffect("Spells\\Smash_Impact_Chest.m2", "Spells\\HellfireRaid_Dust_Impact_Base.m2"));

        // Effective spawn rate matches Native 2.4.3 (~2.5/s for CharParamOne = 5.0).
        CHECK(areaImpactEffectiveRate(5.0f) == Catch::Approx(2.5f));
        CHECK(areaImpactEffectiveRate(0.0f) == Catch::Approx(0.0f));

        // Uniform radial sampling concentrates impacts around the AoE center.
        CHECK(areaImpactSampleDistance(8.0f, 0.0f) == Catch::Approx(0.0f));
        CHECK(areaImpactSampleDistance(8.0f, 0.5f) == Catch::Approx(4.0f));
        CHECK(areaImpactSampleDistance(8.0f, 1.0f) == Catch::Approx(8.0f));
    }

    SECTION("Mutual channel/aura SpellVisual collision suppresses matching StateKit visualId only") {
        // Tempest-Smith 37112 Power Up (SV 8636: ChannelKit 7371 + StateKit 7373).
        CHECK(channelVisualSuppressesAuraVisual(8636u, 8636u));
        // Unrelated aura visuals (e.g. Shadowform SV 3619) stay active.
        CHECK_FALSE(channelVisualSuppressesAuraVisual(8636u, 3619u));
        // Zero visualId never suppresses.
        CHECK_FALSE(channelVisualSuppressesAuraVisual(0u, 0u));
        CHECK_FALSE(channelVisualSuppressesAuraVisual(0u, 8636u));
        CHECK_FALSE(channelVisualSuppressesAuraVisual(8636u, 0u));
    }
}
