// Animation ID validation tests - covers nameFromId() and validateAgainstDBC()
#include <catch_amalgamated.hpp>
#include "rendering/animation/animation_ids.hpp"
#include "rendering/animation/creature_locomotion.hpp"
#include "core/game_object_animation_policy.hpp"
#include "rendering/aura_marker_visibility.hpp"
#include "pipeline/dbc_loader.hpp"
#include "rendering/animation/stand_pose.hpp"
#include "rendering/animation/gesture_weapons.hpp"
#include <cstring>
#include <vector>

using wowee::pipeline::DBCFile;
namespace anim = wowee::rendering::anim;

TEST_CASE("Gesture weapon policy respects NOSHEATHE and both animation layers", "[gesture-weapons]") {
    using namespace wowee::rendering;
    CHECK(gestureSheathesWeapons(0x800, 0x10)); // TBC Wave emote 3 / animation 67.
    CHECK_FALSE(gestureSheathesWeapons(4, 0x10)); // Wave NOSHEATHE 70, same animation.
    CHECK(gestureSheathesWeapons(0, 4));
    CHECK_FALSE(gestureSheathesWeapons(0x800, 0));
    CHECK_FALSE(gestureSheathesWeapons(0, 0x20));
    CHECK(gestureWeaponPoseActive(67, 67, 0));
    CHECK(gestureWeaponPoseActive(67, anim::SITTING, 67));
    CHECK_FALSE(gestureWeaponPoseActive(67, anim::SITTING, 0));
    CHECK_FALSE(gestureWeaponPoseActive(67, anim::RUN, 0));
    CHECK_FALSE(gestureWeaponPoseActive(0, anim::STAND, 0));
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

TEST_CASE("Creature spline movement retains aquatic and aerial poses", "[creature-locomotion]") {
    using wowee::rendering::creatureMovingAnimation;
    CHECK(creatureMovingAnimation(false, false, false) == anim::RUN);
    CHECK(creatureMovingAnimation(false, false, true) == anim::WALK);
    CHECK(creatureMovingAnimation(false, true, false) == anim::SWIM);
    CHECK(creatureMovingAnimation(false, true, true) == anim::SWIM);
    CHECK(creatureMovingAnimation(true, false, true) == anim::FLY_FORWARD);
    CHECK(creatureMovingAnimation(true, true, true) == anim::FLY_FORWARD);
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
