#include "core/animation_callback_handler.hpp"
#include "rendering/spell_visual_system.hpp"
#include "core/appearance_composer.hpp"
#include "core/entity_spawner.hpp"
#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include "rendering/renderer.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/camera_controller.hpp"
#include "rendering/animation_controller.hpp"
#include "rendering/animation/hit_reaction.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "rendering/animation/combat_fsm.hpp"
#include "rendering/animation/emote_registry.hpp"
#include "rendering/animation/stand_pose.hpp"
#include "game/game_handler.hpp"
#include "game/spell_classification.hpp"
#include "game/gather_spells.hpp"
#include "game/world_packets.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/audio_engine.hpp"
#include "audio/activity_sound_manager.hpp"
#include "audio/npc_voice_manager.hpp"
#include "audio/ui_sound_manager.hpp"

#include <cmath>
#include <algorithm>

namespace wowee { namespace core {

AnimationCallbackHandler::AnimationCallbackHandler(
    EntitySpawner& entitySpawner,
    rendering::Renderer& renderer,
    game::GameHandler& gameHandler,
    AppearanceComposer& appearanceComposer)
    : entitySpawner_(entitySpawner)
    , renderer_(renderer)
    , gameHandler_(gameHandler)
    , appearanceComposer_(appearanceComposer)
{
}

void AnimationCallbackHandler::resetChargeState() {
    chargeActive_ = false;
    chargeTimer_ = 0.0f;
    chargeDuration_ = 0.0f;
    chargeTargetGuid_ = 0;
}

bool AnimationCallbackHandler::updateCharge(float deltaTime) {
    if (!chargeActive_) return false;

    // Warrior Charge: lerp position from start to end using smoothstep
    chargeTimer_ += deltaTime;
    float t = std::min(chargeTimer_ / chargeDuration_, 1.0f);
    // smoothstep for natural acceleration/deceleration
    float s = t * t * (3.0f - 2.0f * t);
    glm::vec3 renderPos = chargeStartPos_ + (chargeEndPos_ - chargeStartPos_) * s;
    renderer_.getCharacterPosition() = renderPos;

    // Keep facing toward target and emit charge effect
    glm::vec3 dir = chargeEndPos_ - chargeStartPos_;
    float dirLenSq = glm::dot(dir, dir);
    if (dirLenSq > 1e-4f) {
        dir *= glm::inversesqrt(dirLenSq);
        float yawDeg = glm::degrees(std::atan2(dir.x, dir.y));
        renderer_.setCharacterYaw(yawDeg);
    }

    // Sync to game handler
    glm::vec3 canonical = core::coords::renderToCanonical(renderPos);
    gameHandler_.setPosition(canonical.x, canonical.y, canonical.z);

    // Update camera follow target
    if (renderer_.getCameraController()) {
        glm::vec3* followTarget = renderer_.getCameraController()->getFollowTargetMutable();
        if (followTarget) {
            *followTarget = renderPos;
        }
    }

    // Charge complete
    if (t >= 1.0f) {
        chargeActive_ = false;
        if (auto* ac = renderer_.getAnimationController()) ac->setCharging(false);
        renderer_.getCameraController()->setExternalFollow(false);
        renderer_.getCameraController()->setExternalMoving(false);

        // Snap to melee range of target's CURRENT position (it may have moved)
        if (chargeTargetGuid_ != 0) {
            auto targetEntity = gameHandler_.getEntityManager().getEntity(chargeTargetGuid_);
            if (targetEntity) {
                glm::vec3 targetCanonical(targetEntity->getX(), targetEntity->getY(), targetEntity->getZ());
                glm::vec3 targetRender = core::coords::canonicalToRender(targetCanonical);
                glm::vec3 toTarget = targetRender - renderPos;
                float dSq = glm::dot(toTarget, toTarget);
                if (dSq > 2.25f) {
                    // Place us 1.5 units from target (well within 8-unit melee range)
                    glm::vec3 snapPos = targetRender - toTarget * (1.5f * glm::inversesqrt(dSq));
                    renderer_.getCharacterPosition() = snapPos;
                    glm::vec3 snapCanonical = core::coords::renderToCanonical(snapPos);
                    gameHandler_.setPosition(snapCanonical.x, snapCanonical.y, snapCanonical.z);
                    if (renderer_.getCameraController()) {
                        glm::vec3* ft = renderer_.getCameraController()->getFollowTargetMutable();
                        if (ft) *ft = snapPos;
                    }
                }
            }
            gameHandler_.startAutoAttack(chargeTargetGuid_);
            if (auto* ac = renderer_.getAnimationController()) ac->triggerMeleeSwing();
        }

        // Send movement heartbeat so server knows our new position
        gameHandler_.sendMovement(game::Opcode::MSG_MOVE_HEARTBEAT);
    }

    return true; // charge is active
}

void AnimationCallbackHandler::playCombatBlood(uint64_t attackerGuid, uint64_t victimGuid, bool critical) {
    // M2 attachments ChestBloodFront and ChestBloodBack.
    constexpr uint32_t kAttachChestBloodFront = 15, kAttachChestBloodBack = 16;
    auto* visuals = renderer_.getSpellVisualSystem();
    auto* cr = renderer_.getCharacterRenderer();
    if (!visuals || !cr) return;
    const auto instanceOf = [this](uint64_t guid) -> uint32_t {
        if (guid == gameHandler_.getPlayerGuid()) return renderer_.getCharacterInstanceId();
        const uint32_t creature = entitySpawner_.getCreatureInstanceId(guid);
        return creature ? creature : entitySpawner_.getPlayerInstanceId(guid);
    };
    const auto victim = std::dynamic_pointer_cast<game::Unit>(gameHandler_.getEntityManager().getEntity(victimGuid));
    const uint32_t victimInstance = instanceOf(victimGuid), attackerInstance = instanceOf(attackerGuid);
    glm::vec3 attackerPos;
    if (!victim || !victimInstance || !attackerInstance || !cr->getInstancePosition(attackerInstance, attackerPos)) {
        LOG_DEBUG("Combat blood: attacker 0x", std::hex, attackerGuid, " or victim 0x", victimGuid,
                  std::dec, " not rendered; side unknown, no spurt");
        return;
    }
    // The side the attacker is on is the nearer of the two rendered blood
    // attachments, as the model is posed and turned right now.
    glm::mat4 front(1.0f), back(1.0f);
    const bool hasFront = cr->getAttachmentTransform(victimInstance, kAttachChestBloodFront, front);
    const bool hasBack = cr->getAttachmentTransform(victimInstance, kAttachChestBloodBack, back);
    if (!hasFront || !hasBack) {
        LOG_DEBUG("Combat blood: victim 0x", std::hex, victimGuid, std::dec,
                  " model lacks ChestBloodFront/Back attachments; no spurt");
        return;
    }
    const auto planar = [&](const glm::mat4& m) {
        const glm::vec2 d = glm::vec2(m[3]) - glm::vec2(attackerPos);
        return glm::dot(d, d);
    };
    const bool behind = planar(back) < planar(front);
    const uint32_t effect = entitySpawner_.combatBloodSpurt(victim->getDisplayId(), critical, behind);
    LOG_DEBUG("Combat blood: victim 0x", std::hex, victimGuid, std::dec, " display ", victim->getDisplayId(),
              critical ? " critical" : "", behind ? " from behind" : " from front", " effect ", effect);
    if (effect == 0) return;
    visuals->playEffectAtAttachment(effect, victimInstance, behind ? kAttachChestBloodBack : kAttachChestBloodFront);
}

void AnimationCallbackHandler::setupCallbacks() {
    // Sprint aura callback - use SPRINT(143) animation when sprint-type buff is active
    gameHandler_.setSprintAuraCallback([this](bool active) {
        auto* ac = renderer_.getAnimationController();
        if (ac) ac->setSprintAuraActive(active);
    });

    // Vehicle state callback - hide player character when inside a vehicle
    gameHandler_.setVehicleStateCallback([this](bool entered, uint32_t /*vehicleId*/) {
        auto* cr = renderer_.getCharacterRenderer();
        uint32_t instId = renderer_.getCharacterInstanceId();
        if (!cr || instId == 0) return;
        cr->setInstanceVisible(instId, !entered);
    });

    // Charge callback - warrior rushes toward target
    gameHandler_.setChargeCallback([this](uint64_t targetGuid, float tx, float ty, float tz) {
        if (!renderer_.getCameraController()) return;

        // Get current player position in render coords
        glm::vec3 startRender = renderer_.getCharacterPosition();
        // Convert target from canonical to render
        glm::vec3 targetRender = core::coords::canonicalToRender(glm::vec3(tx, ty, tz));

        // Compute direction and stop 2.0 units short (melee reach)
        glm::vec3 dir = targetRender - startRender;
        float distSq = glm::dot(dir, dir);
        if (distSq < 9.0f) return; // Too close, nothing to do
        float invDist = glm::inversesqrt(distSq);
        glm::vec3 dirNorm = dir * invDist;
        glm::vec3 endRender = targetRender - dirNorm * 2.0f;

        // Face toward target BEFORE starting charge
        float yawRad = std::atan2(dirNorm.x, dirNorm.y);
        float yawDeg = glm::degrees(yawRad);
        renderer_.setCharacterYaw(yawDeg);
        // Sync canonical orientation to server so it knows we turned
        float canonicalYaw = core::coords::characterYawDegToCanonical(yawDeg);
        gameHandler_.setOrientation(canonicalYaw);
        gameHandler_.sendMovement(game::Opcode::MSG_MOVE_SET_FACING);

        // Set charge state
        chargeActive_ = true;
        chargeTimer_ = 0.0f;
        chargeDuration_ = std::max(std::sqrt(distSq) / 25.0f, 0.3f); // ~25 units/sec
        chargeStartPos_ = startRender;
        chargeEndPos_ = endRender;
        chargeTargetGuid_ = targetGuid;

        // Disable player input, play charge animation
        renderer_.getCameraController()->setExternalFollow(true);
        renderer_.getCameraController()->clearMovementInputs();
        if (auto* ac = renderer_.getAnimationController()) ac->setCharging(true);

        // Charge/Intercept visual trails come from SpellVisualKit 44
        // (Spells\ChargeTrail.m2 + Spells\DustCloud_Land.m2) via SpellVisualSystem.

        // Charge's cast kit points at SoundEntries 3330 ("HeroicLeap"), whose file name
        // carries Blizzard's own typo. There is no Sound\Spells\Charge.wav.
        const float sfxScale = renderer_.getAudioCoordinator()
            ? renderer_.getAudioCoordinator()->getSfxVolumeScale() : 1.0f;
        if (sfxScale > 0.0f) {
            auto& audio = audio::AudioEngine::instance();
            if (!audio.playSound2D("Sound\\Spells\\HeroricLeap.wav", 0.8f * sfxScale)) {
                audio.playSound2D("Sound\\Item\\Weapons\\WeaponSwings\\mWooshLarge1.wav", 0.9f * sfxScale);
            }
        }
    });

    // GUID → CharacterRenderer instance for bone-attached spell visuals:
    // local player uses the dedicated character instance, other units the
    // spawner's creature/player instances (0 = not rendered, no attachment).
    gameHandler_.setUnitRenderInstanceResolver([this](uint64_t guid) -> uint32_t {
        if (const uint32_t drawn = entitySpawner_.drawnInstanceIdForGuid(guid)) return drawn;
        if (guid == gameHandler_.getPlayerGuid()) return renderer_.getCharacterInstanceId();
        uint32_t instanceId = entitySpawner_.getCreatureInstanceId(guid);
        if (instanceId == 0) instanceId = entitySpawner_.getPlayerInstanceId(guid);
        return instanceId;
    });

    // NPC/player death callback (online mode) - play death animation
    gameHandler_.setNpcDeathCallback([this](uint64_t guid, bool alreadyDead) {
        entitySpawner_.markCreatureDead(guid);
        if (!renderer_.getCharacterRenderer()) return;
        uint32_t instanceId = entitySpawner_.getCreatureInstanceId(guid);
        if (instanceId == 0) instanceId = entitySpawner_.getPlayerInstanceId(guid);
        if (guid == gameHandler_.getPlayerGuid()) instanceId = renderer_.getCharacterInstanceId();
        // A corpse already dead when it came into view lies as Dead (6); one
        // dying in view plays Death (1) and holds its last frame.
        if (instanceId != 0) {
            renderer_.getCharacterRenderer()->playAnimation(instanceId,
                alreadyDead ? rendering::anim::DEAD : rendering::anim::DEATH, alreadyDead);
            // Its casting effects die with it (SpellVisualSystem::removeCasterStages).
            if (auto* visuals = renderer_.getSpellVisualSystem()) visuals->removeCasterStages(instanceId);
        }
        if (!alreadyDead && renderer_.getAudioCoordinator()) {
            auto* ac = renderer_.getAudioCoordinator();
            if (guid == gameHandler_.getPlayerGuid()) {
                if (auto* act = ac->getActivitySoundManager()) act->playDeath();
            } else if (auto* nvm = ac->getNpcVoiceManager()) {
                if (auto ent = gameHandler_.getEntityManager().getEntity(guid);
                    ent && ent->getType() == game::ObjectType::UNIT) {
                    const uint32_t dispId = std::static_pointer_cast<game::Unit>(ent)->getDisplayId();
                    glm::vec3 pos = core::coords::canonicalToRender(
                        glm::vec3(ent->getX(), ent->getY(), ent->getZ()));
                    entitySpawner_.getRenderPositionForGuid(guid, pos);
                    nvm->playDeath(guid, dispId, pos);
                }
            }
        }
    });

    // NPC/player respawn callback (online mode) - play rise animation then idle
    gameHandler_.setNpcRespawnCallback([this](uint64_t guid) {
        entitySpawner_.unmarkCreatureDead(guid);
        if (!renderer_.getCharacterRenderer()) return;
        uint32_t instanceId = entitySpawner_.getCreatureInstanceId(guid);
        if (instanceId == 0) instanceId = entitySpawner_.getPlayerInstanceId(guid);
        if (guid == gameHandler_.getPlayerGuid()) instanceId = renderer_.getCharacterInstanceId();
        if (instanceId != 0) {
            auto* cr = renderer_.getCharacterRenderer();
            // Play RISE one-shot (auto-returns to STAND when finished), fall back to STAND
            if (cr->hasAnimation(instanceId, rendering::anim::RISE))
                cr->playAnimation(instanceId, rendering::anim::RISE, false);
            else
                cr->playAnimation(instanceId, rendering::anim::STAND, true);
        }
    });

    // NPC/player swing: the attack of the unit's main-hand weapon class
    // (EntitySpawner::meleeWeaponAnimations), resuming the unit's idle - its
    // ready stance while it is meleeing. Build 8606 starts the swing when the
    // packet arrives and finishes it within the hand's attack time
    // (UNIT_FIELD_BASEATTACKTIME, off hand +1): a human-model creature with
    // 1000 ms AttackUnarmed punched and recovered inside 500 ms swings, and at
    // 3000 ms kept the authored speed and then held its ready stance
    // (_experiments/20261118T091949Z.creature_swing_anim, s500 / h3000c).
    gameHandler_.setNpcSwingCallback([this](uint64_t guid, bool offHand) {
        auto* cr = renderer_.getCharacterRenderer();
        if (!cr) return;
        uint32_t instanceId = entitySpawner_.getCreatureInstanceId(guid);
        if (instanceId == 0) instanceId = entitySpawner_.getPlayerInstanceId(guid);
        if (instanceId == 0 || cr->isInstanceDead(instanceId)) return;
        const auto unit = std::dynamic_pointer_cast<game::Unit>(gameHandler_.getEntityManager().getEntity(guid));
        if (!unit) return;
        entitySpawner_.setCreatureRangedDrawn(guid, false);
        // Off-hand swings play the off-hand attack; each swing draws one of
        // the model's variations by frequency. Brutallus (two AttackUnarmed
        // and two AttackOff sequences, frequency 16383 each): native main-hand
        // swings matched both AttackUnarmed variations, off-hand swings both
        // AttackOff ones, never the other hand's (classify.py, mainA/offC).
        const uint32_t attack = offHand ? entitySpawner_.offHandAttackAnimation(*unit)
                                        : entitySpawner_.meleeWeaponAnimations(*unit).attack;
        cr->playAnimation(instanceId, attack, false, entitySpawner_.remoteResumeAnimation(guid));
        cr->pickWeightedVariation(instanceId);
        const uint16_t attackTimeField = game::fieldIndex(game::UF::UNIT_FIELD_BASEATTACKTIME);
        assert(attackTimeField != 0xFFFF);
        const uint32_t attackTimeMs = unit->getField(attackTimeField + (offHand ? 1 : 0));
        LOG_DEBUG("Swing 0x", std::hex, guid, std::dec, offHand ? " off" : " main", " animation ", attack,
                  " attack time ", attackTimeMs, " ms");
        if (attackTimeMs > 0) cr->fitOneShotDuration(instanceId, static_cast<float>(attackTimeMs));
        else LOG_WARNING("Swing of 0x", std::hex, guid, std::dec, " without an attack time; authored speed");
    });

    // Victim reaction from SMSG_ATTACKERSTATEUPDATE, one rule for the local
    // player, other players and creatures. AnimationData: CombatWound (9),
    // CombatCritical (10), Dodge (30), ShieldBlock (24), and the parry of the
    // victim's ready grip (20-23). Character models author ShieldBlock, not
    // Block (36): humanmale/dwarfmale/orcfemale.m2 have 24 and lack 36.
    gameHandler_.setHitReactionCallback([this](uint64_t attackerGuid, uint64_t victimGuid,
                                               game::GameHandler::HitReaction reaction) {
        auto* cr = renderer_.getCharacterRenderer();
        if (!cr) return;
        if (reaction == game::GameHandler::HitReaction::WOUND ||
            reaction == game::GameHandler::HitReaction::CRIT_WOUND)
            playCombatBlood(attackerGuid, victimGuid, reaction == game::GameHandler::HitReaction::CRIT_WOUND);

        const bool isLocalPlayer = (victimGuid == gameHandler_.getPlayerGuid());
        uint32_t instanceId = 0;
        if (isLocalPlayer) {
            if (gameHandler_.isPlayerDead() && !gameHandler_.isPlayerGhost()) return;
            instanceId = renderer_.getCharacterInstanceId();
        } else {
            instanceId = entitySpawner_.getCreatureInstanceId(victimGuid);
            if (instanceId == 0) instanceId = entitySpawner_.getPlayerInstanceId(victimGuid);
        }
        if (instanceId == 0) return;
        if (!isLocalPlayer && cr->isInstanceDead(instanceId)) return;
        auto* ac = renderer_.getAnimationController();

        uint32_t animId = 0;
        switch (reaction) {
            case game::GameHandler::HitReaction::WOUND:      animId = rendering::anim::COMBAT_WOUND; break;
            case game::GameHandler::HitReaction::CRIT_WOUND: animId = rendering::anim::COMBAT_CRITICAL; break;
            case game::GameHandler::HitReaction::DODGE:      animId = rendering::anim::DODGE; break;
            case game::GameHandler::HitReaction::BLOCK:
            case game::GameHandler::HitReaction::SHIELD_BLOCK: animId = rendering::anim::SHIELD_BLOCK; break;
            case game::GameHandler::HitReaction::PARRY: {
                uint32_t ready = 0;
                if (isLocalPlayer) {
                    if (!ac) return;
                    ready = ac->meleeReadyAnimation();
                } else {
                    auto unit = std::dynamic_pointer_cast<game::Unit>(
                        gameHandler_.getEntityManager().getEntity(victimGuid));
                    if (!unit) return;
                    ready = entitySpawner_.meleeReadyAnimation(*unit);
                }
                animId = rendering::parryAnimationForReady(ready);
                break;
            }
        }
        if (!cr->hasAnimation(instanceId, animId)) {
            LOG_DEBUG("Hit reaction ", animId, " not authored for unit 0x", std::hex, victimGuid, std::dec);
            return;
        }
        {
            const auto victim = std::dynamic_pointer_cast<game::Unit>(
                gameHandler_.getEntityManager().getEntity(victimGuid));
            if (!victim) {
                LOG_WARNING("Hit reaction ", animId, " on 0x", std::hex, victimGuid, std::dec, " without a unit entity");
                return;
            }
            const bool creature = victim->getType() == game::ObjectType::UNIT;
            uint32_t typeFlags = 0;
            if (creature) {
                const auto& cache = gameHandler_.getCreatureInfoCache();
                if (const auto info = cache.find(victim->getEntry()); info != cache.end())
                    typeFlags = info->second.typeFlags;
            }
            const auto modelFlags = entitySpawner_.creatureModelFlags(victim->getDisplayId());
            if (!modelFlags)
                LOG_WARNING("Hit reaction on 0x", std::hex, victimGuid, std::dec, ": display ",
                            victim->getDisplayId(), " has no CreatureModelData flags");
            if (!rendering::unitPlaysWound(animId, creature, typeFlags, modelFlags.value_or(0))) {
                LOG_DEBUG("Hit reaction ", animId, " suppressed on 0x", std::hex, victimGuid,
                          " (creature type flags 0x", typeFlags, ", model flags 0x", modelFlags.value_or(0), ")",
                          std::dec);
                return;
            }
        }
        LOG_DEBUG("Hit reaction ", animId, " on 0x", std::hex, victimGuid, std::dec,
                  isLocalPlayer ? " (local player)" : "");

        if (isLocalPlayer) {
            if (ac) ac->triggerHitReaction(animId);
            return;
        }
        if ((reaction == game::GameHandler::HitReaction::WOUND ||
             reaction == game::GameHandler::HitReaction::CRIT_WOUND) &&
            renderer_.getAudioCoordinator()) {
            if (auto* nvm = renderer_.getAudioCoordinator()->getNpcVoiceManager()) {
                if (auto ent = gameHandler_.getEntityManager().getEntity(victimGuid);
                    ent && ent->getType() == game::ObjectType::UNIT) {
                    const uint32_t dispId = std::static_pointer_cast<game::Unit>(ent)->getDisplayId();
                    glm::vec3 pos = core::coords::canonicalToRender(
                        glm::vec3(ent->getX(), ent->getY(), ent->getZ()));
                    entitySpawner_.getRenderPositionForGuid(victimGuid, pos);
                    nvm->playWound(victimGuid, dispId,
                                   reaction == game::GameHandler::HitReaction::CRIT_WOUND, pos);
                }
            }
        }
        cr->playAnimation(instanceId, animId, false, entitySpawner_.remoteResumeAnimation(victimGuid));
    });

    // Stun state callback - enters/exits STUNNED animation on local player
    gameHandler_.setStunStateCallback([this](bool stunned) {
        auto* ac = renderer_.getAnimationController();
        if (ac) ac->setStunned(stunned);
    });

    // Stealth state callback - switches to stealth animation variants
    gameHandler_.setStealthStateCallback([this](bool stealthed) {
        auto* ac = renderer_.getAnimationController();
        if (ac) ac->setStealthed(stealthed);
    });

    // Player health callback - switches to wounded idle when HP < 20%
    gameHandler_.setPlayerHealthCallback([this](uint32_t health, uint32_t maxHealth) {
        auto* ac = renderer_.getAnimationController();
        if (!ac) return;
        // 64-bit: health * 5 wraps in 32 bits above 858993459 HP (GM
        // characters with .modify hp 1000000000 played StandWound at full health).
        bool lowHp = (maxHealth > 0) && (health > 0) &&
                     (static_cast<uint64_t>(health) * 5 <= maxHealth);
        ac->setLowHealth(lowHp);
    });

    // Takeoff plays once, then holds the airborne loop. Landing is owned by
    // the remote movement state transition in the per-frame render sync.
    gameHandler_.setUnitAnimHintCallback([this](uint64_t guid, uint32_t animId) {
        auto* cr = renderer_.getCharacterRenderer();
        if (!cr) return;
        uint32_t instanceId = entitySpawner_.getPlayerInstanceId(guid);
        if (instanceId == 0) instanceId = entitySpawner_.getCreatureInstanceId(guid);
        if (instanceId == 0) return;
        // Don't override Death animation
        uint32_t curAnim = 0; float curT = 0.0f, curDur = 0.0f;
        if (cr->getAnimationState(instanceId, curAnim, curT, curDur) && rendering::anim::isCorpsePose(curAnim)) return;
        const bool takeoff = animId == rendering::anim::JUMP_START;
        cr->playAnimation(instanceId, animId, !takeoff,
                          takeoff ? rendering::anim::JUMP : 0);
    });

    // Unit move-flags callback - updates swimming and walking state from every MSG_MOVE_* packet.
    // This is more reliable than opcode-based hints for cold joins and heartbeats:
    // a player already swimming when we join will have SWIMMING set on the first heartbeat.
    // Walking(4) vs Running(5) is also driven here from the WALKING flag.
    gameHandler_.setUnitMoveFlagsCallback([this](uint64_t guid, uint32_t moveFlags,
                                                 uint32_t mask) {
        // Only what this update speaks about. A full movement block passes a
        // mask of all ones; the spline opcodes each pass the one bit they are
        // named after, so starting to swim no longer says anything about
        // whether the creature is also flying.
        auto& retainedFlags = entitySpawner_.getRemoteMovementFlags()[guid];
        const uint32_t previousFlags = retainedFlags;
        retainedFlags = (retainedFlags & ~mask) | (moveFlags & mask);
        if (retainedFlags != previousFlags)
            LOG_DEBUG("Unit move flags: guid=0x", std::hex, guid, " 0x", previousFlags,
                      " -> 0x", retainedFlags, std::dec);
        const auto apply = [&](game::MovementFlags flag, auto& state) {
            const uint32_t bit = static_cast<uint32_t>(flag);
            if ((mask & bit) == 0) return;
            if (moveFlags & bit) state[guid] = true;
            else                 state.erase(guid);
        };
        apply(game::MovementFlags::SWIMMING, entitySpawner_.getCreatureSwimmingState());
        apply(game::MovementFlags::WALKING,  entitySpawner_.getCreatureWalkingState());
        apply(game::MovementFlags::FLYING,   entitySpawner_.getCreatureFlyingState());
        const uint32_t hoverBit = static_cast<uint32_t>(game::MovementFlags::HOVER);
        if (mask & hoverBit) {
            // Set after LiftOff, it goes on to Hover; cleared, the unit stands
            // at once (no Land). updateRemoteUnitState swaps the idle. A
            // creature that comes into view already hovering (its create
            // block, the only full-mask update a creature gets) hovers too.
            auto entity = gameHandler_.getEntityManager().getEntity(guid);
            const bool createdHovering = mask == ~0u && (previousFlags & hoverBit) == 0 &&
                                         entity && entity->getType() == game::ObjectType::UNIT;
            const bool airborne = (moveFlags & hoverBit) &&
                                  (entitySpawner_.lastEmoteLiftOff(guid) || createdHovering);
            entitySpawner_.setHoverAirborne(guid, airborne);
        }
    });

    // Emote animation callback - play server-driven emote animations on NPCs and other players.
    // isState marks persistent STATE_ emotes (UNIT_NPC_EMOTESTATE or state-type
    // SMSG_EMOTE): they loop until cleared and are retained across model
    // reloads. One-shots play once and resume the retained state loop.
    gameHandler_.setEmoteAnimCallback([this](uint64_t guid, uint32_t emoteAnim, bool isState,
                                           uint32_t emoteId) {
        if (guid == gameHandler_.getPlayerGuid() && gameHandler_.isPlayerDead() &&
            !gameHandler_.isPlayerGhost()) return;
        auto* cr = renderer_.getCharacterRenderer();
        if (!cr) return;
        auto& registry = rendering::EmoteRegistry::instance();
        registry.loadFromDbc();
        const bool sheathe = registry.sheathWeaponsFor(emoteId);
        auto entity = gameHandler_.getEntityManager().getEntity(guid);
        const bool isRemote = entity && guid != gameHandler_.getPlayerGuid();
        auto& activeEmotes = entitySpawner_.getCreatureActiveEmotes();
        if (isRemote && isState) {
            // State emotes can arrive while the creature's model is still loading.
            // Retain the raw animation so spawnOnlineCreature can apply it once the
            // correct display is instantiated.
            if (emoteAnim == 0) activeEmotes.erase(guid);
            else                activeEmotes[guid] = emoteAnim;
        }
        // LiftOff on a hovering unit goes on to Hover; any other emote ends it.
        if (isRemote && emoteAnim != 0) {
            const uint32_t hoverBit = static_cast<uint32_t>(game::MovementFlags::HOVER);
            const auto flags = entitySpawner_.getRemoteMovementFlags().find(guid);
            const bool hovering = flags != entitySpawner_.getRemoteMovementFlags().end() &&
                                  (flags->second & hoverBit) != 0;
            const bool liftOff = emoteAnim == rendering::anim::LIFT_OFF;
            entitySpawner_.setLastEmoteLiftOff(guid, liftOff);
            entitySpawner_.setHoverAirborne(guid, liftOff && hovering);
        }
        // Look up creature instance first, then online players
        uint32_t emoteInstanceId = entitySpawner_.getCreatureInstanceId(guid);
        if (emoteInstanceId != 0) {
            cr->setGestureWeaponPose(emoteInstanceId, emoteAnim, sheathe);
            if (emoteAnim == 0) {
                // A state clear returns to idle. A one-shot cancel (SMSG_EMOTE 0)
                // must not stomp a persistent UNIT_NPC_EMOTESTATE work loop -
                // resume it instead (Westfall woodworkers hammer through these).
                if (isState) activeEmotes.erase(guid);
                auto retained = activeEmotes.find(guid);
                uint32_t resumeAnim = entitySpawner_.remoteResumeAnimation(guid);
                if (retained != activeEmotes.end() &&
                    cr->hasAnimation(emoteInstanceId, retained->second)) {
                    resumeAnim = retained->second;
                }
                cr->playAnimation(emoteInstanceId, resumeAnim, true);
            } else if (isState) {
                // A state emote loops the animation its Emotes.dbc row names -
                // STATE_DANCE is 69, the dance itself. 3.3.5 has no separate
                // looping variants to look for.
                activeEmotes[guid] = emoteAnim;
                cr->playAnimation(emoteInstanceId, emoteAnim, true);
            } else {
                // One-shot: play once, then resume the retained state loop, else
                // the unit's idle - its ready stance while it is meleeing.
                auto retained = activeEmotes.find(guid);
                uint32_t resumeAnim = entitySpawner_.remoteResumeAnimation(guid);
                if (retained != activeEmotes.end() &&
                    cr->hasAnimation(emoteInstanceId, retained->second)) {
                    resumeAnim = retained->second;
                }
                cr->playAnimation(emoteInstanceId, emoteAnim, false, resumeAnim);
            }
            return;
        }
        emoteInstanceId = entitySpawner_.getPlayerInstanceId(guid);
        if (emoteInstanceId != 0) {
            cr->setGestureWeaponPose(emoteInstanceId, emoteAnim, sheathe);
            const uint32_t idle = entitySpawner_.remoteIdleAnimation(guid);
            auto& emotes = rendering::EmoteRegistry::instance();
            emotes.loadFromDbc();
            if (!isState && rendering::isSeatedBaseAnimation(idle) &&
                emotes.canOverlaySeatedAnimation(emoteAnim) && cr->hasAnimation(emoteInstanceId, emoteAnim)) {
                cr->setUpperBodyAnimation(emoteInstanceId, 0, false);
                cr->setUpperBodyAnimation(emoteInstanceId, emoteAnim, false);
                return;
            }
            cr->playAnimation(emoteInstanceId, emoteAnim != 0 ? emoteAnim : idle,
                              isState || emoteAnim == 0, idle);
        }
    });

    // Food/water restoration: the server sits the player and its kit packets
    // schedule each eating gesture (SpellVisualSystem::playSpellVisualKit);
    // weapons go to the back without a sheath animation.
    gameHandler_.setRestorationPresentationCallback([this](bool active) {
        if (active) {
            if (!appearanceComposer_.isWeaponsSheathed()) {
                appearanceComposer_.setWeaponsSheathed(true);
                appearanceComposer_.loadEquippedWeapons();
            }
            return;
        }
        auto* cr = renderer_.getCharacterRenderer();
        auto* ac = renderer_.getAnimationController();
        const uint32_t instanceId = renderer_.getCharacterInstanceId();
        if (!cr || !ac || instanceId == 0) return;
        ac->setSeatedLoopAnimation(0);
        cr->setUpperBodyAnimation(instanceId, 0);
        ac->cancelSpellCast();
    });

    // Caster animation from the spell's SpellVisual kits (Spell.SpellVisualID
    // -> SpellVisual Precast/Cast/ChannelKit -> SpellVisualKit AnimID), for the
    // player, other players and creatures alike. A model lacking the animation
    // plays its AnimationData.dbc fallback (CharacterRenderer::playAnimation).
    //   PRECAST: held while the cast bar fills.
    //   CAST:    once at SMSG_SPELL_GO, then idle (or the channel's animation).
    //   CHANNEL: held for the channel.
    // Scarlet Wizard Arcane Explosion (8439): no precast hold (instant), cast
    // kit 1004 -> SpellCastOmni (54). Frostbolt (9672): kit 194 ReadySpellDirected
    // (51) held, kit 202 SpellCastDirected (53) at release. Scarlet Chaplain
    // Power Word: Shield/Renew on an ally: kits 99/270 -> 52/54, not 51/53.
    // A kit without an animation leaves the caster's current animation alone.
    gameHandler_.setSpellCastAnimCallback([this](uint64_t guid, game::SpellAnimPhase phase,
                                                  uint32_t spellId) {
        auto* cr = renderer_.getCharacterRenderer();
        if (!cr) return;
        const bool isLocalPlayer = guid == gameHandler_.getPlayerGuid();
        uint32_t instanceId = 0;
        if (isLocalPlayer) instanceId = renderer_.getCharacterInstanceId();
        if (instanceId == 0) instanceId = entitySpawner_.getCreatureInstanceId(guid);
        if (instanceId == 0) instanceId = entitySpawner_.getPlayerInstanceId(guid);
        if (instanceId == 0) return;
        auto* ac = isLocalPlayer ? renderer_.getAnimationController() : nullptr;
        if (isLocalPlayer && !ac) return;
        // A corpse keeps lying: native 2.4.3 showed no Whirlwind (37641 cast
        // kit, animation 126) for an SMSG_SPELL_GO sent from a dead Vashj'ir
        // Honor Guard, where WoWee stood it up spinning (Leotheras the Blind
        // was left whirling on the ground).
        if (!isLocalPlayer && cr->isInstanceDead(instanceId)) {
            castHolds_.erase(guid);
            castOneShots_.erase(guid);
            entitySpawner_.setRemoteCastHold(guid, 0);
            LOG_DEBUG("Cast animation phase ", static_cast<int>(phase), " of spell ", spellId,
                      " on corpse 0x", std::hex, guid, std::dec, " ignored");
            return;
        }

        const auto idle = [&] { return entitySpawner_.remoteIdleAnimation(guid); };
        const auto hold = [&](uint32_t spell, uint32_t animation, bool channel) {
            castHolds_[guid] = {spell, animation, channel};
            if (!isLocalPlayer) entitySpawner_.setRemoteCastHold(guid, animation);
        };
        const auto release = [&] {
            castHolds_.erase(guid);
            if (!isLocalPlayer) entitySpawner_.setRemoteCastHold(guid, 0);
        };
        const auto toIdle = [&] {
            castOneShots_.erase(guid);
            if (!castHolds_.contains(guid)) return;
            release();
            if (isLocalPlayer) {
                appearanceComposer_.showMiningPick(false);
                appearanceComposer_.showFishingPole(false);
                ac->cancelSpellCast();
            } else {
                cr->playAnimation(instanceId, idle(), true);
            }
        };
        if (phase == game::SpellAnimPhase::STOP) {
            toIdle();
            return;
        }

        assert(spellId != 0);
        gameHandler_.loadSpellNameCache();
        const auto spell = gameHandler_.spellNameCacheRef().find(spellId);
        if (spell == gameHandler_.spellNameCacheRef().end()) {
            LOG_WARNING("Cast animation: spell ", spellId, " missing from Spell.dbc");
            return;
        }
        auto* visuals = renderer_.getSpellVisualSystem();
        assert(visuals);
        const uint32_t visual = spell->second.spellVisualId;
        constexpr uint32_t none = rendering::SpellVisualSystem::kNoAnimation;

        // A creature's spell that needs a bow, gun or crossbow (Spell.dbc
        // EquippedItemClass 2 with one of those subclasses: Shoot 29575,
        // Multi-Shot 29576) is shot with the creature's ranged weapon, drawn
        // in hand from the cast's start until its next melee swing. Build
        // 8606 Spectral Sentry: sword in hand until the first Shoot, then
        // the crossbow between and through every shot.
        constexpr uint32_t kBowGunCrossbow = (1u << game::WEAPON_SUBCLASS_BOW) |
            (1u << game::WEAPON_SUBCLASS_GUN) | (1u << game::WEAPON_SUBCLASS_CROSSBOW);
        if (!isLocalPlayer && entitySpawner_.getCreatureInstanceId(guid) == instanceId &&
            spell->second.equippedItemClass == static_cast<int32_t>(game::ITEM_CLASS_WEAPON) &&
            (spell->second.equippedItemSubClassMask & kBowGunCrossbow) != 0) {
            const auto unit = std::dynamic_pointer_cast<game::Unit>(gameHandler_.getEntityManager().getEntity(guid));
            const auto ranged = unit ? entitySpawner_.rangedWeaponAnimations(*unit) : std::nullopt;
            if (ranged && (phase == game::SpellAnimPhase::PRECAST || phase == game::SpellAnimPhase::CAST)) {
                entitySpawner_.setCreatureRangedDrawn(guid, true);
                castOneShots_.erase(guid);
                if (phase == game::SpellAnimPhase::PRECAST) {
                    hold(spellId, ranged->aim, false);
                    cr->playAnimation(instanceId, ranged->aim, true);
                } else {
                    release();
                    cr->playAnimation(instanceId, ranged->shot, false, idle());
                }
                LOG_DEBUG("Ranged cast animation guid=0x", std::hex, guid, std::dec, " spell=", spellId,
                          " phase=", static_cast<int>(phase));
                return;
            }
        }

        switch (phase) {
        case game::SpellAnimPhase::PRECAST: {
            castOneShots_.erase(guid);
            const uint32_t animation = visuals->precastAnimation(visual);
            LOG_DEBUG("Cast animation: precast guid=0x", std::hex, guid, std::dec,
                      " spell=", spellId, " visual=", visual, " animation=", animation);
            if (animation == none) return;
            hold(spellId, animation, false);
            if (isLocalPlayer) {
                // The pick is the Mining kit's prop: EmoteWork alone swings an empty hand.
                if (game::isMiningRank(spellId)) appearanceComposer_.showMiningPick(true);
                ac->startSpellCast(0, animation, true, 0);
            } else {
                cr->playAnimation(instanceId, animation, true);
            }
            return;
        }
        case game::SpellAnimPhase::CAST: {
            const uint32_t animation = visuals->castAnimation(visual);
            LOG_DEBUG("Cast animation: cast guid=0x", std::hex, guid, std::dec,
                      " spell=", spellId, " visual=", visual, " animation=", animation);
            const auto held = castHolds_.find(guid);
            const bool underChannel = held != castHolds_.end() && held->second.channel &&
                                      held->second.spellId != spellId;
            if (animation == none) {
                // Only this spell's own precast ends here.
                if (held != castHolds_.end() && !held->second.channel && held->second.spellId == spellId)
                    toIdle();
                return;
            }
            castOneShots_[guid] = {spellId, animation};
            if (isLocalPlayer) {
                appearanceComposer_.showMiningPick(false);
                if (game::spellclass::isFishingCast(spellId)) {
                    // The cast swings the equipped pole: draw it first.
                    if (appearanceComposer_.isWeaponsSheathed()) {
                        appearanceComposer_.setWeaponsSheathed(false);
                        appearanceComposer_.loadEquippedWeapons();
                    }
                    appearanceComposer_.showFishingPole(true);
                }
            }
            if (underChannel) {
                // Once, then the channel's hold again.
                if (isLocalPlayer) ac->startSpellCast(animation, held->second.animation, true, 0);
                else cr->playAnimation(instanceId, animation, false, idle());
                return;
            }
            release();
            if (isLocalPlayer) {
                // Plays once from the cast state, then idle.
                ac->startSpellCast(0, animation, false, animation);
                ac->stopSpellCast();
            } else {
                cr->playAnimation(instanceId, animation, false, idle());
            }
            return;
        }
        case game::SpellAnimPhase::CHANNEL: {
            const uint32_t animation = visuals->channelAnimation(visual);
            LOG_DEBUG("Cast animation: channel guid=0x", std::hex, guid, std::dec,
                      " spell=", spellId, " visual=", visual, " animation=", animation);
            if (animation == none) return;
            const auto oneShot = castOneShots_.find(guid);
            const uint32_t first = oneShot != castOneShots_.end() && oneShot->second.spellId == spellId
                ? oneShot->second.animation : 0;
            castOneShots_.erase(guid);
            const uint32_t previousIdle = idle();
            hold(spellId, animation, true);
            if (isLocalPlayer) {
                ac->startSpellCast(first, animation, true, 0);
            } else if (first != 0) {
                cr->retargetOneShotReturn(instanceId, previousIdle, animation);
            } else {
                cr->playAnimation(instanceId, animation, true);
            }
            return;
        }
        case game::SpellAnimPhase::STOP:
            break;
        }
    });

    // Stand state animation callback - route through AnimationController state machine
    // for proper sit/sleep/kneel transition animations (down → loop → up)
    gameHandler_.setStandStateCallback([this](uint8_t standState) {
        using AC = rendering::AnimationController;

        // The camera controller's two flags, from the one number: movement is
        // blocked while sitting or kneeling, and grounding stands down while
        // the server has the character in a chair.
        //
        // Both used to be set, by two callbacks - and this is one slot, so the
        // one registered later replaced the other. This handler is built long
        // after the world is, so the registration that carried
        // applyServerStandState never ran again: seatedInChair_ stayed false,
        // the chair stayed a floor, and a character sat down in the right pose
        // on top of the furniture rather than in it.
        if (auto* cc = renderer_.getCameraController()) {
            cc->applyServerStandState(standState);
        }

        // Sitting, sleeping and kneeling put the weapons away: the original
        // client sends CMSG_SETSHEATHED 0 on each (pair 1, the other client's
        // view of its UNIT_FIELD_BYTES_2: /sit, /sleep and /kneel from melee
        // and ranged all went to 0; standing up leaves them sheathed; /afk
        // alone does not). Chair seats and death were not measured.
        if (standState == AC::STAND_STATE_SIT || standState == AC::STAND_STATE_SLEEP ||
            standState == AC::STAND_STATE_KNEEL) {
            appearanceComposer_.setWeaponsSheathed(true);
        }
        if (auto* cr = renderer_.getCharacterRenderer(); cr && renderer_.getCharacterInstanceId())
            cr->setStandState(renderer_.getCharacterInstanceId(), standState);

        auto* ac = renderer_.getAnimationController();
        if (!ac) return;

        // Death is special - play directly, not through sit state machine
        if (standState == AC::STAND_STATE_DEAD) {
            auto* cr = renderer_.getCharacterRenderer();
            if (!cr) return;
            uint32_t charInstId = renderer_.getCharacterInstanceId();
            if (charInstId == 0) return;
            cr->playAnimation(charInstId, rendering::anim::DEATH, false);
            return;
        }

        ac->setStandState(standState);
        // Restoration food/water is an aura rather than MSG_CHANNEL_START on
        // supported realms. The aura update starts its loop; standing up ends it.
        // Only stop the cast presentation while restoring - the server also
        // stands a seated player when a real cast begins, and stopping there
        // would kill that cast's animation as it starts.
        if (standState == AC::STAND_STATE_STAND) {
            ac->setSeatedLoopAnimation(0);
            if (gameHandler_.isRestoring()) ac->stopSpellCast();
        }
    });

    // Loot window callback - play kneel/loot animation while looting
    gameHandler_.setLootWindowCallback([this](bool open) {
        auto* ac = renderer_.getAnimationController();
        if (!ac) return;
        if (open) ac->startLooting();
        else      ac->stopLooting();
    });
}

}} // namespace wowee::core
