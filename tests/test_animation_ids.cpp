// Animation ID validation tests - covers nameFromId() and validateAgainstDBC()
#include <catch_amalgamated.hpp>
#include "rendering/animation/animation_ids.hpp"
#include "rendering/animation/creature_locomotion.hpp"
#include "core/game_object_animation_policy.hpp"
#include "rendering/aura_marker_visibility.hpp"
#include "pipeline/dbc_loader.hpp"
#include "rendering/animation/stand_pose.hpp"
#include "rendering/animation/gesture_weapons.hpp"
#include "core/weapon_attachment.hpp"
#include "game/equipment_hash.hpp"
#include "rendering/animation/hit_reaction.hpp"
#include "rendering/animation/freeze_anim_policy.hpp"
#include <cstring>
#include <vector>

using wowee::pipeline::DBCFile;
namespace anim = wowee::rendering::anim;

TEST_CASE("Gesture weapon policy respects NOSHEATHE and both animation layers", "[gesture-weapons]") {
    using namespace wowee::rendering;
    CHECK(gestureSheathesWeapons(0x800, 0x10)); // TBC Wave emote 3 / animation 67.
    CHECK_FALSE(gestureSheathesWeapons(4, 0x10)); // Wave NOSHEATHE 70, same animation.
    CHECK(gestureSheathesWeapons(0x008c, 0x10)); // TBC ONESHOT_TALK emote 1 / animation 60.
    CHECK(gestureSheathesWeapons(0x00a4, 0x10)); // TBC ONESHOT_EXCLAMATION emote 5 / animation 64 (Marshal Dughan).
    CHECK(gestureSheathesWeapons(0x0094, 0x10)); // TBC ONESHOT_QUESTION emote 6 / animation 65.
    CHECK(gestureSheathesWeapons(0x4404, 0x04)); // TBC ONESHOT_LOOT emote 381 / animation 50.
    CHECK_FALSE(gestureSheathesWeapons(0x0088, 0x0)); // TBC ONESHOT_TALK_NOSHEATHE emote 396 / animation 208.
    CHECK(gestureSheathesWeapons(0, 4));
    CHECK_FALSE(gestureSheathesWeapons(0x800, 0));
    CHECK_FALSE(gestureSheathesWeapons(0, 0x20));
    CHECK(gestureWeaponPoseActive(67, 67, 0));
    CHECK(gestureWeaponPoseActive(67, anim::SITTING, 67));
    CHECK_FALSE(gestureWeaponPoseActive(67, anim::SITTING, 0));
    CHECK_FALSE(gestureWeaponPoseActive(67, anim::RUN, 0));
    CHECK_FALSE(gestureWeaponPoseActive(0, anim::STAND, 0));
}

TEST_CASE("Weapon sheath attachments and EquipmentItem sheath hashing", "[gesture-weapons][sheath]") {
    using namespace wowee::core;
    using wowee::game::EquipSlot;
    // Warglaive of Azzinoth (32837/32838) and 2H swords use SheatheType 1 -> crossed back 26/27.
    CHECK(sheathAttachment(1, EquipSlot::MAIN_HAND) == kAttachSheathMainHand);
    CHECK(sheathAttachment(1, EquipSlot::OFF_HAND) == kAttachSheathOffHand);
    // Staves / polearms use SheatheType 2 -> large weapon 30/31.
    CHECK(sheathAttachment(2, EquipSlot::MAIN_HAND) == kAttachLargeWeaponLeft);
    CHECK(sheathAttachment(2, EquipSlot::OFF_HAND) == kAttachLargeWeaponRight);
    // 1H swords / maces (Marshal Dughan 3361) use SheatheType 3 -> hip 32/33.
    CHECK(sheathAttachment(3, EquipSlot::MAIN_HAND) == kAttachHipWeaponLeft);
    CHECK(sheathAttachment(3, EquipSlot::OFF_HAND) == kAttachHipWeaponRight);
    // Shields use SheatheType 4 -> back shield 28.
    CHECK(sheathAttachment(4, EquipSlot::OFF_HAND) == kAttachSheathShield);
    // SheatheType 0 hides weapon when sheathed.
    CHECK(sheathAttachment(0, EquipSlot::MAIN_HAND) == UINT32_MAX);

    // PaperDoll cache hash changes when weapon SheatheType arrives after item query.
    std::vector<wowee::game::EquipmentItem> a = {{.displayModel = 45479, .inventoryType = 21, .enchantment = 0, .sheath = 0}};
    std::vector<wowee::game::EquipmentItem> b = {{.displayModel = 45479, .inventoryType = 21, .enchantment = 0, .sheath = 1}};
    CHECK(wowee::game::hashEquipmentAppearance(a) != wowee::game::hashEquipmentAppearance(b));
}

TEST_CASE("Weapon swings and ready stances draw melee weapons at any sheath state", "[gesture-weapons]") {
    using namespace wowee::rendering;
    // TBC AnimationData.WeaponFlags: Attack1H 17, Ready1H 26, AttackOff 87 and
    // Whirlwind 126 have 0x20. Leotheras the Blind keeps sheath state 0 all
    // fight and natively holds his glaives through them.
    CHECK(animationSheathState(0x20, 0) == 1);
    // A drawn ranged weapon (bow shot) keeps the hand: the axe stays away.
    CHECK(animationSheathState(0x20, 2) == 2);
    CHECK(animationSheathState(0x20, 1) == 1);
    // Stand/Run (0) leave the unit's own state; sheathing flags are handled
    // by gestureSheathesWeapons / the stand state.
    CHECK(animationSheathState(0, 0) == 0);
    CHECK(animationSheathState(0, 2) == 2);
    CHECK(animationSheathState(0x10, 0) == 0);
    CHECK(animationSheathState(0x04, 1) == 1);
}

TEST_CASE("Thrown and non-shot 0x10 poses sheathe a drawn ranged weapon", "[animation][weapons]") {
    using wowee::rendering::poseAnimationSheathesWeapon;
    constexpr int8_t kMelee = 1;
    constexpr int8_t kRanged = 2;

    // No active sheathing pose -> nothing sheathed.
    CHECK_FALSE(poseAnimationSheathesWeapon(0, 0, kRanged, 2));
    CHECK_FALSE(poseAnimationSheathesWeapon(0, 0, kMelee, 1));

    // Bow/gun/crossbow shot animations (AttackBow 46, AttackRifle 49, WeaponFlags 0x10)
    // keep a drawn ranged weapon in hand, while still sheathing melee weapons.
    CHECK_FALSE(poseAnimationSheathesWeapon(46, 0x10, kRanged, 2));
    CHECK_FALSE(poseAnimationSheathesWeapon(49, 0x10, kRanged, 2));
    CHECK(poseAnimationSheathesWeapon(46, 0x10, kMelee, 1));
    CHECK(poseAnimationSheathesWeapon(49, 0x10, kMelee, 1));

    // Thrown animations (ReadyThrown 108, AttackThrown 107, WeaponFlags 0x10 — e.g.
    // Master Engineer Telonicus casting Bomb 37036) must sheathe/hide a drawn gun/bow.
    CHECK(poseAnimationSheathesWeapon(108, 0x10, kRanged, 2));
    CHECK(poseAnimationSheathesWeapon(107, 0x10, kRanged, 2));

    // Other 0x10 poses (AttackUnarmed 16, SitGround 97, EmoteWave 67) and 0x04 spell
    // casts (ReadySpellDirected 51, SpellCastDirected 53) also sheathe a drawn ranged weapon.
    CHECK(poseAnimationSheathesWeapon(16, 0x10, kRanged, 2));
    CHECK(poseAnimationSheathesWeapon(97, 0x10, kRanged, 2));
    CHECK(poseAnimationSheathesWeapon(67, 0x10, kRanged, 2));
    CHECK(poseAnimationSheathesWeapon(51, 0x04, kRanged, 2));
    CHECK(poseAnimationSheathesWeapon(53, 0x04, kRanged, 2));

    // Unit-level sheath check used by CharacterRenderer::weaponSheathingAnimation:
    // AttackBow (46) and AttackRifle (49) at sheathState 2 must NOT trigger local
    // CMSG_SETSHEATHED 0, while spell casts / emotes / thrown poses still do.
    using wowee::rendering::poseAnimationSheathesUnit;
    CHECK_FALSE(poseAnimationSheathesUnit(0, 0, 2));
    CHECK_FALSE(poseAnimationSheathesUnit(46, 0x10, 2));
    CHECK_FALSE(poseAnimationSheathesUnit(49, 0x10, 2));
    CHECK(poseAnimationSheathesUnit(46, 0x10, 1));
    CHECK(poseAnimationSheathesUnit(49, 0x10, 1));
    CHECK(poseAnimationSheathesUnit(107, 0x10, 2));
    CHECK(poseAnimationSheathesUnit(52, 0x04, 2));
    CHECK(poseAnimationSheathesUnit(67, 0x10, 2));
}

TEST_CASE("Emote procedures retain stand states and seated gesture eligibility", "[emote-stance]") {
    using namespace wowee::rendering;
    CHECK(emoteStandState(1, 3) == 3); // SLEEP and LIE share the authored sleep state.
    CHECK(emoteStandState(1, 1) == 1);
    CHECK(emoteStandState(1, 0) == 0);
    CHECK_FALSE(emoteStandState(0, 3));
    CHECK_FALSE(emoteStandState(2, 3));
    CHECK(standPose(*emoteStandState(1, 3)).idle == anim::SLEEP);
    for (uint8_t state : {1, 2, 4, 5, 6}) {
        CHECK(isSeatedStandState(state));
        CHECK(isSeatedBaseAnimation(standPose(state).idle));
        CHECK(emoteAllowsStandState(0, state)); // Wave allows sitting.
        CHECK_FALSE(emoteAllowsStandState(1, state)); // Bow requires standing.
    }
    CHECK_FALSE(isSeatedBaseAnimation(anim::STAND));
    CHECK_FALSE(isSeatedBaseAnimation(anim::SLEEP));
    CHECK_FALSE(emoteAllowsStandState(0, 3));
    CHECK(emoteAllowsStandState(0x200, 3));
    CHECK_FALSE(emoteAllowsStandState(0, 7));
    CHECK(emoteAllowsStandState(1, 0));
}

TEST_CASE("Detection eyes use observer concealment without hiding unrelated aura art", "[detection-marker]") {
    using namespace wowee::rendering;
    CHECK(isConcealmentDetectionMarker("spells\\detectstealth_state_head.m2"));
    CHECK(isConcealmentDetectionMarker("Spells/DetectStealth_State_Head.mdx"));
    CHECK_FALSE(isConcealmentDetectionMarker("Spells/Sleep_State_Head.m2"));
    CHECK_FALSE(isConcealmentDetectionMarker("spells/arcanepower_state_chest.m2"));
    const uint32_t stealth[] = {16, 0, 0};
    const uint32_t invisibility[] = {0, 18, 19};
    const uint32_t detector[] = {19, 17, 0};
    CHECK(auraConcealsObserver(stealth));
    CHECK(auraConcealsObserver(invisibility));
    CHECK_FALSE(auraConcealsObserver(detector));
}

TEST_CASE("Readable books retain authored poses when template metadata arrives", "[game-object-pose]") {
    using wowee::core::gameObjectPoseIsStateDriven;
    for (uint32_t type : {0u, 1u, 3u, 6u, 9u, 10u, 33u, 35u}) {
        CAPTURE(type);
        CHECK(gameObjectPoseIsStateDriven(type));
    }
    // Animated scenery and one-shot duel flags keep their own playback policy.
    for (uint32_t type : {5u, 16u, 25u}) {
        CAPTURE(type);
        CHECK_FALSE(gameObjectPoseIsStateDriven(type));
    }
}

TEST_CASE("Only door-like stateful GOs without state sequences freeze at bind pose in READY", "[game-object-pose]") {
    using wowee::core::gameObjectBindPoseShowsReady;
    // Buttons (1, Coilfang steam), chests (3, Blood of Heroes), traps (6,
    // Molten Core rune rings), texts (9), and goobers (10, Silithyst Geyser)
    // without state sequences loop Stand in the original.
    for (uint32_t type : {1u, 3u, 6u, 9u, 10u}) {
        CAPTURE(type);
        CHECK_FALSE(gameObjectBindPoseShowsReady(type));
    }
    for (uint32_t type : {0u, 33u, 35u}) {
        CAPTURE(type);
        CHECK(gameObjectBindPoseShowsReady(type));
    }
    for (uint32_t type : {5u, 16u, 25u}) {
        CAPTURE(type);
        CHECK_FALSE(gameObjectBindPoseShowsReady(type));
    }
}

TEST_CASE("ACTIVE shows Destroy only for stateful models without Open", "[game-object-pose]") {
    using wowee::core::gameObjectActiveShowsDestroy;
    // Brazier of Madness: BUTTON, G_BrazierTroll (Stand, Destroy, Custom0).
    CHECK(gameObjectActiveShowsDestroy(1u, false, false, true));
    // Thunderbrew Lager Keg: GOOBER, G_Keg (Stand, Destroy).
    CHECK(gameObjectActiveShowsDestroy(10u, false, false, true));
    // Supply Crate (GOOBER, G_Crate01) and Bar Door (DOOR) open instead.
    CHECK_FALSE(gameObjectActiveShowsDestroy(10u, true, true, true));
    CHECK_FALSE(gameObjectActiveShowsDestroy(0u, true, true, true));
    // Troll Hero Fire Portal: GENERIC, FireRift (Stand, Hold, Destroy).
    CHECK_FALSE(gameObjectActiveShowsDestroy(5u, false, false, true));
    CHECK_FALSE(gameObjectActiveShowsDestroy(1u, false, false, false));
}

TEST_CASE("READY loops Stand for GENERIC and CAMERA, Closed for the others", "[game-object-pose]") {
    using wowee::core::gameObjectReadyShowsStand;
    // ScholomanceCrystalBall01 glows steadily natively as Orb of the Blue Flight
    // (GENERIC) and Magisters' Terrace's Scrying Orb (CAMERA).
    CHECK(gameObjectReadyShowsStand(5u));
    CHECK(gameObjectReadyShowsStand(13u));
    // It pulses with Closed as Orb of Command (QUESTGIVER), Archmage Vargoth's
    // Orb (SPELL_FOCUS) and Orb of Domination (GOOBER); Warpwood Pod (CHEST)
    // stands as a closed pulsing bulb.
    for (uint32_t type : {2u, 3u, 8u, 10u}) {
        CAPTURE(type);
        CHECK_FALSE(gameObjectReadyShowsStand(type));
    }
}

TEST_CASE("Creature spline movement retains aquatic and aerial poses", "[creature-locomotion]") {
    using wowee::rendering::creatureMovingAnimation;
    CHECK(creatureMovingAnimation(false, false, false) == anim::RUN);
    CHECK(creatureMovingAnimation(false, false, true) == anim::WALK);
    CHECK(creatureMovingAnimation(false, true, false) == anim::SWIM);
    CHECK(creatureMovingAnimation(false, true, true) == anim::SWIM);
    CHECK(creatureMovingAnimation(true, false, true) == anim::FLY_FORWARD);
    CHECK(creatureMovingAnimation(true, true, true) == anim::FLY_FORWARD);
}

TEST_CASE("Spline gait follows speed against the model's unscaled Walk movingSpeed", "[creature-locomotion]") {
    // Native 8606 observations (_experiments/20261117T215856Z.berserker_gait and
    // _experiments/20261122T201500Z.akama_walk_gait_parity):
    // threshold is 2.0 * abs(M2 Walk.movingSpeed) without display/object scale.
    using wowee::rendering::splineGroundGaitWalks;
    const float trollDireWalk = 1.6666666f;      // Smolderthorn Berserker (cdi_scale=1.2)
    const float berserkerRun = 1.14286f * 7.0f;  // SpeedRun x 7 yd/s
    CHECK(splineGroundGaitWalks(berserkerRun * 0.3f, trollDireWalk));
    CHECK(splineGroundGaitWalks(berserkerRun * 0.4f, trollDireWalk));
    CHECK_FALSE(splineGroundGaitWalks(berserkerRun * 0.5f, trollDireWalk));
    CHECK_FALSE(splineGroundGaitWalks(berserkerRun * 0.7f, trollDireWalk));
    CHECK_FALSE(splineGroundGaitWalks(berserkerRun, trollDireWalk));

    const float elementalEarthWalk = 3.8888888f; // Rock Elemental (cdi_scale=1.0)
    CHECK(splineGroundGaitWalks(5.6f, elementalEarthWalk));
    CHECK(splineGroundGaitWalks(6.4f, elementalEarthWalk));
    CHECK(splineGroundGaitWalks(7.2f, elementalEarthWalk));
    CHECK_FALSE(splineGroundGaitWalks(8.0f, elementalEarthWalk));

    // Akama 23089 (display 21112, cdi_scale=2.6, Walk=2.5 -> threshold 5.0 yd/s)
    const float akamaWalk = 2.5f;
    CHECK(splineGroundGaitWalks(3.0f, akamaWalk));
    CHECK(splineGroundGaitWalks(4.2f, akamaWalk));
    CHECK(splineGroundGaitWalks(4.8f, akamaWalk));
    CHECK_FALSE(splineGroundGaitWalks(5.4f, akamaWalk));
    CHECK_FALSE(splineGroundGaitWalks(6.0f, akamaWalk));
    CHECK_FALSE(splineGroundGaitWalks(10.0f, akamaWalk));

    // Lord Sanguinar 20060 (display 20178, cdi_scale=3.0, BloodElfMale Walk=2.2222)
    const float bloodElfWalk = 2.2222222f;
    CHECK(splineGroundGaitWalks(4.0f, bloodElfWalk));
    CHECK_FALSE(splineGroundGaitWalks(4.5f, bloodElfWalk));
    CHECK_FALSE(splineGroundGaitWalks(12.0f, bloodElfWalk));

    // Zero-moveSpeed M2 models (DoomGuard.m2, HumanFemale.m2) fall back to
    // groundWalkSpeed * OBJECT_FIELD_SCALE_X, NOT CreatureDisplayInfo.scale:
    // - Unchained Doombringer 20900 (display 19945, cdi_scale=2.5, Walk=0):
    //   at serverScale=1.0 -> threshold 5.0 (2.5/3.6 walk, 6.0/10.0 run);
    //   at serverScale=2.0 -> threshold 10.0 (6.0 walks).
    // - Phantom Guardsman (display 16457, cdi_scale=1.15, Walk=0):
    //   at serverScale=1.0 -> threshold 5.0 (4.0 walks, 5.6 runs);
    //   at serverScale=2.0 -> threshold 10.0 (5.6 walks).
    using wowee::rendering::splineWalkStride;
    const auto doomS1 = splineWalkStride(std::nullopt, true, 2.5f, 1.0f);
    REQUIRE(doomS1.has_value());
    CHECK(*doomS1 == Catch::Approx(2.5f));
    CHECK(splineGroundGaitWalks(2.5f, *doomS1));
    CHECK(splineGroundGaitWalks(3.6f, *doomS1));
    CHECK_FALSE(splineGroundGaitWalks(5.6f, *doomS1));
    CHECK_FALSE(splineGroundGaitWalks(6.0f, *doomS1));
    CHECK_FALSE(splineGroundGaitWalks(10.0f, *doomS1));

    const auto doomS2 = splineWalkStride(std::nullopt, true, 2.5f, 2.0f);
    REQUIRE(doomS2.has_value());
    CHECK(*doomS2 == Catch::Approx(5.0f));
    CHECK(splineGroundGaitWalks(5.6f, *doomS2));
    CHECK(splineGroundGaitWalks(6.0f, *doomS2));
    CHECK_FALSE(splineGroundGaitWalks(10.2f, *doomS2));

    // Models lacking Walk animation return nullopt (always run).
    CHECK_FALSE(splineWalkStride(std::nullopt, false, 2.5f, 1.0f).has_value());
}

TEST_CASE("Aquatic creature surface limit preserves submerged server coordinates", "[creature-locomotion]") {
    using wowee::rendering::creatureSwimmingZ;
    const float depth = 0.75f * 2.083f * 1.15f;
    CHECK(creatureSwimmingZ(0.183376f, false, true, 0.183376f, depth) ==
          Catch::Approx(0.183376f - depth));
    CHECK(creatureSwimmingZ(-5.0f, false, true, 0.183376f, depth) == -5.0f);
    CHECK(creatureSwimmingZ(0.183376f, true, true, 0.183376f, depth) == 0.183376f);
    CHECK(creatureSwimmingZ(0.183376f, false, false, 0.183376f, depth) == 0.183376f);
    CHECK(creatureSwimmingZ(0.183376f, false, true, std::nullopt, depth) == 0.183376f);
}

// Build a synthetic AnimationData.dbc in memory.
// AnimationData.dbc layout: each record has at least 1 field (the animation ID).
// We use numFields=2 (id + dummy) to mirror the real DBC which has multiple fields.
static std::vector<uint8_t> buildAnimationDBC(const std::vector<uint32_t>& animIds) {
    const uint32_t numRecords = static_cast<uint32_t>(animIds.size());
    const uint32_t numFields = 2;  // id + a dummy field
    const uint32_t recordSize = numFields * 4;
    const uint32_t stringBlockSize = 1; // single null byte

    std::vector<uint8_t> data;
    data.reserve(20 + numRecords * recordSize + stringBlockSize);

    // Magic "WDBC"
    data.push_back('W'); data.push_back('D'); data.push_back('B'); data.push_back('C');

    auto writeU32 = [&](uint32_t v) {
        data.push_back(static_cast<uint8_t>(v & 0xFF));
        data.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        data.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        data.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    };

    writeU32(numRecords);
    writeU32(numFields);
    writeU32(recordSize);
    writeU32(stringBlockSize);

    // Records: [animId, 0]
    for (uint32_t id : animIds) {
        writeU32(id);
        writeU32(0);
    }

    // String block: single null
    data.push_back('\0');

    return data;
}

// ── nameFromId tests ────────────────────────────────────────────────────────

TEST_CASE("nameFromId returns correct names for known IDs", "[animation]") {
    REQUIRE(std::string(anim::nameFromId(anim::STAND)) == "STAND");
    REQUIRE(std::string(anim::nameFromId(anim::DEATH)) == "DEATH");
    REQUIRE(std::string(anim::nameFromId(anim::WALK)) == "WALK");
    REQUIRE(std::string(anim::nameFromId(anim::RUN)) == "RUN");
    REQUIRE(std::string(anim::nameFromId(anim::ATTACK_UNARMED)) == "ATTACK_UNARMED");
    REQUIRE(std::string(anim::nameFromId(anim::ATTACK_1H)) == "ATTACK_1H");
    REQUIRE(std::string(anim::nameFromId(anim::ATTACK_2H)) == "ATTACK_2H");
    REQUIRE(std::string(anim::nameFromId(anim::ATTACK_2H_LOOSE)) == "ATTACK_2H_LOOSE");
    REQUIRE(std::string(anim::nameFromId(anim::SPELL_CAST_DIRECTED)) == "SPELL_CAST_DIRECTED");
    REQUIRE(std::string(anim::nameFromId(anim::SPELL_CAST_OMNI)) == "SPELL_CAST_OMNI");
    REQUIRE(std::string(anim::nameFromId(anim::READY_1H)) == "READY_1H");
    REQUIRE(std::string(anim::nameFromId(anim::READY_2H)) == "READY_2H");
    REQUIRE(std::string(anim::nameFromId(anim::READY_2H_LOOSE)) == "READY_2H_LOOSE");
    REQUIRE(std::string(anim::nameFromId(anim::READY_UNARMED)) == "READY_UNARMED");
    REQUIRE(std::string(anim::nameFromId(anim::EMOTE_DANCE)) == "EMOTE_DANCE");
}

TEST_CASE("nameFromId returns UNKNOWN for out-of-range IDs", "[animation]") {
    REQUIRE(std::string(anim::nameFromId(anim::ANIM_COUNT)) == "UNKNOWN");
    REQUIRE(std::string(anim::nameFromId(anim::ANIM_COUNT + 1)) == "UNKNOWN");
    REQUIRE(std::string(anim::nameFromId(9999)) == "UNKNOWN");
    REQUIRE(std::string(anim::nameFromId(UINT32_MAX)) == "UNKNOWN");
}

TEST_CASE("nameFromId covers first and last IDs", "[animation]") {
    REQUIRE(std::string(anim::nameFromId(0)) == "STAND");
    REQUIRE(std::string(anim::nameFromId(anim::ANIM_COUNT - 1)) == "FLY_CARRIED_2H");
}

// ── validateAgainstDBC tests ────────────────────────────────────────────────

TEST_CASE("validateAgainstDBC handles null DBC", "[animation][dbc]") {
    // Should not crash - just logs a warning
    anim::validateAgainstDBC(nullptr);
}

TEST_CASE("validateAgainstDBC handles unloaded DBC", "[animation][dbc]") {
    auto dbc = std::make_shared<DBCFile>();
    REQUIRE_FALSE(dbc->isLoaded());
    // Should not crash - just logs a warning
    anim::validateAgainstDBC(dbc);
}

TEST_CASE("validateAgainstDBC with perfect match", "[animation][dbc]") {
    // Build a DBC containing IDs 0..ANIM_COUNT-1 (exact match)
    std::vector<uint32_t> allIds;
    for (uint32_t i = 0; i < anim::ANIM_COUNT; ++i) {
        allIds.push_back(i);
    }
    auto data = buildAnimationDBC(allIds);

    auto dbc = std::make_shared<DBCFile>();
    REQUIRE(dbc->load(data));
    REQUIRE(dbc->getRecordCount() == anim::ANIM_COUNT);

    // Should complete without crashing - all IDs match
    anim::validateAgainstDBC(dbc);
}

TEST_CASE("validateAgainstDBC with missing IDs in DBC", "[animation][dbc]") {
    // DBC only contains a subset of IDs - misses many
    std::vector<uint32_t> partialIds = {0, 1, 2, 4, 5};
    auto data = buildAnimationDBC(partialIds);

    auto dbc = std::make_shared<DBCFile>();
    REQUIRE(dbc->load(data));
    REQUIRE(dbc->getRecordCount() == 5);

    // Should log warnings for missing IDs but not crash
    anim::validateAgainstDBC(dbc);
}

TEST_CASE("validateAgainstDBC with extra IDs beyond range", "[animation][dbc]") {
    // DBC has some IDs beyond ANIM_COUNT
    std::vector<uint32_t> extraIds = {0, 1, 500, 600, 1000};
    auto data = buildAnimationDBC(extraIds);

    auto dbc = std::make_shared<DBCFile>();
    REQUIRE(dbc->load(data));
    REQUIRE(dbc->getRecordCount() == 5);

    // Should log info about extra DBC-only IDs but not crash
    anim::validateAgainstDBC(dbc);
}

TEST_CASE("validateAgainstDBC with empty DBC", "[animation][dbc]") {
    // DBC with zero records
    auto data = buildAnimationDBC({});

    auto dbc = std::make_shared<DBCFile>();
    REQUIRE(dbc->load(data));
    REQUIRE(dbc->getRecordCount() == 0);

    // Should log warnings for all ANIM_COUNT missing IDs but not crash
    anim::validateAgainstDBC(dbc);
}

TEST_CASE("validateAgainstDBC with single ID", "[animation][dbc]") {
    // DBC with just STAND (id=0)
    auto data = buildAnimationDBC({0});

    auto dbc = std::make_shared<DBCFile>();
    REQUIRE(dbc->load(data));
    REQUIRE(dbc->getRecordCount() == 1);

    anim::validateAgainstDBC(dbc);
}

TEST_CASE("ANIM_COUNT matches expected value", "[animation]") {
    REQUIRE(anim::ANIM_COUNT == 506);   // 0-505, 3.3.5's AnimationData.dbc
}

TEST_CASE("Animation constant IDs are unique and sequential from documentation", "[animation]") {
    // Verify key animation ID values match WoW's AnimationData.dbc layout
    REQUIRE(anim::STAND == 0);
    REQUIRE(anim::DEATH == 1);
    REQUIRE(anim::SPELL == 2);
    REQUIRE(anim::ATTACK_UNARMED == 16);
    REQUIRE(anim::ATTACK_1H == 17);
    REQUIRE(anim::ATTACK_2H == 18);
    REQUIRE(anim::ATTACK_2H_LOOSE == 19);
    REQUIRE(anim::READY_UNARMED == 25);
    REQUIRE(anim::READY_1H == 26);
    REQUIRE(anim::READY_2H == 27);
    REQUIRE(anim::READY_2H_LOOSE == 28);
    REQUIRE(anim::SPELL_CAST == 32);
    REQUIRE(anim::SPELL_CAST_DIRECTED == 53);
    REQUIRE(anim::SPELL_CAST_OMNI == 54);
    REQUIRE(anim::CHANNEL_CAST_DIRECTED == 124);
    REQUIRE(anim::CHANNEL_CAST_OMNI == 125);
    REQUIRE(anim::EMOTE_DANCE == 69);
}

TEST_CASE("IDs above 145 follow 3.3.5's AnimationData.dbc", "[animation]") {
    // The block these sit in was once transcribed from a list that dropped
    // Opened at 149, so everything after named something else in the game.
    REQUIRE(anim::OPENED == 149);
    REQUIRE(anim::DESTROY == 150);
    REQUIRE(anim::HOLD == 158);
    REQUIRE(anim::JUMP_LAND_RUN == 187);
    REQUIRE(anim::HOVER == 193);
    REQUIRE(anim::STEALTH_RUN == 223);
    REQUIRE(anim::FLY_STAND == 229);
    REQUIRE(anim::FLY_RUN == 234);
    REQUIRE(anim::EMOTE_YES == 185);
    REQUIRE(anim::EMOTE_TRAIN == 195);
    // A flying mount moves with Fly and holds still with Hover.
    REQUIRE(anim::FLY_FORWARD == anim::FLY);
    REQUIRE(anim::FLY_IDLE == anim::HOVER);
    REQUIRE(std::string(anim::nameFromId(193)) == "HOVER");
}

TEST_CASE("Type flag 0x8 creatures and model flag 0x2 units do not flinch from hits", "[hit-reaction]") {
    using namespace wowee::rendering;
    CHECK_FALSE(unitPlaysWound(anim::COMBAT_WOUND, true, 0x8, 0x0));      // Qiraji Gladiator
    CHECK_FALSE(unitPlaysWound(anim::COMBAT_CRITICAL, true, 0x848, 0x0)); // Amani'shi Warbringer
    CHECK_FALSE(unitPlaysWound(anim::COMBAT_WOUND, true, 0x4c, 0x0));     // Hex Lord Malacrass
    CHECK(unitPlaysWound(anim::COMBAT_WOUND, true, 0, 0x4));              // Defias Thug, HumanMale.mdx
    CHECK(unitPlaysWound(anim::COMBAT_WOUND, true, 0, 0x0));              // Defias Thug as a gnoll
    CHECK_FALSE(unitPlaysWound(anim::COMBAT_WOUND, true, 0, 0x3));        // Ras Frostwhisper, Lich.mdx
    CHECK_FALSE(unitPlaysWound(anim::COMBAT_CRITICAL, true, 0, 0x2));     // Skeleton.mdx, Ghoul.mdx
    CHECK_FALSE(unitPlaysWound(anim::COMBAT_WOUND, false, 0, 0x2));       // a player morphed into one
    CHECK(unitPlaysWound(anim::DODGE, true, 0x8, 0x2));
    CHECK(unitPlaysWound(anim::COMBAT_WOUND, false, 0x8, 0x4));           // players have no type flags
}

TEST_CASE("SpellVisualKit CharProc 11 freezes animation in place once a sequence is bound", "[freeze-anim]") {
    using namespace wowee::rendering;
    // CharProc 11: Stoned (10255), Freeze Anim (16245), Ice Block (27619/45438), Petrification (17624).
    CHECK(charProcFreezesAnimation(11));
    // Other CharProc codes (0 chain, 1 color, 9 area impacts, 13 light, 14 opacity) do not freeze.
    for (uint32_t proc : {0u, 1u, 2u, 7u, 9u, 12u, 13u, 14u}) {
        CAPTURE(proc);
        CHECK_FALSE(charProcFreezesAnimation(proc));
    }
    // Unfrozen units always allow playAnimation(); frozen units allow only initial bind (< 0).
    CHECK(frozenAnimationAllowsPlay(false, -1));
    CHECK(frozenAnimationAllowsPlay(false, 0));
    CHECK(frozenAnimationAllowsPlay(false, 42));
    CHECK(frozenAnimationAllowsPlay(true, -1));
    CHECK_FALSE(frozenAnimationAllowsPlay(true, 0));
    CHECK_FALSE(frozenAnimationAllowsPlay(true, 42));
}

TEST_CASE("Shapeshift and transform aura transitions play ImpactKit on server apply, alive exit, and death revert", "[aura][shapeshift]") {
    using namespace wowee::rendering;
    // Ghost Wolf (2645): SPELL_AURA_MOD_SHAPESHIFT (36), SPELL_AURA_MOD_INCREASE_SPEED (31)
    const uint32_t ghostWolfAuras[3] = {36, 31, 0};
    // Polymorph (118): SPELL_AURA_MOD_CONFUSE (5), SPELL_AURA_TRANSFORM (56)
    const uint32_t polymorphAuras[3] = {5, 56, 0};
    // Druid of the Fang Serpent Form (8041): SPELL_AURA_TRANSFORM (56), SPELL_AURA_PERIODIC_DAMAGE (27), SPELL_AURA_MOD_DAMAGE_DONE (13)
    const uint32_t serpentFormAuras[3] = {56, 27, 13};
    // Regular buff: SPELL_AURA_MOD_STAT (29)
    const uint32_t normalBuffAuras[3] = {29, 0, 0};

    CHECK(auraIsTransformOrShapeshift(ghostWolfAuras));
    CHECK(auraIsTransformOrShapeshift(polymorphAuras));
    CHECK(auraIsTransformOrShapeshift(serpentFormAuras));
    CHECK_FALSE(auraIsTransformOrShapeshift(normalBuffAuras));

    const std::vector<uint32_t> empty{};
    const std::vector<uint32_t> ghostWolf{311};
    const std::vector<uint32_t> bearOrSerpentForm{653};  // Bear Form 5487 and Serpent Form 8041 share SpellVisual 653
    const std::vector<uint32_t> catForm{4228};

    // Entering form: detected as added (caller skips replay if SMSG_SPELL_GO already fired ImpactKit,
    // and plays ImpactKit when applied directly by server update / .aura without SMSG_SPELL_GO),
    // while nothing is treated as removed.
    CHECK(addedTransformAuraVisuals(empty, ghostWolf) == std::vector<uint32_t>{311});
    CHECK(addedTransformAuraVisuals(empty, bearOrSerpentForm) == std::vector<uint32_t>{653});
    CHECK(removedTransformAuraVisuals(empty, ghostWolf).empty());
    CHECK(removedTransformAuraVisuals(empty, bearOrSerpentForm).empty());

    // Leaving Ghost Wolf -> humanoid (alive cancel/expiry or death): replays visual 311 ImpactKit.
    CHECK(addedTransformAuraVisuals(ghostWolf, empty).empty());
    CHECK(removedTransformAuraVisuals(ghostWolf, empty) == std::vector<uint32_t>{311});

    // Leaving Bear Form (5487) or Druid of the Fang Serpent Form (8041) -> native humanoid
    // on alive expiry/dispel OR death (RemoveAllAurasOnDeath + display revert): replays visual 653 ImpactKit.
    CHECK(addedTransformAuraVisuals(bearOrSerpentForm, empty).empty());
    CHECK(removedTransformAuraVisuals(bearOrSerpentForm, empty) == std::vector<uint32_t>{653});

    // Direct Bear -> Cat swap in one update: old form does not replay on exit; new form is reported in added.
    CHECK(removedTransformAuraVisuals(bearOrSerpentForm, catForm).empty());
    CHECK(addedTransformAuraVisuals(bearOrSerpentForm, catForm) == std::vector<uint32_t>{4228});

    // Steady state: neither added nor removed.
    CHECK(addedTransformAuraVisuals(ghostWolf, ghostWolf).empty());
    CHECK(removedTransformAuraVisuals(ghostWolf, ghostWolf).empty());
}
