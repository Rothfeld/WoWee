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
    CHECK(upperBodyAllowsBase(anim::SHUFFLE_LEFT, kBfStand, false));
    CHECK(upperBodyAllowsBase(anim::SHUFFLE_RIGHT, kBfStand, false));
}

TEST_CASE("Turn shuffle preserves stationary upper-body stance above SpineLow", "[anim][upper_body]") {
    CHECK(isTurnShuffleAnimation(anim::SHUFFLE_LEFT));
    CHECK(isTurnShuffleAnimation(anim::SHUFFLE_RIGHT));
    CHECK_FALSE(isTurnShuffleAnimation(anim::STAND));
    CHECK_FALSE(isTurnShuffleAnimation(anim::RUN));

    CHECK(isLocomotionBaseAnimation(anim::SHUFFLE_LEFT, kBfStand));
    CHECK(isLocomotionBaseAnimation(anim::SHUFFLE_RIGHT, kBfStand));

    ShuffleUpperBodyInput in;
    in.hasSpineLow = true;
    in.riding = false;
    in.previousSequenceValid = true;
    in.previousLoop = true;
    in.previousAnimId = anim::STAND;
    in.upperBodyActive = false;
    in.incomingAnimId = anim::SHUFFLE_LEFT;
    in.incomingLoop = true;

    CHECK(shouldPreserveUpperBodyStanceOnShuffle(in));

    in.incomingAnimId = anim::SHUFFLE_RIGHT;
    CHECK(shouldPreserveUpperBodyStanceOnShuffle(in));

    for (uint32_t stance : {anim::READY_UNARMED, anim::READY_1H, anim::READY_2H,
                            anim::READY_2H_LOOSE, anim::READY_BOW, anim::READY_RIFLE,
                            anim::READY_THROWN, anim::READY_SPELL_DIRECTED, anim::READY_SPELL_OMNI,
                            anim::HOLD_BOW, anim::HOLD_RIFLE, anim::HOLD_THROWN,
                            anim::STEALTH_STAND}) {
        in.previousAnimId = stance;
        CHECK(shouldPreserveUpperBodyStanceOnShuffle(in));
        CHECK(upperBodyAllowsBase(stance, kBfReady, false));
    }

    // Models without SpineLow (quadrupeds) play full-body shuffle.
    in.previousAnimId = anim::STAND;
    in.hasSpineLow = false;
    CHECK_FALSE(shouldPreserveUpperBodyStanceOnShuffle(in));
    in.hasSpineLow = true;

    // Already have an active upper-body layer (e.g. Stand kept from ShuffleLeft
    // when switching to ShuffleRight, or an active one-shot): do not overwrite.
    in.upperBodyActive = true;
    CHECK_FALSE(shouldPreserveUpperBodyStanceOnShuffle(in));
    in.upperBodyActive = false;

    // Non-stationary base (Run, Death) does not promote as a shuffle stance.
    in.previousAnimId = anim::RUN;
    CHECK_FALSE(shouldPreserveUpperBodyStanceOnShuffle(in));
    in.previousAnimId = anim::DEATH;
    CHECK_FALSE(shouldPreserveUpperBodyStanceOnShuffle(in));

    // One-shot upper-body action arriving while shuffling plays on upper body.
    UpperBodyPlayInput playIn;
    playIn.hasSpineLow = true;
    playIn.riding = false;
    playIn.currentSequenceValid = true;
    playIn.currentLoop = true;
    playIn.currentAnimId = anim::SHUFFLE_LEFT;
    playIn.currentBodyFlags = kBfStand;
    playIn.incomingAnimId = anim::ATTACK_1H;
    playIn.incomingBodyFlags = kBfAttack;
    playIn.incomingLoop = false;
    playIn.incomingHoldFinalPose = false;
    CHECK(shouldPlayOnUpperBodyOverLocomotion(playIn));
}
