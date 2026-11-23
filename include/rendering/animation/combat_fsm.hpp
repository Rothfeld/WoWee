#pragma once

#include "rendering/animation/anim_capability_set.hpp"
#include "rendering/animation/anim_event.hpp"
#include "rendering/animation/weapon_type.hpp"
#include <cstdint>
#include <optional>

namespace wowee {
namespace rendering {

// ============================================================================
// CombatFSM
//
// Pure logic state machine for combat animation. No renderer dependency.
// States: INACTIVE · COMBAT_IDLE · MELEE_SWING · RANGED_SHOOT · RANGED_LOAD ·
//         SPELL_PRECAST · SPELL_CASTING · SPELL_FINALIZE · HIT_REACTION ·
//         STUNNED · CHARGE · UNSHEATHE · SHEATHE
//
// Stun overrides all combat states. Spell state cleared on interrupts.
// offHandTurn_ alternation managed internally.
// ============================================================================
/// Ready stance for a melee grip: unarmed, one-hand, two-hand, or polearm and
/// staff (Ready2HL). Same loadout rule as the combat idle stance.
uint32_t meleeReadyAnimation(const WeaponLoadout& loadout);

/// AnimationData pairs each melee ready stance with a parry of the same grip:
/// ReadyUnarmed/1H/2H/2HL (25-28) -> ParryUnarmed/1H/2H/2HL (20-23).
uint32_t parryAnimationForReady(uint32_t readyAnimId);

class CombatFSM {
public:
    enum class State : uint8_t {
        INACTIVE,
        COMBAT_IDLE,
        MELEE_SWING,
        RANGED_SHOOT,
        RANGED_LOAD,
        RANGED_HOLD,
        SPELL_PRECAST,
        SPELL_CASTING,
        SPELL_FINALIZE,
        HIT_REACTION,
        STUNNED,
        CHARGE,
        UNSHEATHE,
        SHEATHE,
    };

    struct Input {
        bool inCombat = false;
        bool grounded = true;
        bool jumping = false;
        bool swimming = false;
        bool moving = false;
        bool sprinting = false;
        bool lowHealth = false;
        bool rangedWeaponActive = false;
        bool rangedAiming = false;
        float meleeSwingTimer = 0.0f;   // >0 = melee active
        std::optional<bool> meleeSwingOffHand; // Explicit hand from HITINFO_LEFTSWING when present
        uint32_t meleeSwingSerial = 0;  // Increments on each newly started swing (including queued follow-ups)
        float rangedShootTimer = 0.0f;  // >0 = ranged active
        uint32_t specialAttackAnimId = 0;
        uint32_t rangedAnimId = 0;
        // Animation state query for one-shot completion detection
        uint32_t currentAnimId = 0;
        float currentAnimTime = 0.0f;
        float currentAnimDuration = 0.0f;
        bool haveAnimState = false;
        // Whether model has specific one-shot animations
        bool hasUnsheathe = false;
        bool hasSheathe = false;
    };

    void onEvent(AnimEvent event);

    /// Evaluate current state against input and capabilities.
    AnimOutput resolve(const Input& in, const AnimCapabilitySet& caps,
                       const WeaponLoadout& loadout);

    [[nodiscard]] State getState() const { return state_; }
    void setState(State s) { state_ = s; }

    [[nodiscard]] bool isStunned() const { return state_ == State::STUNNED; }
    [[nodiscard]] bool isActive() const { return state_ != State::INACTIVE; }
    /// True while a spell precast, cast/channel hold, release one-shot, or a
    /// mid-cast hit reaction returning to a cast is active.
    [[nodiscard]] bool isSpellCastActive() const {
        return state_ == State::SPELL_PRECAST ||
               state_ == State::SPELL_CASTING ||
               state_ == State::SPELL_FINALIZE ||
               (state_ == State::HIT_REACTION && resumeSpellAfterHit_);
    }
    void reset();

    // ── Spell cast management ───────────────────────────────────────────
    void startSpellCast(uint32_t precast, uint32_t cast, bool castLoop, uint32_t finalize);
    void stopSpellCast();
    /// Leave the spell states at once, with no release animation.
    void cancelSpellCast();
    void clearSpellState();

    // ── Hit/stun management ─────────────────────────────────────────────
    void triggerHitReaction(uint32_t animId);
    /// A spell kit's animation on this unit (impact kit 348: Knockdown 121),
    /// played once over every combat state, stun included; 0 clears it.
    void triggerKitAnimation(uint32_t animId);
    void setStunned(bool stunned);
    void setCharging(bool charging);

    static constexpr uint8_t PRIORITY = 50;

private:
    State state_ = State::INACTIVE;

    // Spell cast sequence
    uint32_t spellPrecastAnimId_ = 0;
    uint32_t spellCastAnimId_ = 0;
    uint32_t spellFinalizeAnimId_ = 0;
    bool spellCastLoop_ = false;
    bool spellPrecastAnimSeen_ = false;
    uint8_t spellPrecastFrames_ = 0;
    bool spellFinalizeAnimSeen_ = false;
    uint8_t spellFinalizeFrames_ = 0;

    // Hit reaction. Completion counts only once the renderer has shown the
    // reaction: on the frame it is requested the previous pose is current.
    uint32_t hitReactionAnimId_ = 0;
    bool hitReactionAnimSeen_ = false;
    /// A hit taken while a cast or channel is held: the held animation comes
    /// back once the reaction has played, as a remote unit's cast hold does.
    bool resumeSpellAfterHit_ = false;
    uint8_t hitReactionFrames_ = 0;

    // Spell kit one-shot; same completion rule as the hit reaction.
    uint32_t kitAnimId_ = 0;
    bool kitAnimSeen_ = false;
    uint8_t kitAnimFrames_ = 0;

    // Stun
    bool stunned_ = false;

    // Charge
    bool charging_ = false;

    // Weapons already drawn this combat: moving and stopping must not replay the unsheathe.
    bool weaponsDrawn_ = false;

    // Off-hand alternation for dual wielding
    /// Which hand the next swing uses, and which this one chose.
    ///
    /// The turn is taken when a swing begins. Taken where the animation is
    /// resolved instead, it was taken again on every frame the swing lasted -
    /// so a dual-wielding character alternated main-hand and off-hand
    /// animations sixty times a second and stood there shuddering. With one
    /// weapon there was no second animation to alternate with, which is why it
    /// only ever showed with two.
    bool offHandTurn_ = false;
    bool offHandThisSwing_ = false;
    uint32_t lastMeleeSwingSerial_ = 0;

    // Ranged reload (LoadBow/LoadRifle) one-shot tracking
    bool rangedLoadAnimSeen_ = false;
    uint8_t rangedLoadFrames_ = 0;

    /// Internal: update state transitions based on input.
    void updateTransitions(const Input& in, const AnimCapabilitySet& caps,
                           const WeaponLoadout& loadout);

    /// Detect if a one-shot animation has completed.
    [[nodiscard]] bool oneShotComplete(const Input& in, uint32_t expectedAnimId) const;
};

} // namespace rendering
} // namespace wowee
