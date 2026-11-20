// Upper-body animation split over locomotion (M2 SpineLow keyBoneId 4 +
// AnimationData.dbc BodyFlags 0x08 / 0x40). Verified against TBC 2.4.3.8606
// on Felguard pet attack-then-return (_experiments/20261120T131800Z.felguard_pet_swing).
#include <catch_amalgamated.hpp>

#include "rendering/animation/animation_ids.hpp"
#include "rendering/animation/upper_body_split.hpp"

using namespace wowee::rendering;

namespace {
constexpr uint32_t kBfStand = 0x0001u;
constexpr uint32_t kBfRunWalk = 0x0041u;
constexpr uint32_t kBfSwim = 0x0141u;
constexpr uint32_t kBfAttack = 0x0128u;
constexpr uint32_t kBfSpellCast = 0x0108u;
constexpr uint32_t kBfSpecial = 0x0120u;
constexpr uint32_t kBfDeath = 0x0300u;
constexpr uint32_t kBfReady = 0x0100u;
} // namespace

TEST_CASE("AnimationData BodyFlags classify upper-body and locomotion animations", "[anim][upper_body]") {
    CHECK(isUpperBodySplitAnimation(anim::ATTACK_UNARMED, kBfAttack));
    CHECK(isUpperBodySplitAnimation(anim::ATTACK_1H, kBfAttack));
    CHECK(isUpperBodySplitAnimation(anim::ATTACK_2H, kBfAttack));
    CHECK(isUpperBodySplitAnimation(anim::ATTACK_2H_LOOSE, kBfAttack));
    CHECK(isUpperBodySplitAnimation(anim::SPELL_CAST_DIRECTED, kBfSpellCast));
    CHECK(isUpperBodySplitAnimation(anim::SPELL_CAST_OMNI, kBfSpellCast));
    CHECK_FALSE(isUpperBodySplitAnimation(anim::WHIRLWIND, kBfSpecial));
    CHECK_FALSE(isUpperBodySplitAnimation(anim::DEATH, kBfDeath));
    CHECK_FALSE(isUpperBodySplitAnimation(anim::RUN, kBfRunWalk));

    CHECK(isLocomotionBaseAnimation(anim::WALK, kBfRunWalk));
    CHECK(isLocomotionBaseAnimation(anim::RUN, kBfRunWalk));
    CHECK(isLocomotionBaseAnimation(anim::SWIM, kBfSwim));
    CHECK_FALSE(isLocomotionBaseAnimation(anim::STAND, kBfStand));
    CHECK_FALSE(isLocomotionBaseAnimation(anim::READY_1H, kBfReady));
    CHECK_FALSE(isLocomotionBaseAnimation(anim::DEATH, kBfDeath));
}

TEST_CASE("In-progress attack swing promotes to upper body when Run or Walk starts", "[anim][upper_body]") {
    UpperBodyPromoteInput in;
    in.hasSpineLow = true;
    in.riding = false;
    in.previousSequenceValid = true;
    in.previousLoop = false;
    in.previousAnimId = anim::ATTACK_2H_LOOSE;
    in.previousBodyFlags = kBfAttack;
    in.previousTimeMs = 220.0f;
    in.previousDurationMs = 1500.0f;
    in.incomingAnimId = anim::RUN;
    in.incomingBodyFlags = kBfRunWalk;
    in.incomingLoop = true;

    CHECK(shouldPromoteToUpperBodyOnBaseChange(in));

    // Walk also promotes.
    in.incomingAnimId = anim::WALK;
    CHECK(shouldPromoteToUpperBodyOnBaseChange(in));

    // Models without SpineLow (quadrupeds like Wolf) cannot split.
    in.hasSpineLow = false;
    CHECK_FALSE(shouldPromoteToUpperBodyOnBaseChange(in));
    in.hasSpineLow = true;

    // Finished one-shot does not promote.
    in.previousTimeMs = 1500.0f;
    CHECK_FALSE(shouldPromoteToUpperBodyOnBaseChange(in));
    in.previousTimeMs = 220.0f;

    // Looping base (Stand / Ready) does not promote.
    in.previousLoop = true;
    CHECK_FALSE(shouldPromoteToUpperBodyOnBaseChange(in));
    in.previousLoop = false;

    // Full-body special (Whirlwind, no 0x08 flag) does not promote.
    in.previousAnimId = anim::WHIRLWIND;
    in.previousBodyFlags = kBfSpecial;
    CHECK_FALSE(shouldPromoteToUpperBodyOnBaseChange(in));
    in.previousAnimId = anim::ATTACK_2H_LOOSE;
    in.previousBodyFlags = kBfAttack;

    // Death / Stand do not trigger promotion.
    in.incomingAnimId = anim::DEATH;
    in.incomingBodyFlags = kBfDeath;
    in.incomingLoop = false;
    CHECK_FALSE(shouldPromoteToUpperBodyOnBaseChange(in));

    in.incomingAnimId = anim::STAND;
    in.incomingBodyFlags = kBfStand;
    in.incomingLoop = true;
    CHECK_FALSE(shouldPromoteToUpperBodyOnBaseChange(in));
}

TEST_CASE("One-shot swing arriving while already running plays on upper body", "[anim][upper_body]") {
    UpperBodyPlayInput in;
    in.hasSpineLow = true;
    in.riding = false;
    in.currentSequenceValid = true;
    in.currentLoop = true;
    in.currentAnimId = anim::RUN;
    in.currentBodyFlags = kBfRunWalk;
    in.incomingAnimId = anim::ATTACK_1H;
    in.incomingBodyFlags = kBfAttack;
    in.incomingLoop = false;
    in.incomingHoldFinalPose = false;

    CHECK(shouldPlayOnUpperBodyOverLocomotion(in));

    // Instant cast while running also plays on upper body.
    in.incomingAnimId = anim::SPELL_CAST_OMNI;
    in.incomingBodyFlags = kBfSpellCast;
    CHECK(shouldPlayOnUpperBodyOverLocomotion(in));

    // Quadruped without SpineLow plays full-body.
    in.hasSpineLow = false;
    CHECK_FALSE(shouldPlayOnUpperBodyOverLocomotion(in));
    in.hasSpineLow = true;

    // Stationary unit plays full-body.
    in.currentAnimId = anim::STAND;
    in.currentBodyFlags = kBfStand;
    CHECK_FALSE(shouldPlayOnUpperBodyOverLocomotion(in));
    in.currentAnimId = anim::RUN;
    in.currentBodyFlags = kBfRunWalk;

    // Full-body Whirlwind or Death does not split onto upper body.
    in.incomingAnimId = anim::WHIRLWIND;
    in.incomingBodyFlags = kBfSpecial;
    CHECK_FALSE(shouldPlayOnUpperBodyOverLocomotion(in));
    in.incomingAnimId = anim::DEATH;
    in.incomingBodyFlags = kBfDeath;
    CHECK_FALSE(shouldPlayOnUpperBodyOverLocomotion(in));
}

TEST_CASE("Upper-body one-shot survives Run->Stand arrival but clears on Death/Stun/Attack", "[anim][upper_body]") {
    CHECK(upperBodyAllowsBase(anim::RUN, kBfRunWalk, false));
    CHECK(upperBodyAllowsBase(anim::WALK, kBfRunWalk, false));
    CHECK(upperBodyAllowsBase(anim::SWIM, kBfSwim, false));
    CHECK(upperBodyAllowsBase(anim::STAND, kBfStand, false));
    CHECK(upperBodyAllowsBase(anim::READY_1H, kBfReady, false));
    CHECK(upperBodyAllowsBase(anim::READY_2H_LOOSE, 0u, false));
    CHECK(upperBodyAllowsBase(anim::MOUNT, 0u, true));
    CHECK_FALSE(upperBodyAllowsBase(anim::MOUNT, 0u, false));
    CHECK_FALSE(upperBodyAllowsBase(anim::DEATH, kBfDeath, false));
    CHECK_FALSE(upperBodyAllowsBase(anim::DEAD, 0x0900u, false));
    CHECK_FALSE(upperBodyAllowsBase(anim::STUN, 0x0100u, false));
    CHECK_FALSE(upperBodyAllowsBase(anim::WHIRLWIND, kBfSpecial, false));
    CHECK_FALSE(upperBodyAllowsBase(anim::ATTACK_1H, kBfAttack, false));
}
