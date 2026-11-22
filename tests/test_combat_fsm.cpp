// CombatFSM unit tests
#include <catch_amalgamated.hpp>
#include "rendering/animation/combat_fsm.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "rendering/animation/hit_reaction.hpp"
#include "rendering/animation/melee_swing_queue.hpp"
#include "game/inventory.hpp"

using namespace wowee::rendering;
namespace anim = wowee::rendering::anim;

static AnimCapabilitySet makeCombatCaps() {
    AnimCapabilitySet caps;
    caps.resolvedStand = anim::STAND;
    caps.resolvedCombatIdle = anim::READY_UNARMED;
    caps.resolvedMelee1H = anim::ATTACK_1H;
    caps.resolvedMelee2H = anim::ATTACK_2H;
    caps.resolvedMeleeUnarmed = anim::ATTACK_UNARMED;
    caps.resolvedReadyUnarmed = anim::READY_UNARMED;
    caps.resolvedStun = anim::STUN;
    caps.resolvedUnsheathe = anim::SHEATHE;
    caps.resolvedSheathe = anim::SHEATHE;
    caps.hasMelee = true;
    return caps;
}

static CombatFSM::Input combatInput() {
    CombatFSM::Input in;
    in.inCombat = true;
    in.grounded = true;
    return in;
}

static WeaponLoadout unarmedLoadout() {
    return WeaponLoadout{}; // Default is unarmed (inventoryType=0)
}

TEST_CASE("CombatFSM: INACTIVE by default", "[combat]") {
    CombatFSM fsm;
    CHECK(fsm.getState() == CombatFSM::State::INACTIVE);
    CHECK_FALSE(fsm.isActive());
}

TEST_CASE("CombatFSM: unarmed combat uses fist-ready and fist-swing animations", "[combat][unarmed]") {
    CombatFSM fsm;
    auto caps = makeCombatCaps();
    auto loadout = unarmedLoadout();
    auto in = combatInput();
    in.hasUnsheathe = false;

    auto ready = fsm.resolve(in, caps, loadout);
    REQUIRE(ready.valid);
    CHECK(fsm.getState() == CombatFSM::State::COMBAT_IDLE);
    CHECK(ready.animId == anim::READY_UNARMED);

    in.meleeSwingTimer = 0.5f;
    auto swing = fsm.resolve(in, caps, loadout);
    REQUIRE(swing.valid);
    CHECK(fsm.getState() == CombatFSM::State::MELEE_SWING);
    CHECK(swing.animId == anim::ATTACK_UNARMED);
}

TEST_CASE("CombatFSM: equipped fist weapons retain fist-weapon animations", "[combat][fist-weapon]") {
    CombatFSM fsm;
    auto caps = makeCombatCaps();
    caps.resolvedReadyFist = anim::READY_1H;
    caps.resolvedMeleeFist = anim::ATTACK_1H_PIERCE;
    WeaponLoadout loadout;
    loadout.inventoryType = wowee::game::InvType::ONE_HAND;
    loadout.isFist = true;
    auto in = combatInput();

    fsm.setState(CombatFSM::State::COMBAT_IDLE);
    CHECK(fsm.resolve(in, caps, loadout).animId == anim::READY_1H);

    in.meleeSwingTimer = 0.5f;
    CHECK(fsm.resolve(in, caps, loadout).animId == anim::ATTACK_1H_PIERCE);
}

TEST_CASE("CombatFSM: INACTIVE → UNSHEATHE on COMBAT_ENTER event", "[combat]") {
    CombatFSM fsm;
    fsm.onEvent(AnimEvent::COMBAT_ENTER);
    CHECK(fsm.getState() == CombatFSM::State::UNSHEATHE);
    CHECK(fsm.isActive());
}

TEST_CASE("CombatFSM: moving in combat does not replay the unsheathe", "[combat]") {
    CombatFSM fsm;
    auto caps = makeCombatCaps();
    auto wl = unarmedLoadout();
    auto in = combatInput();
    in.hasUnsheathe = true;

    fsm.setState(CombatFSM::State::COMBAT_IDLE);
    fsm.resolve(in, caps, wl);
    in.moving = true;
    fsm.resolve(in, caps, wl);
    CHECK(fsm.getState() == CombatFSM::State::INACTIVE);
    in.moving = false;
    fsm.resolve(in, caps, wl);
    CHECK(fsm.getState() == CombatFSM::State::COMBAT_IDLE);
}

TEST_CASE("CombatFSM: stun overrides active combat", "[combat]") {
    CombatFSM fsm;
    auto caps = makeCombatCaps();
    auto wl = unarmedLoadout();

    // Enter combat
    fsm.onEvent(AnimEvent::COMBAT_ENTER);

    // Stun
    fsm.setStunned(true);
    auto in = combatInput();
    auto out = fsm.resolve(in, caps, wl);

    CHECK(fsm.isStunned());
    REQUIRE(out.valid);
    CHECK(out.animId == anim::STUN);
}

TEST_CASE("CombatFSM: stun does not override swimming", "[combat]") {
    CombatFSM fsm;
    auto caps = makeCombatCaps();
    auto wl = unarmedLoadout();

    fsm.setStunned(true);
    auto in = combatInput();
    in.swimming = true;
    auto out = fsm.resolve(in, caps, wl);

    // Swimming overrides combat entirely - FSM should go inactive
    // The exact behavior depends on implementation, but stun should not
    // force an animation while swimming
    CHECK(fsm.getState() != CombatFSM::State::STUNNED);
}

TEST_CASE("CombatFSM: spell cast sequence", "[combat]") {
    CombatFSM fsm;
    auto caps = makeCombatCaps();
    auto wl = unarmedLoadout();

    fsm.startSpellCast(anim::SPELL_PRECAST, anim::SPELL, true, anim::SPELL_CAST);
    CHECK(fsm.getState() == CombatFSM::State::SPELL_PRECAST);

    auto in = combatInput();
    auto out = fsm.resolve(in, caps, wl);
    REQUIRE(out.valid);
    CHECK(out.animId == anim::SPELL_PRECAST);
}

TEST_CASE("CombatFSM: stopSpellCast transitions to finalize", "[combat]") {
    CombatFSM fsm;
    auto caps = makeCombatCaps();
    auto wl = unarmedLoadout();

    fsm.startSpellCast(0, anim::SPELL, true, anim::SPELL_CAST);
    // Skip precast (animId=0), go to casting
    fsm.setState(CombatFSM::State::SPELL_CASTING);

    fsm.stopSpellCast();
    CHECK(fsm.getState() == CombatFSM::State::SPELL_FINALIZE);
}

TEST_CASE("CombatFSM: a released cast plays its kit animation once", "[combat][cast-kit]") {
    // Scarlet Wizard Arcane Explosion: cast kit 1004 -> SpellCastOmni, no hold.
    CombatFSM fsm;
    fsm.startSpellCast(0, anim::SPELL_CAST_OMNI, false, anim::SPELL_CAST_OMNI);
    fsm.stopSpellCast();
    CHECK(fsm.getState() == CombatFSM::State::SPELL_FINALIZE);
    auto in = combatInput();
    in.inCombat = false;
    auto out = fsm.resolve(in, makeCombatCaps(), unarmedLoadout());
    REQUIRE(out.valid);
    CHECK(out.animId == anim::SPELL_CAST_OMNI);
    CHECK_FALSE(out.loop);
}

TEST_CASE("CombatFSM: cancelSpellCast leaves without a release", "[combat][cast-kit]") {
    // An interrupted Frostbolt drops ReadySpellDirected; no SpellCastDirected.
    CombatFSM fsm;
    fsm.startSpellCast(0, anim::READY_SPELL_DIRECTED, true, 0);
    CHECK(fsm.getState() == CombatFSM::State::SPELL_CASTING);
    fsm.cancelSpellCast();
    CHECK(fsm.getState() == CombatFSM::State::INACTIVE);
}

TEST_CASE("CombatFSM: hit reaction", "[combat]") {
    CombatFSM fsm;
    auto caps = makeCombatCaps();
    auto wl = unarmedLoadout();

    fsm.triggerHitReaction(anim::COMBAT_WOUND);
    CHECK(fsm.getState() == CombatFSM::State::HIT_REACTION);
}

// The renderer still shows the ready stance on the frame the reaction is
// requested; that is not the reaction's completion.
TEST_CASE("CombatFSM: hit reaction plays before it completes", "[combat]") {
    CombatFSM fsm;
    auto caps = makeCombatCaps();
    auto wl = unarmedLoadout();
    fsm.setState(CombatFSM::State::COMBAT_IDLE);

    fsm.triggerHitReaction(anim::DODGE);
    auto in = combatInput();
    in.haveAnimState = true;
    in.currentAnimId = anim::READY_UNARMED;
    in.currentAnimTime = 500.0f;
    in.currentAnimDuration = 1000.0f;
    auto first = fsm.resolve(in, caps, wl);
    REQUIRE(first.valid);
    CHECK(first.animId == anim::DODGE);
    CHECK_FALSE(first.loop);

    in.currentAnimId = anim::DODGE;
    in.currentAnimTime = 100.0f;
    in.currentAnimDuration = 800.0f;
    CHECK(fsm.resolve(in, caps, wl).animId == anim::DODGE);

    in.currentAnimTime = 800.0f;
    auto after = fsm.resolve(in, caps, wl);
    CHECK(fsm.getState() == CombatFSM::State::COMBAT_IDLE);
    CHECK(after.animId == anim::READY_UNARMED);
}

TEST_CASE("CombatFSM: a hit during a channel plays once and the channel returns", "[combat][cast-kit]") {
    // Tame Beast: ChannelCastDirected (124) held, the beast hits every 2 s.
    CombatFSM fsm;
    auto caps = makeCombatCaps();
    auto wl = unarmedLoadout();
    fsm.startSpellCast(0, anim::CHANNEL_CAST_DIRECTED, true, 0);
    fsm.triggerHitReaction(anim::COMBAT_WOUND);
    CHECK(fsm.getState() == CombatFSM::State::HIT_REACTION);
    auto in = combatInput();
    in.haveAnimState = true;
    in.currentAnimId = anim::COMBAT_WOUND;
    in.currentAnimTime = 100.0f;
    in.currentAnimDuration = 800.0f;
    CHECK(fsm.resolve(in, caps, wl).animId == anim::COMBAT_WOUND);
    in.currentAnimTime = 800.0f;
    auto after = fsm.resolve(in, caps, wl);
    CHECK(fsm.getState() == CombatFSM::State::SPELL_CASTING);
    CHECK(after.animId == anim::CHANNEL_CAST_DIRECTED);
    CHECK(after.loop);
}

TEST_CASE("CombatFSM: a channel ended during a hit does not come back", "[combat][cast-kit]") {
    CombatFSM fsm;
    auto caps = makeCombatCaps();
    auto wl = unarmedLoadout();
    fsm.startSpellCast(0, anim::CHANNEL_CAST_DIRECTED, true, 0);
    fsm.triggerHitReaction(anim::COMBAT_WOUND);
    fsm.cancelSpellCast();
    auto in = combatInput();
    in.haveAnimState = true;
    in.currentAnimId = anim::COMBAT_WOUND;
    in.currentAnimTime = 800.0f;
    in.currentAnimDuration = 800.0f;
    fsm.resolve(in, caps, wl);
    CHECK(fsm.getState() == CombatFSM::State::COMBAT_IDLE);
}

TEST_CASE("Parry animation follows the ready stance grip", "[combat]") {
    CHECK(parryAnimationForReady(anim::READY_UNARMED) == anim::PARRY_UNARMED);
    CHECK(parryAnimationForReady(anim::READY_1H) == anim::PARRY_1H);
    CHECK(parryAnimationForReady(anim::READY_2H) == anim::PARRY_2H);
    CHECK(parryAnimationForReady(anim::READY_2H_LOOSE) == anim::PARRY_2H_LOOSE);
    WeaponLoadout staff;
    staff.inventoryType = wowee::game::InvType::TWO_HAND;
    staff.is2HLoose = true;
    CHECK(meleeReadyAnimation(staff) == anim::READY_2H_LOOSE);
    CHECK(meleeReadyAnimation(unarmedLoadout()) == anim::READY_UNARMED);
}

TEST_CASE("CombatFSM: reset clears all state", "[combat]") {
    CombatFSM fsm;
    fsm.onEvent(AnimEvent::COMBAT_ENTER);
    fsm.setStunned(true);

    fsm.reset();
    CHECK(fsm.getState() == CombatFSM::State::INACTIVE);
    CHECK_FALSE(fsm.isStunned());
}

TEST_CASE("CombatFSM: equipped thrown weapon does not replace melee ready stance", "[combat]") {
    CombatFSM fsm;
    auto caps = makeCombatCaps();
    caps.resolvedReady1H = anim::READY_1H;
    caps.resolvedReadyThrown = anim::READY_THROWN;
    WeaponLoadout loadout;
    loadout.inventoryType = wowee::game::InvType::ONE_HAND;
    loadout.rangedType = RangedWeaponType::THROWN;

    fsm.setState(CombatFSM::State::COMBAT_IDLE);
    auto in = combatInput();
    in.rangedWeaponActive = false;
    CHECK(fsm.resolve(in, caps, loadout).animId == anim::READY_1H);

    in.rangedWeaponActive = true;
    CHECK(fsm.resolve(in, caps, loadout).animId == anim::READY_THROWN);
}

// A wand shares INVTYPE_RANGEDRIGHT with a gun, so reading the inventory type
// alone shouldered it like a rifle. It is aimed the way a directed spell is.
TEST_CASE("CombatFSM: a drawn wand takes the directed spell stance, not the rifle's",
          "[combat]") {
    CombatFSM fsm;
    auto caps = makeCombatCaps();
    caps.resolvedReadyRifle = anim::READY_RIFLE;
    caps.resolvedReadyWand  = anim::READY_SPELL_DIRECTED;
    WeaponLoadout loadout;
    loadout.rangedType = RangedWeaponType::WAND;

    fsm.setState(CombatFSM::State::COMBAT_IDLE);
    auto in = combatInput();
    in.rangedWeaponActive = true;
    const uint32_t chosen = fsm.resolve(in, caps, loadout).animId;
    CHECK(chosen == anim::READY_SPELL_DIRECTED);
    CHECK(chosen != anim::READY_RIFLE);

    // And a gun still takes the rifle's, which is the half that was right.
    loadout.rangedType = RangedWeaponType::GUN;
    fsm.setState(CombatFSM::State::COMBAT_IDLE);
    CHECK(fsm.resolve(in, caps, loadout).animId == anim::READY_RIFLE);
}

TEST_CASE("CombatFSM: a kit animation plays once over the stun its spell applies", "[combat]") {
    // Knockdown (5164/16790) impact kit 348: Knockdown (121) on the target,
    // whose stun arrives right after; the original shows the fall first.
    CombatFSM fsm;
    auto caps = makeCombatCaps();
    auto wl = unarmedLoadout();
    fsm.onEvent(AnimEvent::COMBAT_ENTER);
    fsm.triggerKitAnimation(anim::KNOCKDOWN);
    fsm.setStunned(true);
    auto in = combatInput();
    in.haveAnimState = true;
    in.currentAnimId = anim::STAND;
    auto out = fsm.resolve(in, caps, wl);
    REQUIRE(out.valid);
    CHECK(out.animId == anim::KNOCKDOWN);
    CHECK_FALSE(out.loop);
    // Shown and finished: the stun takes over.
    in.currentAnimId = anim::KNOCKDOWN;
    in.currentAnimTime = 0.5f;
    in.currentAnimDuration = 2.0f;
    CHECK(fsm.resolve(in, caps, wl).animId == anim::KNOCKDOWN);
    in.currentAnimTime = 2.0f;
    out = fsm.resolve(in, caps, wl);
    CHECK(out.animId == anim::STUN);
}

TEST_CASE("CombatFSM: back-to-back precast lets previous CAST finalize complete before looping",
          "[combat][spell_cast_anim]") {
    CombatFSM fsm;
    auto caps = makeCombatCaps();
    auto wl = unarmedLoadout();

    // First spell finishes: plays SpellCastOmni (54) in SPELL_FINALIZE.
    fsm.startSpellCast(0, anim::SPELL_CAST_OMNI, false, anim::SPELL_CAST_OMNI);
    fsm.stopSpellCast();
    REQUIRE(fsm.getState() == CombatFSM::State::SPELL_FINALIZE);

    auto in = combatInput();
    in.haveAnimState = true;
    in.currentAnimId = anim::SPELL_CAST_OMNI;
    in.currentAnimTime = 0.2f;
    in.currentAnimDuration = 1.0f;
    auto out = fsm.resolve(in, caps, wl);
    CHECK(out.animId == anim::SPELL_CAST_OMNI);
    CHECK_FALSE(out.loop);

    // 200ms into SpellCastOmni, next spell's PRECAST (ReadySpellDirected 51) arrives:
    fsm.startSpellCast(0, anim::READY_SPELL_DIRECTED, true, 0);
    REQUIRE(fsm.getState() == CombatFSM::State::SPELL_PRECAST);

    // SpellCastOmni continues until its 1.0s duration completes:
    in.currentAnimTime = 0.6f;
    out = fsm.resolve(in, caps, wl);
    CHECK(out.animId == anim::SPELL_CAST_OMNI);
    CHECK_FALSE(out.loop);

    // Once SpellCastOmni finishes, transitions seamlessly into ReadySpellDirected (51) loop:
    in.currentAnimTime = 1.0f;
    out = fsm.resolve(in, caps, wl);
    CHECK(fsm.getState() == CombatFSM::State::SPELL_CASTING);
    CHECK(out.animId == anim::READY_SPELL_DIRECTED);
    CHECK(out.loop);
}

TEST_CASE("CombatFSM: isSpellCastActive stays true through precast, mid-cast hit flinch, and release finalize",
          "[combat][spell_cast_anim]") {
    CombatFSM fsm;
    auto caps = makeCombatCaps();
    auto wl = unarmedLoadout();
    auto in = combatInput();
    in.haveAnimState = true;

    CHECK_FALSE(fsm.isSpellCastActive());

    // 1. Start timed directed cast (ReadySpellDirected 51).
    fsm.startSpellCast(0, anim::READY_SPELL_DIRECTED, true, 0);
    CHECK(fsm.isSpellCastActive());
    in.currentAnimId = anim::READY_SPELL_DIRECTED;
    in.currentAnimTime = 0.2f;
    in.currentAnimDuration = 1.0f;
    CHECK(fsm.resolve(in, caps, wl).animId == anim::READY_SPELL_DIRECTED);

    // 2. Melee hit arrives mid-cast -> CombatWound (9) plays while isSpellCastActive()
    // stays true so auto-unsheathe does not redraw weapons mid-cast.
    fsm.triggerHitReaction(anim::COMBAT_WOUND);
    CHECK(fsm.getState() == CombatFSM::State::HIT_REACTION);
    CHECK(fsm.isSpellCastActive());
    in.currentAnimId = anim::COMBAT_WOUND;
    in.currentAnimTime = 0.2f;
    in.currentAnimDuration = 0.8f;
    CHECK(fsm.resolve(in, caps, wl).animId == anim::COMBAT_WOUND);

    // 3. Hit reaction finishes -> returns to ReadySpellDirected (51).
    in.currentAnimTime = 0.8f;
    auto back = fsm.resolve(in, caps, wl);
    CHECK(fsm.getState() == CombatFSM::State::SPELL_CASTING);
    CHECK(fsm.isSpellCastActive());
    CHECK(back.animId == anim::READY_SPELL_DIRECTED);
    CHECK(back.loop);

    // 4. Cast completes (SMSG_SPELL_GO) -> SpellCastDirected (53) one-shot in SPELL_FINALIZE.
    // isSpellCastActive() must stay true so auto-unsheathe does not cut off 53 with SHEATHE (89).
    fsm.startSpellCast(0, anim::SPELL_CAST_DIRECTED, false, anim::SPELL_CAST_DIRECTED);
    fsm.stopSpellCast();
    CHECK(fsm.getState() == CombatFSM::State::SPELL_FINALIZE);
    CHECK(fsm.isSpellCastActive());
    in.currentAnimId = anim::SPELL_CAST_DIRECTED;
    in.currentAnimTime = 0.3f;
    in.currentAnimDuration = 1.0f;
    CHECK(fsm.resolve(in, caps, wl).animId == anim::SPELL_CAST_DIRECTED);

    // 5. Release animation finishes -> returns to COMBAT_IDLE and isSpellCastActive() clears.
    in.currentAnimTime = 1.0f;
    auto done = fsm.resolve(in, caps, wl);
    CHECK(fsm.getState() == CombatFSM::State::COMBAT_IDLE);
    CHECK_FALSE(fsm.isSpellCastActive());
    CHECK(done.animId == anim::READY_UNARMED);

    // 6. Out-of-cast hit reaction does NOT report isSpellCastActive().
    fsm.triggerHitReaction(anim::COMBAT_WOUND);
    CHECK(fsm.getState() == CombatFSM::State::HIT_REACTION);
    CHECK_FALSE(fsm.isSpellCastActive());
}

TEST_CASE("Stunned units suppress hit reaction animations while holding Stun pose",
          "[combat][stun]") {
    constexpr uint32_t kUnitFlagStunned = 0x00040000u;
    CHECK_FALSE(unitSuppressesHitReactionWhileStunned(0u));
    CHECK(unitSuppressesHitReactionWhileStunned(kUnitFlagStunned));
    CHECK(unitSuppressesHitReactionWhileStunned(kUnitFlagStunned | 0x00080000u));

    // Local CombatFSM also ignores hit reactions while stunned.
    CombatFSM fsm;
    auto caps = makeCombatCaps();
    auto wl = unarmedLoadout();
    fsm.setStunned(true);
    fsm.triggerHitReaction(anim::COMBAT_WOUND);
    CHECK(fsm.isStunned());
    auto out = fsm.resolve(combatInput(), caps, wl);
    REQUIRE(out.valid);
    CHECK(out.animId == anim::STUN);
    CHECK(out.loop);
}

TEST_CASE("Melee swing queue: classifies melee swing animations and queues overlapping dual-wield swings",
          "[combat][dualwield]") {
    CHECK(isMeleeSwingAnimation(anim::ATTACK_1H));
    CHECK(isMeleeSwingAnimation(anim::ATTACK_1H_PIERCE));
    CHECK(isMeleeSwingAnimation(anim::ATTACK_OFF));
    CHECK(isMeleeSwingAnimation(anim::ATTACK_OFF_PIERCE));
    CHECK(isMeleeSwingAnimation(anim::ATTACK_UNARMED));
    CHECK(isMeleeSwingAnimation(anim::ATTACK_UNARMED_OFF));
    CHECK(isMeleeSwingAnimation(anim::ATTACK_2H));
    CHECK(isMeleeSwingAnimation(anim::FLY_ATTACK_OFF));

    CHECK_FALSE(isMeleeSwingAnimation(anim::READY_1H));
    CHECK_FALSE(isMeleeSwingAnimation(anim::STAND));
    CHECK_FALSE(isMeleeSwingAnimation(anim::RUN));
    CHECK_FALSE(isMeleeSwingAnimation(anim::COMBAT_WOUND));
    CHECK_FALSE(isMeleeSwingAnimation(anim::DEATH));

    // Mid-swing MH -> incoming OH queues instead of interrupting.
    CHECK(shouldQueueIncomingMeleeSwing(true, anim::ATTACK_1H, 200.0f, 800.0f));
    // Mid-swing OH -> incoming MH (or Thrash) queues instead of interrupting.
    CHECK(shouldQueueIncomingMeleeSwing(true, anim::ATTACK_OFF, 185.0f, 750.0f));
    // Completed swing starts immediately.
    CHECK_FALSE(shouldQueueIncomingMeleeSwing(true, anim::ATTACK_OFF, 750.0f, 750.0f));
    // Ready stance, looping locomotion, or hit reaction starts immediately.
    CHECK_FALSE(shouldQueueIncomingMeleeSwing(false, anim::READY_1H, 100.0f, 1000.0f));
    CHECK_FALSE(shouldQueueIncomingMeleeSwing(true, anim::COMBAT_WOUND, 100.0f, 500.0f));

    // Effective duration cap fits overlapping dual-wield MH+OH pairs + 150ms
    // Ready blend inside one attack cycle without cumulative phase drift, while
    // un-overlapped swings keep full authored / attackTimeMs speed.
    CHECK(dualWieldSwingMaxMs(1500.0f) == Catch::Approx(570.0f));
    CHECK(dualWieldSwingMaxMs(1000.0f) == Catch::Approx(380.0f));
    CHECK(dualWieldSwingMaxMs(500.0f) == Catch::Approx(190.0f));
    CHECK(dualWieldSwingMaxMs(300.0f) == Catch::Approx(135.0f));
    CHECK(dualWieldSwingMaxMs(0.0f) == Catch::Approx(0.0f));

    // Brutallus: OFF arriving 200ms into 1800ms AttackUnarmed (running at 1000ms cap,
    // currentTimeMs = 360ms, p = 0.20) accelerates remaining 80% into 180ms wall time
    // (225ms equivalent full cap) so MAIN finishes at 380ms total wall time.
    CHECK(activeSwingMaxMsOnQueue(360.0f, 1800.0f, 1000.0f, 380.0f) == Catch::Approx(225.0f));
    // VanCleef parry-hasted: OFF arriving 950ms into 1000ms Attack1H (50ms remaining)
    // does not slow down the final 50ms tail.
    CHECK(activeSwingMaxMsOnQueue(950.0f, 1000.0f, 1500.0f, 570.0f) == Catch::Approx(1500.0f));
}

TEST_CASE("Brutallus (24882) 1000ms and VanCleef (639) 1500ms dual-wield cadences complete every pair without drift",
          "[combat][dualwield][brutallus]") {
    constexpr float kDtMs = 1000.0f / 30.0f;

    auto runSimulation = [&](float attackTimeMs, float mhAuthoredMs, float ohAuthoredMs, float ohOffsetMs) {
        bool playingOneShot = false;
        uint32_t currentAnim = anim::READY_UNARMED;
        float currentTimeMs = 0.0f;
        float currentDurationMs = 2000.0f;
        float oneShotMaxMs = 0.0f;
        QueuedMeleeSwing pending{};

        int startedMain = 0;
        int startedOff = 0;
        int readyTransitions = 0;
        uint32_t lastStartedSwing = 0;
        int consecutiveOffSwings = 0;

        auto startSwing = [&](uint32_t id, float maxMs) {
            if (id == anim::ATTACK_UNARMED) {
                ++startedMain;
            } else {
                if (lastStartedSwing == anim::ATTACK_UNARMED_OFF) ++consecutiveOffSwings;
                ++startedOff;
            }
            lastStartedSwing = id;
            playingOneShot = true;
            currentAnim = id;
            currentTimeMs = 0.0f;
            currentDurationMs = (id == anim::ATTACK_UNARMED) ? mhAuthoredMs : ohAuthoredMs;
            oneShotMaxMs = maxMs;
        };

        auto onPacket = [&](uint32_t id) {
            if (shouldQueueIncomingMeleeSwing(playingOneShot, currentAnim, currentTimeMs, currentDurationMs)) {
                const float cap = dualWieldSwingMaxMs(attackTimeMs);
                pending = {id, anim::READY_UNARMED, cap};
                oneShotMaxMs = activeSwingMaxMsOnQueue(currentTimeMs, currentDurationMs, oneShotMaxMs, cap);
                return;
            }
            pending.clear();
            startSwing(id, attackTimeMs);
        };

        int nextMainCycle = 0;
        int nextOffCycle = 0;
        const float totalSimMs = 25.0f * attackTimeMs + 1000.0f;
        for (float wallMs = 0.0f; wallMs < totalSimMs; wallMs += kDtMs) {
            if (nextMainCycle < 25 && wallMs >= static_cast<float>(nextMainCycle) * attackTimeMs) {
                onPacket(anim::ATTACK_UNARMED);
                ++nextMainCycle;
            }
            if (nextOffCycle < 25 && wallMs >= static_cast<float>(nextOffCycle) * attackTimeMs + ohOffsetMs) {
                onPacket(anim::ATTACK_UNARMED_OFF);
                ++nextOffCycle;
            }

            if (playingOneShot) {
                const float rate = (oneShotMaxMs > 0.0f && currentDurationMs > oneShotMaxMs)
                    ? (currentDurationMs / oneShotMaxMs)
                    : 1.0f;
                currentTimeMs += kDtMs * rate;
                if (currentTimeMs >= currentDurationMs) {
                    if (pending.valid()) {
                        const auto next = pending;
                        pending.clear();
                        startSwing(next.animId, next.maxDurationMs);
                    } else {
                        playingOneShot = false;
                        currentAnim = anim::READY_UNARMED;
                        currentTimeMs = 0.0f;
                        currentDurationMs = 2000.0f;
                        oneShotMaxMs = 0.0f;
                        ++readyTransitions;
                    }
                }
            }
        }

        CHECK(startedMain == 25);
        CHECK(startedOff == 25);
        CHECK(consecutiveOffSwings == 0);
        CHECK(readyTransitions == 25);
    };

    // 1. Brutallus (1000ms MH/OH, 1800ms/1834ms authored, 200ms offset)
    runSimulation(1000.0f, 1800.0f, 1834.0f, 200.0f);
    // 2. Edwin VanCleef normal dual-wield (1500ms MH/OH, 1000ms authored, 200ms offset)
    runSimulation(1500.0f, 1000.0f, 1000.0f, 200.0f);
    // 3. Edwin VanCleef parry-hasted alternating hands (1500ms MH/OH, 1000ms authored, 880ms offset)
    runSimulation(1500.0f, 1000.0f, 1000.0f, 880.0f);
}

TEST_CASE("Hit-reaction flinch is suppressed during active melee swings on both remote units and local CombatFSM",
          "[combat][dualwield][hit_reaction]") {
    CHECK(unitSuppressesHitReactionDuringSwing(true, anim::ATTACK_1H));
    CHECK(unitSuppressesHitReactionDuringSwing(true, anim::ATTACK_OFF));
    CHECK(unitSuppressesHitReactionDuringSwing(true, anim::ATTACK_UNARMED));
    CHECK(unitSuppressesHitReactionDuringSwing(true, anim::ATTACK_UNARMED_OFF));
    CHECK_FALSE(unitSuppressesHitReactionDuringSwing(false, anim::ATTACK_1H));
    CHECK_FALSE(unitSuppressesHitReactionDuringSwing(true, anim::COMBAT_WOUND));
    CHECK_FALSE(unitSuppressesHitReactionDuringSwing(false, anim::READY_1H));

    // Local CombatFSM keeps MELEE_SWING undisturbed when hit mid-swing.
    CombatFSM fsm;
    auto caps = makeCombatCaps();
    caps.resolvedReady1H = anim::READY_1H;
    caps.resolvedMelee1H = anim::ATTACK_1H;
    caps.resolvedMeleeOffHand = anim::ATTACK_OFF;

    WeaponLoadout wl;
    wl.inventoryType = wowee::game::InvType::ONE_HAND;
    wl.hasOffHand = true;

    auto in = combatInput();
    in.meleeSwingTimer = 0.4f;
    in.meleeSwingOffHand = true;
    in.meleeSwingSerial = 1;
    REQUIRE(fsm.resolve(in, caps, wl).animId == anim::ATTACK_OFF);
    REQUIRE(fsm.getState() == CombatFSM::State::MELEE_SWING);

    fsm.triggerHitReaction(anim::COMBAT_WOUND);
    CHECK(fsm.getState() == CombatFSM::State::MELEE_SWING);
    CHECK(fsm.resolve(in, caps, wl).animId == anim::ATTACK_OFF);
}

TEST_CASE("CombatFSM: dual-wield MH+OH, OH+MH, and Thrash sequences honor HITINFO_LEFTSWING and serial transitions",
          "[combat][dualwield]") {
    CombatFSM fsm;
    auto caps = makeCombatCaps();
    caps.resolvedReady1H = anim::READY_1H;
    caps.resolvedMelee1H = anim::ATTACK_1H;
    caps.resolvedMeleeOffHand = anim::ATTACK_OFF;
    caps.resolvedMeleeOffHandPierce = anim::ATTACK_OFF_PIERCE;

    WeaponLoadout wl;
    wl.inventoryType = wowee::game::InvType::ONE_HAND;
    wl.hasOffHand = true;

    auto in = combatInput();
    // 1. Main-hand swing starts (serial 1).
    in.meleeSwingTimer = 0.7f;
    in.meleeSwingOffHand = false;
    in.meleeSwingSerial = 1;
    auto mh = fsm.resolve(in, caps, wl);
    REQUIRE(mh.valid);
    CHECK(fsm.getState() == CombatFSM::State::MELEE_SWING);
    CHECK(mh.animId == anim::ATTACK_1H);
    CHECK_FALSE(mh.loop);

    // 2. Queued off-hand swing pops immediately on completion without dipping to COMBAT_IDLE (serial 2).
    in.meleeSwingTimer = 0.65f;
    in.meleeSwingOffHand = true;
    in.meleeSwingSerial = 2;
    auto oh = fsm.resolve(in, caps, wl);
    REQUIRE(oh.valid);
    CHECK(fsm.getState() == CombatFSM::State::MELEE_SWING);
    CHECK(oh.animId == anim::ATTACK_OFF);
    CHECK_FALSE(oh.loop);

    // 3. Fast OH weapon swings twice in a row (OH -> OH, serial 3) — must stay AttackOff.
    in.meleeSwingTimer = 0.65f;
    in.meleeSwingOffHand = true;
    in.meleeSwingSerial = 3;
    auto oh2 = fsm.resolve(in, caps, wl);
    REQUIRE(oh2.valid);
    CHECK(oh2.animId == anim::ATTACK_OFF);

    // 4. OH -> MH transition (e.g. Thrash proc after off-hand, serial 4) — switches cleanly to Attack1H.
    in.meleeSwingTimer = 0.7f;
    in.meleeSwingOffHand = false;
    in.meleeSwingSerial = 4;
    auto mh2 = fsm.resolve(in, caps, wl);
    REQUIRE(mh2.valid);
    CHECK(mh2.animId == anim::ATTACK_1H);

    // 5. Off-hand dagger uses AttackOffPierce when offHand=true.
    wl.offHandIsDagger = true;
    in.meleeSwingTimer = 0.6f;
    in.meleeSwingOffHand = true;
    in.meleeSwingSerial = 5;
    auto daggerOh = fsm.resolve(in, caps, wl);
    REQUIRE(daggerOh.valid);
    CHECK(daggerOh.animId == anim::ATTACK_OFF_PIERCE);
}
