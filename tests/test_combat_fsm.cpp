// CombatFSM unit tests
#include <catch_amalgamated.hpp>
#include "rendering/animation/combat_fsm.hpp"
#include "rendering/animation/animation_ids.hpp"
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
