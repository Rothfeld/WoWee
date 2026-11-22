#include <catch_amalgamated.hpp>

#include "rendering/animation/animation_ids.hpp"
#include "rendering/animation/spell_cast_anim_policy.hpp"

using namespace wowee::rendering;
using wowee::game::SpellAnimPhase;

namespace {

constexpr uint32_t kNoAnim = ~0u;
constexpr uint32_t kEarthgrabTotem = 8376;   // Precast 52, Cast 54
constexpr uint32_t kLightningBolt = 15234;   // Precast 51, Cast 53
constexpr uint32_t kChainLightning = 15305;  // Precast 51, Cast 53
constexpr uint32_t kHealingWave = 15982;     // Precast 52, Cast 54
constexpr uint32_t kChainHeal = 1064;        // Precast 51, Cast 53
constexpr uint32_t kArcaneMissiles = 5143;   // Channel 124
constexpr uint32_t kArcaneMissileTick = 7268;// Cast 53

} // namespace

TEST_CASE("Back-to-back cast-time spells let CAST release finish before entering next PRECAST",
          "[spell_cast_anim]") {
    SpellCastAnimState state{};

    // 1. Earthgrab Totem starts precast (ReadySpellOmni 52).
    auto a1 = stepRemoteSpellCastAnim(state, SpellAnimPhase::PRECAST, kEarthgrabTotem,
                                      anim::READY_SPELL_OMNI, kNoAnim, anim::STAND, false, 0);
    CHECK(a1.kind == SpellCastAnimAction::Kind::PLAY_LOOP);
    CHECK(a1.animation == anim::READY_SPELL_OMNI);
    REQUIRE(state.hold.has_value());
    CHECK(state.hold->spellId == kEarthgrabTotem);

    // 2. Earthgrab Totem finishes -> plays SpellCastOmni (54) one-shot returning to Stand.
    auto a2 = stepRemoteSpellCastAnim(state, SpellAnimPhase::CAST, kEarthgrabTotem,
                                      anim::SPELL_CAST_OMNI, kNoAnim, anim::STAND, false, 0);
    CHECK(a2.kind == SpellCastAnimAction::Kind::PLAY_ONESHOT);
    CHECK(a2.animation == anim::SPELL_CAST_OMNI);
    CHECK(a2.returnAnimation == anim::STAND);
    CHECK_FALSE(state.hold.has_value());
    REQUIRE(state.oneShot.has_value());
    CHECK(state.oneShot->animation == anim::SPELL_CAST_OMNI);

    // 3. 0ms or 150ms later, Lightning Bolt PRECAST (ReadySpellDirected 51) arrives while
    //    SpellCastOmni (54) is still playing -> MUST NOT interrupt 54; retargets return to 51.
    auto a3 = stepRemoteSpellCastAnim(state, SpellAnimPhase::PRECAST, kLightningBolt,
                                      anim::READY_SPELL_DIRECTED, kNoAnim, anim::STAND,
                                      /*playingOneShot=*/true, anim::SPELL_CAST_OMNI);
    CHECK(a3.kind == SpellCastAnimAction::Kind::RETARGET_ONESHOT_RETURN);
    CHECK(a3.returnAnimation == anim::READY_SPELL_DIRECTED);
    REQUIRE(state.hold.has_value());
    CHECK(state.hold->spellId == kLightningBolt);
    CHECK(state.hold->animation == anim::READY_SPELL_DIRECTED);
    REQUIRE(state.oneShot.has_value());
    CHECK(state.oneShot->animation == anim::SPELL_CAST_OMNI);

    // 4. Lightning Bolt finishes (after 54 already completed and transitioned into 51) ->
    //    plays SpellCastDirected (53) returning to Stand.
    auto a4 = stepRemoteSpellCastAnim(state, SpellAnimPhase::CAST, kLightningBolt,
                                      anim::SPELL_CAST_DIRECTED, kNoAnim, anim::STAND,
                                      /*playingOneShot=*/false, 0);
    CHECK(a4.kind == SpellCastAnimAction::Kind::PLAY_ONESHOT);
    CHECK(a4.animation == anim::SPELL_CAST_DIRECTED);
    CHECK(a4.returnAnimation == anim::STAND);

    // 5. 199ms later, Chain Lightning PRECAST (51) arrives while SpellCastDirected (53)
    //    is still playing -> retargets 53's return to 51 without cutting 53 short.
    auto a5 = stepRemoteSpellCastAnim(state, SpellAnimPhase::PRECAST, kChainLightning,
                                      anim::READY_SPELL_DIRECTED, kNoAnim, anim::STAND,
                                      /*playingOneShot=*/true, anim::SPELL_CAST_DIRECTED);
    CHECK(a5.kind == SpellCastAnimAction::Kind::RETARGET_ONESHOT_RETURN);
    CHECK(a5.returnAnimation == anim::READY_SPELL_DIRECTED);
}

TEST_CASE("Back-to-back instant CAST spells: same anim finishes, different anim crossfades",
          "[spell_cast_anim]") {
    SECTION("Same CAST animation ID does not restart the active one-shot") {
        SpellCastAnimState state{};
        auto a1 = stepRemoteSpellCastAnim(state, SpellAnimPhase::CAST, kEarthgrabTotem,
                                          anim::SPELL_CAST_OMNI, kNoAnim, anim::STAND, false, 0);
        CHECK(a1.kind == SpellCastAnimAction::Kind::PLAY_ONESHOT);
        CHECK(a1.animation == anim::SPELL_CAST_OMNI);

        // 250ms later, Healing Wave instant CAST (also SpellCastOmni 54) arrives:
        auto a2 = stepRemoteSpellCastAnim(state, SpellAnimPhase::CAST, kHealingWave,
                                          anim::SPELL_CAST_OMNI, kNoAnim, anim::STAND,
                                          /*playingOneShot=*/true, anim::SPELL_CAST_OMNI);
        CHECK(a2.kind == SpellCastAnimAction::Kind::RETARGET_ONESHOT_RETURN);
        CHECK(a2.returnAnimation == anim::STAND);
    }

    SECTION("Different CAST animation ID crossfades immediately into the new one-shot") {
        SpellCastAnimState state{};
        auto a1 = stepRemoteSpellCastAnim(state, SpellAnimPhase::CAST, kEarthgrabTotem,
                                          anim::SPELL_CAST_OMNI, kNoAnim, anim::STAND, false, 0);
        CHECK(a1.kind == SpellCastAnimAction::Kind::PLAY_ONESHOT);
        CHECK(a1.animation == anim::SPELL_CAST_OMNI);

        // 250ms later, Chain Heal instant CAST (SpellCastDirected 53) arrives:
        auto a2 = stepRemoteSpellCastAnim(state, SpellAnimPhase::CAST, kChainHeal,
                                          anim::SPELL_CAST_DIRECTED, kNoAnim, anim::STAND,
                                          /*playingOneShot=*/true, anim::SPELL_CAST_OMNI);
        CHECK(a2.kind == SpellCastAnimAction::Kind::PLAY_ONESHOT);
        CHECK(a2.animation == anim::SPELL_CAST_DIRECTED);
        CHECK(a2.returnAnimation == anim::STAND);
    }
}

TEST_CASE("Instant CAST during an active PRECAST or CHANNEL returns to that held animation",
          "[spell_cast_anim]") {
    SECTION("Instant spell during PRECAST preserves PRECAST hold") {
        SpellCastAnimState state{};
        auto a1 = stepRemoteSpellCastAnim(state, SpellAnimPhase::PRECAST, kHealingWave,
                                          anim::READY_SPELL_OMNI, kNoAnim, anim::STAND, false, 0);
        CHECK(a1.kind == SpellCastAnimAction::Kind::PLAY_LOOP);
        CHECK(a1.animation == anim::READY_SPELL_OMNI);

        // Unrelated instant Earthgrab Totem arrives mid-precast:
        auto a2 = stepRemoteSpellCastAnim(state, SpellAnimPhase::CAST, kEarthgrabTotem,
                                          anim::SPELL_CAST_OMNI, kNoAnim, anim::STAND, false, 0);
        CHECK(a2.kind == SpellCastAnimAction::Kind::PLAY_ONESHOT);
        CHECK(a2.animation == anim::SPELL_CAST_OMNI);
        CHECK(a2.returnAnimation == anim::READY_SPELL_OMNI);
        REQUIRE(state.hold.has_value());
        CHECK(state.hold->spellId == kHealingWave);
        CHECK(state.hold->animation == anim::READY_SPELL_OMNI);
    }

    SECTION("Per-tick CAST during CHANNEL preserves CHANNEL hold") {
        SpellCastAnimState state{};
        auto a1 = stepRemoteSpellCastAnim(state, SpellAnimPhase::CHANNEL, kArcaneMissiles,
                                          anim::CHANNEL_CAST_DIRECTED, kNoAnim, anim::STAND, false, 0);
        CHECK(a1.kind == SpellCastAnimAction::Kind::PLAY_LOOP);
        CHECK(a1.animation == anim::CHANNEL_CAST_DIRECTED);

        auto a2 = stepRemoteSpellCastAnim(state, SpellAnimPhase::CAST, kArcaneMissileTick,
                                          anim::SPELL_CAST_DIRECTED, kNoAnim, anim::STAND, false, 0);
        CHECK(a2.kind == SpellCastAnimAction::Kind::PLAY_ONESHOT);
        CHECK(a2.animation == anim::SPELL_CAST_DIRECTED);
        CHECK(a2.returnAnimation == anim::CHANNEL_CAST_DIRECTED);
        REQUIRE(state.hold.has_value());
        CHECK(state.hold->channel);
        CHECK(state.hold->spellId == kArcaneMissiles);
    }
}

TEST_CASE("STOP during an active CAST one-shot clears queued PRECAST hold without snapping",
          "[spell_cast_anim]") {
    SpellCastAnimState state{};
    stepRemoteSpellCastAnim(state, SpellAnimPhase::CAST, kEarthgrabTotem,
                            anim::SPELL_CAST_OMNI, kNoAnim, anim::READY_2H_LOOSE, false, 0);
    stepRemoteSpellCastAnim(state, SpellAnimPhase::PRECAST, kLightningBolt,
                            anim::READY_SPELL_DIRECTED, kNoAnim, anim::READY_2H_LOOSE,
                            true, anim::SPELL_CAST_OMNI);

    // Lightning Bolt is interrupted while Earthgrab Totem's SpellCastOmni is still playing:
    auto stop = stepRemoteSpellCastAnim(state, SpellAnimPhase::STOP, kLightningBolt,
                                        kNoAnim, kNoAnim, anim::READY_2H_LOOSE,
                                        true, anim::SPELL_CAST_OMNI);
    CHECK_FALSE(state.hold.has_value());
    CHECK(stop.kind == SpellCastAnimAction::Kind::RETARGET_ONESHOT_RETURN);
    CHECK(stop.returnAnimation == anim::READY_2H_LOOSE);
}

TEST_CASE("Stationary facing-only MonsterMove does not trigger creature locomotion",
          "[spell_cast_anim]") {
    CHECK_FALSE(shouldStartCreatureLocomotion(0, 0.0f));
    CHECK_FALSE(shouldStartCreatureLocomotion(1, 0.0f));
    CHECK_FALSE(shouldStartCreatureLocomotion(100, 0.0f));
    CHECK(shouldStartCreatureLocomotion(1000, 2.5f));
    CHECK(shouldStartCreatureLocomotion(500, 7.0f));
}

TEST_CASE("Brutallus Meteor Slash PRECAST lets in-flight melee swing finish before looping",
          "[spell_cast_anim][brutallus]") {
    constexpr uint32_t kMeteorSlash = 45150;
    SpellCastAnimState state{};

    // Meteor Slash (45150, Precast 57) arrives 86ms into Brutallus's AttackUnarmed (16):
    // must retarget the one-shot return to 57 instead of snapping mid-slash.
    auto pre = stepRemoteSpellCastAnim(state, SpellAnimPhase::PRECAST, kMeteorSlash,
                                       anim::SPECIAL_UNARMED, kNoAnim, anim::READY_UNARMED,
                                       /*playingOneShot=*/true, anim::ATTACK_UNARMED);
    CHECK(pre.kind == SpellCastAnimAction::Kind::RETARGET_ONESHOT_RETURN);
    CHECK(pre.returnAnimation == anim::SPECIAL_UNARMED);
    REQUIRE(state.hold.has_value());
    CHECK(state.hold->spellId == kMeteorSlash);

    // When Meteor Slash finishes (no cast release animation) after 57 was looping,
    // returns cleanly to ReadyUnarmed (25).
    auto go = stepRemoteSpellCastAnim(state, SpellAnimPhase::CAST, kMeteorSlash,
                                      kNoAnim, kNoAnim, anim::READY_UNARMED,
                                      /*playingOneShot=*/false, 0);
    CHECK(go.kind == SpellCastAnimAction::Kind::PLAY_LOOP);
    CHECK(go.animation == anim::READY_UNARMED);
    CHECK_FALSE(state.hold.has_value());
}

TEST_CASE("Active cast/channel hold takes priority over UNIT_FLAG_STUNNED for self-stunning channels",
          "[spell_cast_anim][stun]") {
    // Akama Soul Channel (40447) self-applies SPELL_AURA_MOD_STUN (UNIT_FLAG_STUNNED)
    // while channeling CHANNEL_CAST_DIRECTED (124): the channel pose must win over STUN (14).
    CHECK(remoteCastOrStunIdle(anim::CHANNEL_CAST_DIRECTED, /*stunned=*/true) == anim::CHANNEL_CAST_DIRECTED);
    CHECK(remoteCastOrStunIdle(anim::CHANNEL_CAST_OMNI, /*stunned=*/true) == anim::CHANNEL_CAST_OMNI);
    CHECK(remoteCastOrStunIdle(anim::READY_SPELL_DIRECTED, /*stunned=*/true) == anim::READY_SPELL_DIRECTED);

    // When no cast/channel hold is active (including after an external stun interrupts
    // a cast/channel via STOP), UNIT_FLAG_STUNNED resolves to STUN (14).
    CHECK(remoteCastOrStunIdle(0, /*stunned=*/true) == anim::STUN);
    CHECK(remoteCastOrStunIdle(0, /*stunned=*/false) == 0);

    // Full sequence: unit channels 40447, then external stun interrupts via STOP while
    // UNIT_FLAG_STUNNED is set -> transitions cleanly to STUN (14).
    SpellCastAnimState state{};
    const auto start = stepRemoteSpellCastAnim(
        state, SpellAnimPhase::CHANNEL, 40447, anim::CHANNEL_CAST_DIRECTED,
        kNoAnim, anim::STAND, false, 0);
    CHECK(start.kind == SpellCastAnimAction::Kind::PLAY_LOOP);
    CHECK(start.animation == anim::CHANNEL_CAST_DIRECTED);
    REQUIRE(state.hold.has_value());
    CHECK(remoteCastOrStunIdle(state.hold->animation, /*stunned=*/true) == anim::CHANNEL_CAST_DIRECTED);

    // On STOP, nonCastIdle() evaluates with hold cleared -> returns STUN (14).
    const uint32_t postStopIdle = remoteCastOrStunIdle(0, /*stunned=*/true);
    const auto stop = stepRemoteSpellCastAnim(
        state, SpellAnimPhase::STOP, 40447, kNoAnim, kNoAnim, postStopIdle, false, 0);
    CHECK(stop.kind == SpellCastAnimAction::Kind::PLAY_LOOP);
    CHECK(stop.animation == anim::STUN);
    CHECK_FALSE(state.hold.has_value());
}

TEST_CASE("Aura StateKit animation takes priority over UNIT_FLAG_STUNNED for boss phase/defeat spells",
          "[spell_cast_anim][stun][illidan]") {
    constexpr uint32_t kCustomSpell02 = 214; // Illidan Demon Transform 1 (40511)
    constexpr uint32_t kCustomSpell03 = 215; // Illidan Demon Transform 2 (40398) & Caged (40695)
    constexpr uint32_t kCustomSpell04 = 216; // Illidan Demon Transform 3 (40510)
    constexpr uint32_t kCustomSpell06 = 218; // Illidan Death collapse (41218)
    constexpr uint32_t kCustomSpell07 = 219; // Illidan Death kneel loop (41220)

    for (uint32_t auraAnim : {kCustomSpell02, kCustomSpell03, kCustomSpell04, kCustomSpell06, kCustomSpell07}) {
        const auto chosen = remoteOverlayIdle(0, auraAnim, kNoAnim, /*stunned=*/true);
        REQUIRE(chosen.has_value());
        CHECK(*chosen == auraAnim);
    }

    // Active cast/channel hold still outranks aura StateKit animation.
    const auto castWins = remoteOverlayIdle(anim::CHANNEL_CAST_DIRECTED, kCustomSpell03, kNoAnim, /*stunned=*/true);
    REQUIRE(castWins.has_value());
    CHECK(*castWins == anim::CHANNEL_CAST_DIRECTED);

    // Explicit AnimID = 0 (Stand) on an aura StateKit also outranks generic STUN.
    const auto explicitStand = remoteOverlayIdle(0, anim::STAND, kNoAnim, /*stunned=*/true);
    REQUIRE(explicitStand.has_value());
    CHECK(*explicitStand == anim::STAND);

    // Without cast hold or aura StateKit animation, UNIT_FLAG_STUNNED resolves to STUN (14).
    const auto plainStun = remoteOverlayIdle(0, kNoAnim, kNoAnim, /*stunned=*/true);
    REQUIRE(plainStun.has_value());
    CHECK(*plainStun == anim::STUN);
    CHECK_FALSE(remoteOverlayIdle(0, kNoAnim, kNoAnim, /*stunned=*/false).has_value());
}

TEST_CASE("Non-looping PRECAST animation finishes its authored clip when CAST has no animation",
          "[spell_cast_anim][illidan]") {
    // Illidan Phase 2 Throw Glaive Visual (39849): 500 ms cast time, PrecastKit 7713 ->
    // DragonSpitHover (183, 2100 ms, M2Sequence::flags=0x1 non-looping), CastKit = 0.
    constexpr uint32_t kThrowGlaiveVisual = 39849;
    constexpr uint32_t kDragonSpitHover = 183;
    constexpr uint32_t kHover = 193;

    SpellCastAnimState state{};
    const auto pre = stepRemoteSpellCastAnim(
        state, SpellAnimPhase::PRECAST, kThrowGlaiveVisual, kDragonSpitHover, kNoAnim,
        kHover, /*playingOneShot=*/false, 0, /*kitAnimNonLooping=*/true);
    CHECK(pre.kind == SpellCastAnimAction::Kind::PLAY_ONESHOT);
    CHECK(pre.animation == kDragonSpitHover);
    CHECK(pre.returnAnimation == kHover);
    REQUIRE(state.oneShot.has_value());
    CHECK(state.oneShot->animation == kDragonSpitHover);

    // At t=500 ms, SMSG_SPELL_GO arrives with CastKit=0 (kNoAnim) while 183 is still playing:
    // must NOT snap back to Hover (193); retargets return to Hover so the 2100 ms throw finishes.
    const auto go = stepRemoteSpellCastAnim(
        state, SpellAnimPhase::CAST, kThrowGlaiveVisual, kNoAnim, kNoAnim,
        kHover, /*playingOneShot=*/true, kDragonSpitHover);
    CHECK(go.kind == SpellCastAnimAction::Kind::RETARGET_ONESHOT_RETURN);
    CHECK(go.returnAnimation == kHover);
    CHECK_FALSE(state.hold.has_value());
}
