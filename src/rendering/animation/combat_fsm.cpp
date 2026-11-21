#include "rendering/animation/combat_fsm.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "game/inventory.hpp"
#include "core/logger.hpp"
#include <cassert>

namespace wowee {
namespace rendering {

uint32_t meleeReadyAnimation(const WeaponLoadout& loadout) {
    if (loadout.inventoryType == game::InvType::NON_EQUIP) return anim::READY_UNARMED;
    if (loadout.is2HLoose) return anim::READY_2H_LOOSE;
    if (loadout.inventoryType == game::InvType::TWO_HAND) return anim::READY_2H;
    return anim::READY_1H;
}

uint32_t parryAnimationForReady(uint32_t readyAnimId) {
    switch (readyAnimId) {
        case anim::READY_UNARMED:  return anim::PARRY_UNARMED;
        case anim::READY_1H:       return anim::PARRY_1H;
        case anim::READY_2H:       return anim::PARRY_2H;
        case anim::READY_2H_LOOSE: return anim::PARRY_2H_LOOSE;
    }
    assert(!"parryAnimationForReady: not a melee ready stance");
    return anim::PARRY_UNARMED;
}

// ── One-shot completion helper ───────────────────────────────────────────────

bool CombatFSM::oneShotComplete(const Input& in, uint32_t expectedAnimId) const {
    if (!in.haveAnimState) return false;
    // Renderer auto-returns one-shots to STAND; detect that OR normal completion
    return in.currentAnimId != expectedAnimId ||
           (in.currentAnimDuration > 0.1f && in.currentAnimTime >= in.currentAnimDuration - 0.05f);
}

// ── Event handling ───────────────────────────────────────────────────────────

void CombatFSM::onEvent(AnimEvent event) {
    switch (event) {
        case AnimEvent::COMBAT_ENTER:
            if (state_ == State::INACTIVE)
                state_ = weaponsDrawn_ ? State::COMBAT_IDLE : State::UNSHEATHE;
            break;
        case AnimEvent::COMBAT_EXIT:
            if (state_ == State::COMBAT_IDLE)
                state_ = State::SHEATHE;
            break;
        case AnimEvent::STUN_ENTER:
            clearSpellState();
            hitReactionAnimId_ = 0;
            stunned_ = true;
            state_ = State::STUNNED;
            break;
        case AnimEvent::STUN_EXIT:
            stunned_ = false;
            if (state_ == State::STUNNED)
                state_ = State::INACTIVE;
            break;
        case AnimEvent::HIT_REACT:
            // Handled by triggerHitReaction() with animId
            break;
        case AnimEvent::CHARGE_START:
            charging_ = true;
            clearSpellState();
            state_ = State::CHARGE;
            break;
        case AnimEvent::CHARGE_END:
            charging_ = false;
            if (state_ == State::CHARGE)
                state_ = State::INACTIVE;
            break;
        case AnimEvent::SWIM_ENTER:
            clearSpellState();
            hitReactionAnimId_ = 0;
            stunned_ = false;
            state_ = State::INACTIVE;
            break;
        default:
            break;
    }
}

// ── Spell cast management ────────────────────────────────────────────────────

void CombatFSM::startSpellCast(uint32_t precast, uint32_t cast, bool castLoop, uint32_t finalize) {
    resumeSpellAfterHit_ = false;
    // A new looping precast/channel arriving while the previous cast's release
    // one-shot is still playing lets that release animation complete first, then
    // transitions into SPELL_CASTING (matching TBC 2.4.3 back-to-back cast parity).
    if (state_ == State::SPELL_FINALIZE && precast == 0 && castLoop && cast != 0) {
        const uint32_t activeRelease = spellFinalizeAnimId_ ? spellFinalizeAnimId_
            : (spellCastAnimId_ ? spellCastAnimId_ : anim::SPELL);
        spellPrecastAnimId_ = activeRelease;
        spellPrecastAnimSeen_ = spellFinalizeAnimSeen_;
        spellPrecastFrames_ = spellFinalizeFrames_;
        spellCastAnimId_ = cast;
        spellCastLoop_ = true;
        spellFinalizeAnimId_ = finalize;
        spellFinalizeAnimSeen_ = false;
        spellFinalizeFrames_ = 0;
        state_ = State::SPELL_PRECAST;
        return;
    }
    // A back-to-back CAST one-shot with the exact same animation ID lets the
    // running release animation complete instead of restarting from frame 0.
    if (state_ == State::SPELL_FINALIZE && precast == 0 && !castLoop && finalize != 0) {
        const uint32_t activeRelease = spellFinalizeAnimId_ ? spellFinalizeAnimId_
            : (spellCastAnimId_ ? spellCastAnimId_ : anim::SPELL);
        if (finalize == activeRelease) {
            spellCastAnimId_ = cast;
            spellCastLoop_ = false;
            spellFinalizeAnimId_ = finalize;
            return;
        }
    }
    spellPrecastAnimId_ = precast;
    spellCastAnimId_ = cast;
    spellCastLoop_ = castLoop;
    spellFinalizeAnimId_ = finalize;
    spellPrecastAnimSeen_ = false;
    spellPrecastFrames_ = 0;
    spellFinalizeAnimSeen_ = false;
    spellFinalizeFrames_ = 0;
    state_ = (precast != 0) ? State::SPELL_PRECAST : State::SPELL_CASTING;
}

void CombatFSM::stopSpellCast() {
    if (resumeSpellAfterHit_) {
        // Ended during a hit reaction: nothing to return to.
        resumeSpellAfterHit_ = false;
        clearSpellState();
        return;
    }
    if (state_ != State::SPELL_PRECAST && state_ != State::SPELL_CASTING) return;
    spellFinalizeAnimSeen_ = false;
    spellFinalizeFrames_ = 0;
    state_ = State::SPELL_FINALIZE;
}

void CombatFSM::cancelSpellCast() {
    if (resumeSpellAfterHit_) {
        resumeSpellAfterHit_ = false;
        clearSpellState();
        return;
    }
    if (state_ != State::SPELL_PRECAST && state_ != State::SPELL_CASTING &&
        state_ != State::SPELL_FINALIZE) return;
    clearSpellState();
    state_ = State::INACTIVE;
}

void CombatFSM::clearSpellState() {
    resumeSpellAfterHit_ = false;
    spellPrecastAnimId_ = 0;
    spellCastAnimId_ = 0;
    spellCastLoop_ = false;
    spellFinalizeAnimId_ = 0;
    spellPrecastAnimSeen_ = false;
    spellPrecastFrames_ = 0;
    spellFinalizeAnimSeen_ = false;
    spellFinalizeFrames_ = 0;
}

// ── Hit/stun ─────────────────────────────────────────────────────────────────

void CombatFSM::triggerHitReaction(uint32_t animId) {
    // Don't interrupt swim/jump/stun states
    if (state_ == State::STUNNED) return;
    // Zero clears a pending reaction (AnimationController::reset).
    if (animId == 0) {
        hitReactionAnimId_ = 0;
        resumeSpellAfterHit_ = false;
        if (state_ == State::HIT_REACTION) state_ = State::INACTIVE;
        return;
    }
    // A hit does not end a cast or a channel: the reaction plays once and
    // the held animation returns (Tame Beast's ChannelCastDirected while the
    // beast fights back every two seconds). Remote units already return to
    // their cast hold after a reaction (remoteResumeAnimation). A finishing
    // one-shot is simply cut short.
    if (state_ == State::SPELL_PRECAST || state_ == State::SPELL_CASTING) {
        resumeSpellAfterHit_ = true;
    } else if (state_ == State::SPELL_FINALIZE) {
        clearSpellState();
    }
    hitReactionAnimId_ = animId;
    hitReactionAnimSeen_ = false;
    hitReactionFrames_ = 0;
    state_ = State::HIT_REACTION;
}

void CombatFSM::triggerKitAnimation(uint32_t animId) {
    kitAnimId_ = animId;
    kitAnimSeen_ = false;
    kitAnimFrames_ = 0;
}

void CombatFSM::setStunned(bool stunned) {
    stunned_ = stunned;
    if (stunned) {
        if (state_ == State::SPELL_PRECAST || state_ == State::SPELL_CASTING || state_ == State::SPELL_FINALIZE) {
            clearSpellState();
        }
        hitReactionAnimId_ = 0;
        state_ = State::STUNNED;
    } else {
        if (state_ == State::STUNNED)
            state_ = State::INACTIVE;
    }
}

void CombatFSM::setCharging(bool charging) {
    charging_ = charging;
    if (charging) {
        clearSpellState();
        hitReactionAnimId_ = 0;
        state_ = State::CHARGE;
    } else if (state_ == State::CHARGE) {
        state_ = State::INACTIVE;
    }
}

// ── State transitions ────────────────────────────────────────────────────────

void CombatFSM::updateTransitions(const Input& in) {
    // Stun override: can't act while stunned
    if (stunned_ && state_ != State::STUNNED) {
        state_ = State::STUNNED;
        return;
    }

    // Force melee/ranged overrides
    if (in.meleeSwingTimer > 0.0f && !stunned_ && in.grounded && !in.swimming) {
        if (state_ != State::MELEE_SWING) {
            clearSpellState();
            hitReactionAnimId_ = 0;
            // The hand is chosen here, once, because this is where a swing
            // begins. Choosing it where the animation is resolved chose it
            // again on every frame the swing lasted.
            offHandThisSwing_ = offHandTurn_;
            offHandTurn_ = !offHandTurn_;
        }
        state_ = State::MELEE_SWING;
        return;
    }
    if (in.rangedShootTimer > 0.0f && !stunned_ && in.meleeSwingTimer <= 0.0f && in.grounded && !in.swimming) {
        if (state_ != State::RANGED_SHOOT) {
            clearSpellState();
            hitReactionAnimId_ = 0;
        }
        state_ = State::RANGED_SHOOT;
        return;
    }
    if (charging_ && !stunned_) {
        if (state_ != State::CHARGE) {
            clearSpellState();
            hitReactionAnimId_ = 0;
        }
        state_ = State::CHARGE;
        return;
    }

    switch (state_) {
        case State::INACTIVE:
            if (!in.inCombat) {
                weaponsDrawn_ = false;
            } else if (in.grounded && !in.swimming && !in.moving) {
                state_ = (in.hasUnsheathe && !weaponsDrawn_) ? State::UNSHEATHE : State::COMBAT_IDLE;
            }
            break;

        case State::COMBAT_IDLE:
            weaponsDrawn_ = true;
            if (in.swimming || in.jumping || !in.grounded || in.moving) {
                state_ = State::INACTIVE;
            } else if (!in.inCombat) {
                state_ = in.hasSheathe ? State::SHEATHE : State::INACTIVE;
            }
            break;

        case State::MELEE_SWING:
            if (in.meleeSwingTimer <= 0.0f) {
                if (in.swimming) {
                    state_ = State::INACTIVE;
                } else if (in.inCombat && in.grounded) {
                    state_ = State::COMBAT_IDLE;
                } else {
                    state_ = State::INACTIVE;
                }
            }
            break;

        case State::RANGED_SHOOT:
            if (in.rangedShootTimer <= 0.0f) {
                if (in.swimming) {
                    state_ = State::INACTIVE;
                } else if (in.inCombat && in.grounded) {
                    state_ = State::RANGED_LOAD;
                } else {
                    state_ = State::INACTIVE;
                }
            }
            break;

        case State::RANGED_LOAD:
            if (in.swimming || in.jumping || !in.grounded || in.moving) {
                state_ = State::INACTIVE;
            } else if (in.inCombat) {
                state_ = State::COMBAT_IDLE;
            } else {
                state_ = State::INACTIVE;
            }
            break;

        case State::SPELL_PRECAST:
            if (in.swimming || (in.jumping && !in.grounded) || (!in.grounded && !in.jumping)) {
                clearSpellState();
                state_ = State::INACTIVE;
            } else if (in.haveAnimState) {
                uint32_t expectedAnim = spellPrecastAnimId_ ? spellPrecastAnimId_ : anim::SPELL_PRECAST;
                if (in.currentAnimId == expectedAnim) spellPrecastAnimSeen_ = true;
                if (spellPrecastAnimSeen_ && oneShotComplete(in, expectedAnim)) {
                    state_ = State::SPELL_CASTING;
                }
                if (!spellPrecastAnimSeen_ && ++spellPrecastFrames_ > 10) {
                    state_ = State::SPELL_CASTING;
                }
            }
            break;

        case State::SPELL_CASTING:
            if (in.swimming || (in.jumping && !in.grounded) || (!in.grounded && !in.jumping)) {
                clearSpellState();
                state_ = State::INACTIVE;
            } else if (in.moving) {
                clearSpellState();
                state_ = State::INACTIVE;
            }
            // Stays in SPELL_CASTING until stopSpellCast() is called externally
            break;

        case State::SPELL_FINALIZE: {
            if (in.swimming || (in.jumping && !in.grounded)) {
                clearSpellState();
                state_ = State::INACTIVE;
            } else if (in.haveAnimState) {
                uint32_t expectedAnim = spellFinalizeAnimId_ ? spellFinalizeAnimId_
                    : (spellCastAnimId_ ? spellCastAnimId_ : anim::SPELL);
                if (in.currentAnimId == expectedAnim) spellFinalizeAnimSeen_ = true;
                if (spellFinalizeAnimSeen_ && oneShotComplete(in, expectedAnim)) {
                    clearSpellState();
                    state_ = in.inCombat ? State::COMBAT_IDLE : State::INACTIVE;
                }
                // Safety: if finalize anim never seen (model lacks it), finish after timeout
                if (!spellFinalizeAnimSeen_ && ++spellFinalizeFrames_ > 10) {
                    clearSpellState();
                    state_ = in.inCombat ? State::COMBAT_IDLE : State::INACTIVE;
                }
            }
            break;
        }

        case State::HIT_REACTION:
            if (in.swimming || in.moving) {
                hitReactionAnimId_ = 0;
                if (resumeSpellAfterHit_) clearSpellState();
                resumeSpellAfterHit_ = false;
                state_ = State::INACTIVE;
            } else if (in.haveAnimState) {
                if (in.currentAnimId == hitReactionAnimId_) hitReactionAnimSeen_ = true;
                // A model lacking the reaction never shows it; finish like
                // the spell finalize does.
                if ((hitReactionAnimSeen_ && oneShotComplete(in, hitReactionAnimId_)) ||
                    (!hitReactionAnimSeen_ && ++hitReactionFrames_ > 10)) {
                    hitReactionAnimId_ = 0;
                    if (resumeSpellAfterHit_) {
                        // Back to the held loop; a precast one-shot is not replayed.
                        resumeSpellAfterHit_ = false;
                        state_ = State::SPELL_CASTING;
                    } else {
                        state_ = in.inCombat ? State::COMBAT_IDLE : State::INACTIVE;
                    }
                }
            }
            break;

        case State::STUNNED:
            if (!stunned_) {
                state_ = in.inCombat ? State::COMBAT_IDLE : State::INACTIVE;
            } else if (in.swimming) {
                stunned_ = false;
                state_ = State::INACTIVE;
            }
            break;

        case State::CHARGE:
            if (!charging_) {
                state_ = State::INACTIVE;
            }
            break;

        case State::UNSHEATHE:
            if (in.swimming || in.moving) {
                weaponsDrawn_ = true;  // the draw is not replayed after an interrupted one
                state_ = State::INACTIVE;
            } else if (in.haveAnimState && oneShotComplete(in, anim::SHEATHE)) {
                state_ = State::COMBAT_IDLE;
            }
            break;

        case State::SHEATHE:
            weaponsDrawn_ = false;
            if (in.swimming || in.moving) {
                state_ = State::INACTIVE;
            } else if (in.inCombat) {
                state_ = State::COMBAT_IDLE;
            } else if (in.haveAnimState && oneShotComplete(in, anim::SHEATHE)) {
                state_ = State::INACTIVE;
            }
            break;
    }
}

// ── Animation resolution ─────────────────────────────────────────────────────

AnimOutput CombatFSM::resolve(const Input& in, const AnimCapabilitySet& caps,
                               const WeaponLoadout& loadout) {
    updateTransitions(in);

    // A kit animation outlasts the stun its spell applies: the original
    // knocks a stunned player down with Knockdown (121), then stuns.
    if (kitAnimId_ != 0) {
        if (in.swimming || in.moving) {
            kitAnimId_ = 0;
        } else if (in.haveAnimState) {
            if (in.currentAnimId == kitAnimId_) kitAnimSeen_ = true;
            if ((kitAnimSeen_ && oneShotComplete(in, kitAnimId_)) ||
                (!kitAnimSeen_ && ++kitAnimFrames_ > 10))
                kitAnimId_ = 0;
        }
        if (kitAnimId_ != 0) return AnimOutput::ok(kitAnimId_, false);
    }

    if (state_ == State::INACTIVE) return AnimOutput::stay();

    uint32_t animId = 0;
    bool loop = true;

    switch (state_) {
        case State::INACTIVE:
            return AnimOutput::stay();

        case State::COMBAT_IDLE:
            if (in.lowHealth && caps.resolvedStandWound) {
                animId = caps.resolvedStandWound;
            } else if (in.rangedWeaponActive && loadout.rangedType == RangedWeaponType::BOW) {
                animId = caps.resolvedReadyBow;
            } else if (in.rangedWeaponActive && loadout.rangedType == RangedWeaponType::GUN) {
                animId = caps.resolvedReadyRifle;
            } else if (in.rangedWeaponActive && loadout.rangedType == RangedWeaponType::CROSSBOW) {
                animId = caps.resolvedReadyCrossbow;
            } else if (in.rangedWeaponActive && loadout.rangedType == RangedWeaponType::THROWN) {
                animId = caps.resolvedReadyThrown;
            } else if (in.rangedWeaponActive && loadout.rangedType == RangedWeaponType::WAND) {
                animId = caps.resolvedReadyWand;
            } else if (loadout.is2HLoose) {
                animId = caps.resolvedReady2HLoose;
            } else if (loadout.inventoryType == game::InvType::TWO_HAND) {
                animId = caps.resolvedReady2H;
            } else if (loadout.isFist) {
                animId = caps.resolvedReadyFist;
            } else if (loadout.inventoryType == game::InvType::NON_EQUIP) {
                animId = caps.resolvedReadyUnarmed;
            } else {
                animId = caps.resolvedReady1H;
            }
            loop = true;
            break;

        case State::MELEE_SWING:
            if (in.specialAttackAnimId != 0) {
                animId = in.specialAttackAnimId;
            } else {
                // Resolve melee animation using probed capabilities + weapon
                // loadout. Which hand is already settled: this runs every frame
                // of the swing and has to answer the same way each time.
                const bool useOffHand = loadout.hasOffHand && offHandThisSwing_;

                if (useOffHand) {
                    // The off-hand weapon's kind, not the main hand's.
                    if (loadout.offHandIsFist) animId = caps.resolvedMeleeOffHandFist;
                    else if (loadout.offHandIsDagger) animId = caps.resolvedMeleeOffHandPierce;
                    else if (loadout.inventoryType == game::InvType::NON_EQUIP) animId = caps.resolvedMeleeOffHandUnarmed;
                    else animId = caps.resolvedMeleeOffHand;
                } else if (loadout.isFist) {
                    animId = caps.resolvedMeleeFist;
                } else if (loadout.isDagger) {
                    animId = caps.resolvedMeleePierce;
                } else if (loadout.is2HLoose) {
                    animId = caps.resolvedMelee2HLoose;
                } else if (loadout.inventoryType == game::InvType::TWO_HAND) {
                    animId = caps.resolvedMelee2H;
                } else if (loadout.inventoryType == game::InvType::NON_EQUIP) {
                    animId = caps.resolvedMeleeUnarmed;
                } else {
                    animId = caps.resolvedMelee1H;
                }
            }
            if (animId == 0) {
                LOG_DEBUG("CombatFSM: MELEE_SWING resolved animId=0, falling back to STAND");
                animId = anim::STAND;
            }
            loop = false;
            break;

        case State::RANGED_SHOOT:
            animId = in.rangedAnimId ? in.rangedAnimId : anim::ATTACK_BOW;
            loop = false;
            break;

        case State::RANGED_LOAD:
            switch (loadout.rangedType) {
                case RangedWeaponType::BOW:      animId = caps.resolvedLoadBow; break;
                case RangedWeaponType::GUN:      animId = caps.resolvedLoadRifle; break;
                case RangedWeaponType::CROSSBOW: animId = caps.resolvedLoadBow; break;
                default: break;
            }
            loop = false;
            break;

        case State::SPELL_PRECAST:
            animId = spellPrecastAnimId_ ? spellPrecastAnimId_ : anim::SPELL_PRECAST;
            loop = false;
            break;

        case State::SPELL_CASTING:
            animId = spellCastAnimId_ ? spellCastAnimId_ : anim::SPELL;
            loop = spellCastLoop_;
            break;

        case State::SPELL_FINALIZE:
            animId = spellFinalizeAnimId_ ? spellFinalizeAnimId_
                   : (spellCastAnimId_ ? spellCastAnimId_ : anim::SPELL);
            loop = false;
            break;

        case State::HIT_REACTION:
            animId = hitReactionAnimId_;
            loop = false;
            break;

        case State::STUNNED:
            animId = caps.resolvedStun ? caps.resolvedStun : anim::STUN;
            loop = true;
            break;

        case State::CHARGE:
            animId = caps.resolvedRun ? caps.resolvedRun : anim::RUN;
            loop = true;
            break;

        case State::UNSHEATHE:
            animId = caps.resolvedUnsheathe ? caps.resolvedUnsheathe : anim::SHEATHE;
            loop = false;
            break;

        case State::SHEATHE:
            animId = caps.resolvedSheathe ? caps.resolvedSheathe : anim::SHEATHE;
            loop = false;
            break;
    }

    if (animId == 0) return AnimOutput::stay();
    return AnimOutput::ok(animId, loop);
}

// ── Reset ────────────────────────────────────────────────────────────────────

void CombatFSM::reset() {
    state_ = State::INACTIVE;
    clearSpellState();
    hitReactionAnimId_ = 0;
    stunned_ = false;
    charging_ = false;
    weaponsDrawn_ = false;
    offHandTurn_ = false;
    offHandThisSwing_ = false;
}

} // namespace rendering
} // namespace wowee
