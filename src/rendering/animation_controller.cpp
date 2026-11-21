#include <span>

#include "rendering/animation/melee_anim_chains.hpp"
#include "rendering/animation/weapon_spell_anim.hpp"
#include "rendering/animation_controller.hpp"
#include "rendering/animation/emote_registry.hpp"
#include "rendering/animation/stand_pose.hpp"
#include "rendering/animation/anim_capability_probe.hpp"
#include "rendering/animation/mount_fsm.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "rendering/renderer.hpp"
#include "rendering/camera.hpp"
#include "rendering/camera_controller.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/terrain_manager.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/water_renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/levelup_effect.hpp"
#include "rendering/spell_visual_system.hpp"
#include "pipeline/m2_loader.hpp"
#include "pipeline/asset_manager.hpp"
#include "game/inventory.hpp"
#include "game/game_handler.hpp"
#include "core/application.hpp"
#include "core/logger.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/audio_engine.hpp"
#include "audio/footstep_manager.hpp"
#include "audio/activity_sound_manager.hpp"
#include "audio/mount_sound_manager.hpp"
#include "audio/music_manager.hpp"
#include "audio/movement_sound_manager.hpp"
#include "rendering/swim_effects.hpp"
#include <algorithm>
#include <cstdlib>
#include <unordered_map>
#include <set>
#include <random>
#include <cctype>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>

namespace wowee {
namespace rendering {

// ── AnimationController implementation ───────────────────────────────────────

AnimationController::AnimationController() = default;
AnimationController::~AnimationController() = default;

void AnimationController::initialize(Renderer* renderer) {
    renderer_ = renderer;
}

void AnimationController::probeCapabilities() {
    if (!renderer_) return;
    uint32_t instanceId = renderer_->getCharacterInstanceId();
    if (instanceId == 0) return;
    auto caps = AnimCapabilityProbe::probe(renderer_, instanceId);
    characterAnimator_.setCapabilities(caps);
    capabilitiesProbed_ = true;
}

void AnimationController::onCharacterFollow(uint32_t /*instanceId*/) {
    // Reset animation state when follow target changes
    capabilitiesProbed_ = false;
}

// ── Emote support ────────────────────────────────────────────────────────────

void AnimationController::playEmote(const std::string& emoteName) {
    auto& registry = EmoteRegistry::instance();
    registry.loadFromDbc();
    auto result = registry.findEmote(emoteName);
    if (!result) return;

    uint32_t animId = result->animId;
    bool loop = result->loop;
    const uint8_t standState = characterAnimator_.getActivity().getStandState();
    if (!emoteAllowsStandState(result->flags, standState)) return;
    if (isSeatedStandState(standState) && !loop && registry.canOverlaySeatedAnimation(animId)) {
        auto* cr = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
        const uint32_t instanceId = renderer_ ? renderer_->getCharacterInstanceId() : 0;
        if (cr && instanceId && cr->hasAnimation(instanceId, animId)) {
            cr->setUpperBodyAnimation(instanceId, 0, false);
            cr->setUpperBodyAnimation(instanceId, animId, false);
            cr->setGestureWeaponPose(instanceId, animId, result->sheathWeapons);
        }
        return;
    }

    // What an emote resolved to, and whether the model can play it. An emote
    // that does nothing is either a name that resolved to no animation or an
    // animation the model does not carry, and from outside those look the same.
    {
        auto* cr = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
        uint32_t ci = renderer_ ? renderer_->getCharacterInstanceId() : 0;
        const bool has = (cr && ci > 0) ? cr->hasAnimation(ci, animId) : false;
        LOG_WARNING("Emote '", emoteName, "' -> animation ", animId,
                    (loop ? " (looping)" : " (one shot)"),
                    " model has it: ", (has ? "yes" : "NO"));
    }

    // Forward to CharacterAnimator (ActivityFSM handles emote state)
    characterAnimator_.playEmote(animId, loop);

    // Immediately play the emote animation on the renderer
    auto* characterRenderer = renderer_->getCharacterRenderer();
    uint32_t characterInstanceId = renderer_->getCharacterInstanceId();
    if (characterRenderer && characterInstanceId > 0) {
        characterRenderer->playAnimation(characterInstanceId, animId, loop);
        characterRenderer->setGestureWeaponPose(characterInstanceId, animId, result->sheathWeapons);
        lastPlayerAnimRequest_ = animId;
        lastPlayerAnimLoopRequest_ = loop;
    }
}

void AnimationController::playWeaponSheathAnimation(SheathSpot mainHand, SheathSpot offHand) {
    if (!renderer_) return;
    auto* characterRenderer = renderer_->getCharacterRenderer();
    const uint32_t characterInstanceId = renderer_->getCharacterInstanceId();
    if (!characterRenderer || characterInstanceId == 0) return;

    // One animation both ways: 3.3.5 has Sheath and HipSheath and no unsheathe,
    // and plays the reach for drawing as well as for putting away. Each arm
    // reaches for its own hand's item and an empty hand stays still; the rest
    // of the body follows whichever hand holds something.
    auto reach = [](SheathSpot spot) {
        return spot == SheathSpot::HIP  ? anim::HIP_SHEATHE
             : spot == SheathSpot::BACK ? anim::SHEATHE
                                        : anim::STAND;
    };
    const SheathSpot body = mainHand != SheathSpot::NONE ? mainHand : offHand;
    if (body == SheathSpot::NONE) return;
    uint32_t animId = reach(body);
    if (!characterRenderer->hasAnimation(characterInstanceId, animId)) {
        animId = body == SheathSpot::HIP ? anim::SHEATHE : anim::HIP_SHEATHE;
        if (!characterRenderer->hasAnimation(characterInstanceId, animId)) return;
    }

    // ActivityFSM owns the one-shot until completion so locomotion/combat does
    // not replace the reach animation on the next frame.
    characterAnimator_.playEmote(animId, false);
    characterRenderer->playAnimation(characterInstanceId, animId, false);
    characterRenderer->setArmAnimations(characterInstanceId, reach(offHand), reach(mainHand));
    lastPlayerAnimRequest_ = animId;
    lastPlayerAnimLoopRequest_ = false;
}

void AnimationController::cancelEmote() {
    characterAnimator_.cancelEmote();
}

std::string AnimationController::getEmoteText(const std::string& emoteName, const std::string* targetName) {
    auto& registry = EmoteRegistry::instance();
    registry.loadFromDbc();
    return registry.textFor(emoteName, targetName);
}

uint32_t AnimationController::getEmoteDbcId(const std::string& emoteName) {
    auto& registry = EmoteRegistry::instance();
    registry.loadFromDbc();
    return registry.dbcIdFor(emoteName);
}

std::string AnimationController::getEmoteTextByDbcId(uint32_t dbcId, const std::string& senderName,
                                                      const std::string* targetName) {
    auto& registry = EmoteRegistry::instance();
    registry.loadFromDbc();
    return registry.textByDbcId(dbcId, senderName, targetName);
}
uint32_t AnimationController::getEmoteAnimByEmotesId(uint32_t emoteId) {
    auto& registry = EmoteRegistry::instance();
    registry.loadFromDbc();
    return registry.animByEmotesId(emoteId);
}

bool AnimationController::isStateEmoteById(uint32_t emoteId) {
    auto& registry = EmoteRegistry::instance();
    registry.loadFromDbc();
    return registry.isStateEmote(emoteId);
}

// ── Spell casting ────────────────────────────────────────────────────────────

void AnimationController::startSpellCast(uint32_t precastAnimId, uint32_t castAnimId, bool castLoop,
                                         uint32_t finalizeAnimId) {
    // A starting cast or channel takes over from any in-flight swing timer;
    // otherwise CombatFSM::updateTransitions's melee/ranged override clears
    // the spell state on the very next frame.
    meleeSwingTimer_ = 0.0f;
    hasPendingMeleeSwing_ = false;
    specialAttackAnimId_ = 0;
    rangedShootTimer_ = 0.0f;
    // Allow consecutive identical one-shot casts (e.g. Mortal Strike followed
    // immediately by Heroic Strike) to restart from t=0.
    lastPlayerAnimRequest_ = UINT32_MAX;
    characterAnimator_.startSpellCast(precastAnimId, castAnimId, castLoop, finalizeAnimId);
}

void AnimationController::stopSpellCast() {
    characterAnimator_.stopSpellCast();
}

void AnimationController::cancelSpellCast() {
    characterAnimator_.cancelSpellCast();
}

void AnimationController::setSeatedLoopAnimation(uint32_t animationId) {
    characterAnimator_.getActivity().setSeatedLoopAnimation(animationId);
}

// ── Loot animation ───────────────────────────────────────────────────────────

void AnimationController::startLooting() {
    characterAnimator_.startLooting();
}

void AnimationController::stopLooting() {
    characterAnimator_.stopLooting();
}

// ── Hit reactions ────────────────────────────────────────────────────────────

void AnimationController::triggerHitReaction(uint32_t animId) {
    if (animId != 0 && meleeSwingTimer_ <= 0.0f && rangedShootTimer_ <= 0.0f &&
        !charging_ && !characterAnimator_.getCombat().isStunned()) {
        // Allow back-to-back hit reactions of the same animId to replay.
        lastPlayerAnimRequest_ = UINT32_MAX;
    }
    characterAnimator_.triggerHitReaction(animId);
}

// ── Crowd control ────────────────────────────────────────────────────────────

void AnimationController::setStunned(bool stunned) {
    stunned_ = stunned;
    characterAnimator_.setStunned(stunned);
}

// ── Health-based idle ────────────────────────────────────────────────────────

void AnimationController::setLowHealth(bool low) {
    characterAnimator_.setLowHealth(low);
}

// ── Stand state ──────────────────────────────────────────────────────────────

void AnimationController::setStandState(uint8_t state) {
    characterAnimator_.setStandState(state);
}

// ── Stealth ──────────────────────────────────────────────────────────────────

void AnimationController::setStealthed(bool stealth) {
    characterAnimator_.setStealthed(stealth);
}

// ── Sprint aura ──────────────────────────────────────────────────────────────

void AnimationController::setSprintAuraActive(bool active) {
    // Straight through. The animator keeps the copy the locomotion state
    // machine reads; this class kept one of its own and read it nowhere.
    characterAnimator_.setSprintAuraActive(active);
}

// ── Targeting / combat ───────────────────────────────────────────────────────

void AnimationController::setTargetPosition(const glm::vec3* pos) {
    targetPosition_ = pos;
}

void AnimationController::setInCombat(bool combat) {
    inCombat_ = combat;
    characterAnimator_.setInCombat(combat);
}

void AnimationController::resetCombatVisualState() {
    inCombat_ = false;
    targetPosition_ = nullptr;
    meleeSwingTimer_ = 0.0f;
    meleeSwingCooldown_ = 0.0f;
    hasPendingMeleeSwing_ = false;
    pendingMeleeSwingOffHand_ = false;
    meleeSwingOffHand_ = false;
    specialAttackAnimId_ = 0;
    rangedShootTimer_ = 0.0f;
    rangedAnimId_ = 0;
    restoreWeaponAfterRangedShot_ = false;
    characterAnimator_.setRangedWeaponActive(false);
    stunned_ = false;
    charging_ = false;

    // Reset all CharacterAnimator combat state
    characterAnimator_.setInCombat(false);
    characterAnimator_.setStunned(false);
    characterAnimator_.setCharging(false);
    characterAnimator_.setLowHealth(false);
    characterAnimator_.stopSpellCast();
    characterAnimator_.triggerHitReaction(0);  // Clear hit reaction

    if (auto* svs = renderer_->getSpellVisualSystem()) svs->reset();
}

bool AnimationController::isMoving() const {
    auto* cameraController = renderer_->getCameraController();
    return cameraController && cameraController->isMoving();
}

// ── Melee combat ─────────────────────────────────────────────────────────────

void AnimationController::startLocalMeleeSwing(bool offHand) {
    auto* characterRenderer = renderer_->getCharacterRenderer();
    uint32_t characterInstanceId = renderer_->getCharacterInstanceId();
    if (!characterRenderer || characterInstanceId == 0) return;
    if (characterAnimator_.getActivity().isEmoteActive()) {
        characterAnimator_.cancelEmote();
    }
    specialAttackAnimId_ = 0;  // Clear any special attack override
    meleeSwingOffHand_ = offHand;
    ++meleeSwingSerial_;
    // Ensure consecutive swings (including same-hand Thrash/Windfury procs)
    // cross-fade cleanly into a fresh sequence instead of being deduplicated.
    lastPlayerAnimRequest_ = UINT32_MAX;

    resolveMeleeAnimId();
    meleeSwingCooldown_ = 0.1f;
    float durationSec = meleeAnimDurationMs_ > 0.0f ? meleeAnimDurationMs_ / 1000.0f : 0.6f;
    if (durationSec < 0.25f) durationSec = 0.25f;
    if (durationSec > 1.0f) durationSec = 1.0f;
    meleeSwingTimer_ = durationSec;

    if (renderer_->getAudioCoordinator()->getActivitySoundManager()) {
        renderer_->getAudioCoordinator()->getActivitySoundManager()->playMeleeSwing();
    }
}

void AnimationController::triggerMeleeSwing(bool offHand) {
    auto* characterRenderer = renderer_->getCharacterRenderer();
    uint32_t characterInstanceId = renderer_->getCharacterInstanceId();
    if (!characterRenderer || characterInstanceId == 0) return;
    // If a melee swing is already in flight, queue the incoming swing in the
    // 1-slot pending buffer rather than dropping it or cutting the active swing
    // short mid-animation.
    if (meleeSwingTimer_ > 0.0f) {
        hasPendingMeleeSwing_ = true;
        pendingMeleeSwingOffHand_ = offHand;
        return;
    }
    if (meleeSwingCooldown_ > 0.0f) return;
    startLocalMeleeSwing(offHand);
}

void AnimationController::setEquippedWeaponType(uint32_t inventoryType, bool is2HLoose,
                                                 bool isFist, bool isDagger,
                                                 bool hasOffHand, bool hasShield,
                                                 bool offHandIsFist,
                                                 bool offHandIsDagger) {
    weaponLoadout_.inventoryType = inventoryType;
    weaponLoadout_.is2HLoose = is2HLoose;
    weaponLoadout_.isFist = isFist;
    weaponLoadout_.isDagger = isDagger;
    weaponLoadout_.hasOffHand = hasOffHand;
    weaponLoadout_.offHandIsFist = offHandIsFist;
    weaponLoadout_.offHandIsDagger = offHandIsDagger;
    weaponLoadout_.hasShield = hasShield;
    meleeAnimId_ = 0;  // Force re-resolve on next swing
    characterAnimator_.setEquippedWeaponType(weaponLoadout_);
}

void AnimationController::triggerSpecialAttack(uint32_t /*spellId*/) {
    auto* characterRenderer = renderer_->getCharacterRenderer();
    uint32_t characterInstanceId = renderer_->getCharacterInstanceId();
    if (!characterRenderer || characterInstanceId == 0) return;
    if (meleeSwingCooldown_ > 0.0f) return;
    if (characterAnimator_.getActivity().isEmoteActive()) {
        characterAnimator_.cancelEmote();
    }

    auto has = [&](uint32_t id) { return characterRenderer->hasAnimation(characterInstanceId, id); };

    // Choose generic special attack animation based on equipped main-hand weapon
    // (ShieldBash 59 comes from SpellVisualKit.dbc for shield abilities, never
    // from merely having a shield equipped).
    uint32_t specAnim = resolveWeaponSpellAnimation(anim::SPECIAL_1H, weaponLoadout_);
    if (!has(specAnim)) {
        if (has(anim::SPECIAL_1H)) specAnim = anim::SPECIAL_1H;
        else if (has(anim::SPECIAL_UNARMED)) specAnim = anim::SPECIAL_UNARMED;
        else specAnim = 0;
    }

    if (specAnim == 0) {
        // No special animation available - fall back to regular melee swing
        triggerMeleeSwing();
        return;
    }

    specialAttackAnimId_ = specAnim;
    hasPendingMeleeSwing_ = false;
    meleeSwingCooldown_ = 0.1f;
    // Query the special attack animation duration
    std::vector<pipeline::M2Sequence> sequences;
    float dur = 0.6f;
    if (characterRenderer->getAnimationSequences(characterInstanceId, sequences)) {
        for (const auto& seq : sequences) {
            if (seq.id == specAnim && seq.duration > 0) {
                dur = static_cast<float>(seq.duration) / 1000.0f;
                break;
            }
        }
    }
    if (dur < 0.25f) dur = 0.25f;
    if (dur > 1.0f) dur = 1.0f;
    meleeSwingTimer_ = dur;
    if (renderer_->getAudioCoordinator()->getActivitySoundManager()) {
        renderer_->getAudioCoordinator()->getActivitySoundManager()->playMeleeSwing();
    }
}

// ── Ranged combat ────────────────────────────────────────────────────────────

void AnimationController::setEquippedRangedType(RangedWeaponType type) {
    weaponLoadout_.rangedType = type;
    rangedAnimId_ = 0;
    characterAnimator_.setEquippedRangedType(type);
}

void AnimationController::setRangedWeaponActive(bool active) {
    characterAnimator_.setRangedWeaponActive(active);
}

void AnimationController::setCharging(bool c) {
    charging_ = c;
    characterAnimator_.setCharging(c);
}

void AnimationController::triggerRangedShot() {
    auto* characterRenderer = renderer_->getCharacterRenderer();
    uint32_t characterInstanceId = renderer_->getCharacterInstanceId();
    if (!characterRenderer || characterInstanceId == 0) return;
    if (rangedShootTimer_ > 0.0f) return;
    if (characterAnimator_.getActivity().isEmoteActive()) characterAnimator_.cancelEmote();

    auto has = [&](uint32_t id) { return characterRenderer->hasAnimation(characterInstanceId, id); };

    // Resolve ranged attack animation based on weapon type
    uint32_t shootAnim = 0;
    switch (weaponLoadout_.rangedType) {
        case RangedWeaponType::BOW:
            if (has(anim::FIRE_BOW))        shootAnim = anim::FIRE_BOW;
            else if (has(anim::ATTACK_BOW)) shootAnim = anim::ATTACK_BOW;
            break;
        case RangedWeaponType::GUN:
            if (has(anim::ATTACK_RIFLE))    shootAnim = anim::ATTACK_RIFLE;
            break;
        case RangedWeaponType::CROSSBOW:
            if (has(anim::ATTACK_BOW)) shootAnim = anim::ATTACK_BOW;
            break;
        case RangedWeaponType::THROWN:
            if (has(anim::ATTACK_THROWN))    shootAnim = anim::ATTACK_THROWN;
            break;
        case RangedWeaponType::WAND: {
            // WOWEE_WAND_ANIM=<id> overrides the shot animation, for comparing
            // candidates on screen: 32 cast, 49 rifle, 53 directed, 54 omni.
            static const uint32_t kForced = []() -> uint32_t {
                const char* v = std::getenv("WOWEE_WAND_ANIM");
                return v ? static_cast<uint32_t>(std::atoi(v)) : 0;
            }();
            if (kForced != 0 && has(kForced))        shootAnim = kForced;
            else if (has(anim::SPELL_CAST_DIRECTED)) shootAnim = anim::SPELL_CAST_DIRECTED;
            else if (has(anim::ATTACK_RIFLE))        shootAnim = anim::ATTACK_RIFLE;
            break;
        }
        default: break;
    }
    if (shootAnim == 0) return;  // Model has no ranged animation

    rangedAnimId_ = shootAnim;

    // Query animation duration
    std::vector<pipeline::M2Sequence> sequences;
    float dur = 0.6f;
    if (characterRenderer->getAnimationSequences(characterInstanceId, sequences)) {
        for (const auto& seq : sequences) {
            if (seq.id == shootAnim && seq.duration > 0) {
                dur = static_cast<float>(seq.duration) / 1000.0f;
                break;
            }
        }
    }
    if (dur < 0.25f) dur = 0.25f;
    if (dur > 1.5f) dur = 1.5f;
    rangedShootTimer_ = dur;
    restoreWeaponAfterRangedShot_ =
        (weaponLoadout_.rangedType == RangedWeaponType::THROWN);
}

uint32_t AnimationController::resolveMeleeAnimId() {
    auto* characterRenderer = renderer_->getCharacterRenderer();
    uint32_t characterInstanceId = renderer_->getCharacterInstanceId();
    if (!characterRenderer || characterInstanceId == 0) {
        meleeAnimId_ = 0;
        meleeAnimDurationMs_ = 0.0f;
        return 0;
    }

    // When dual-wielding, bypass cache to alternate main/off-hand animations
    if (!weaponLoadout_.hasOffHand && meleeAnimId_ != 0 && characterRenderer->hasAnimation(characterInstanceId, meleeAnimId_)) {
        return meleeAnimId_;
    }

    std::vector<pipeline::M2Sequence> sequences;
    if (!characterRenderer->getAnimationSequences(characterInstanceId, sequences)) {
        meleeAnimId_ = 0;
        meleeAnimDurationMs_ = 0.0f;
        return 0;
    }

    auto findDuration = [&](uint32_t id) -> float {
        for (const auto& seq : sequences) {
            if (seq.id == id && seq.duration > 0) {
                return static_cast<float>(seq.duration);
            }
        }
        return 0.0f;
    };

    std::span<const uint32_t> chain;
    // The chains live in melee_anim_chains.hpp, which the capability probe
    // reads too - it used to keep its own copies under a comment saying they
    // matched these.
    using anim::MeleeChain;

    // Dual-wield: use authoritative hand from the incoming swing (HITINFO_LEFTSWING).
    bool useOffHand = weaponLoadout_.hasOffHand && meleeSwingOffHand_;

    if (useOffHand) {
        // The off-hand weapon's kind, not the main hand's.
        if (weaponLoadout_.offHandIsFist) {
            chain = anim::meleeAnimChain(MeleeChain::OffHandFist);
        } else if (weaponLoadout_.offHandIsDagger) {
            chain = anim::meleeAnimChain(MeleeChain::OffHandPierce);
        } else if (weaponLoadout_.inventoryType == game::InvType::NON_EQUIP) {
            chain = anim::meleeAnimChain(MeleeChain::OffHandUnarmed);
        } else {
            chain = anim::meleeAnimChain(MeleeChain::OffHand);
        }
    } else if (weaponLoadout_.isFist) {
        chain = anim::meleeAnimChain(MeleeChain::Fist);
    } else if (weaponLoadout_.isDagger) {
        chain = anim::meleeAnimChain(MeleeChain::Dagger);
    } else if (weaponLoadout_.is2HLoose) {
        // Polearm thrust uses pierce variant
        chain = anim::meleeAnimChain(MeleeChain::TwoHandLoose);
    } else if (weaponLoadout_.inventoryType == game::InvType::TWO_HAND) {
        chain = anim::meleeAnimChain(MeleeChain::TwoHand);
    } else if (weaponLoadout_.inventoryType == game::InvType::NON_EQUIP) {
        chain = anim::meleeAnimChain(MeleeChain::Unarmed);
    } else {
        chain = anim::meleeAnimChain(MeleeChain::OneHand);
    }
    for (uint32_t id : chain) {
        if (characterRenderer->hasAnimation(characterInstanceId, id)) {
            meleeAnimId_ = id;
            meleeAnimDurationMs_ = findDuration(id);
            return meleeAnimId_;
        }
    }

    const uint32_t avoidIds[] = {anim::STAND, anim::DEATH, anim::WALK, anim::RUN, anim::SHUFFLE_LEFT, anim::SHUFFLE_RIGHT, anim::WALK_BACKWARDS, anim::JUMP_START, anim::JUMP, anim::JUMP_END, anim::SWIM_IDLE, anim::SWIM, anim::SITTING};
    auto isAvoid = [&](uint32_t id) -> bool {
        for (uint32_t avoid : avoidIds) {
            if (id == avoid) return true;
        }
        return false;
    };

    uint32_t bestId = 0;
    uint32_t bestDuration = 0;
    for (const auto& seq : sequences) {
        if (seq.duration == 0) continue;
        if (isAvoid(seq.id)) continue;
        if (seq.movingSpeed > 0.1f) continue;
        if (seq.duration < 150 || seq.duration > 2000) continue;
        if (bestId == 0 || seq.duration < bestDuration) {
            bestId = seq.id;
            bestDuration = seq.duration;
        }
    }

    if (bestId == 0) {
        for (const auto& seq : sequences) {
            if (seq.duration == 0) continue;
            if (isAvoid(seq.id)) continue;
            if (bestId == 0 || seq.duration < bestDuration) {
                bestId = seq.id;
                bestDuration = seq.duration;
            }
        }
    }

    meleeAnimId_ = bestId;
    meleeAnimDurationMs_ = static_cast<float>(bestDuration);
    return meleeAnimId_;
}

// ── Effect triggers ──────────────────────────────────────────────────────────

void AnimationController::triggerLevelUpEffect(const glm::vec3& position) {
    auto* levelUpEffect = renderer_->getLevelUpEffect();
    if (!levelUpEffect) return;

    if (!levelUpEffect->isModelLoaded()) {
        auto* m2Renderer = renderer_->getM2Renderer();
        if (m2Renderer) {
            auto* assetManager = core::Application::getInstance().getAssetManager();
            if (!assetManager) {
                LOG_WARNING("LevelUpEffect: no asset manager available");
            } else {
                auto m2Data = assetManager->readFile("Spells\\LevelUp\\LevelUp.m2");
                auto skinData = assetManager->readFile("Spells\\LevelUp\\LevelUp00.skin");
                LOG_INFO("LevelUpEffect: m2Data=", m2Data.size(), " skinData=", skinData.size());
                if (!m2Data.empty()) {
                    levelUpEffect->loadModel(m2Renderer, m2Data, skinData);
                } else {
                    LOG_WARNING("LevelUpEffect: failed to read Spell\\LevelUp\\LevelUp.m2");
                }
            }
        }
    }

    levelUpEffect->trigger(position);
}

// ── Mount ────────────────────────────────────────────────────────────────────

void AnimationController::setMounted(uint32_t mountInstId, uint32_t mountDisplayId, float heightOffset, const std::string& modelPath) {
    auto* characterRenderer = renderer_->getCharacterRenderer();
    auto* cameraController = renderer_->getCameraController();

    mountInstanceId_ = mountInstId;
    mountHeightOffset_ = heightOffset;
    mountPitch_ = 0.0f;
    mountLocomotion_.reset();
    mountCapsProbed_ = false;
    lastMountAnimRequest_ = UINT32_MAX;
    mountJumpKeyHeld_ = false;
    mountSpecialActive_ = false;

    if (cameraController) {
        cameraController->setMounted(true);
        cameraController->setMountHeightOffset(heightOffset);
    }

    if (characterRenderer && mountInstId > 0) {
        characterRenderer->dumpAnimations(mountInstId);
    }

    // Discover mount animation capabilities (property-based, not hardcoded IDs)
    LOG_DEBUG("=== Mount Animation Dump (Display ID ", mountDisplayId, ") ===");
    if (characterRenderer) characterRenderer->dumpAnimations(mountInstId);

    std::vector<pipeline::M2Sequence> sequences;
    if (!characterRenderer || !characterRenderer->getAnimationSequences(mountInstId, sequences)) {
        LOG_WARNING("Failed to get animation sequences for mount, using fallback IDs");
        sequences.clear();
    }

    auto findFirst = [&](std::initializer_list<uint32_t> candidates) -> uint32_t {
        for (uint32_t id : candidates) {
            if (characterRenderer && characterRenderer->hasAnimation(mountInstId, id)) {
                return id;
            }
        }
        return 0;
    };

    // Property-based jump animation discovery with chain-based scoring
    auto discoverJumpSet = [&]() {
        LOG_DEBUG("=== Full sequence table for mount ===");
        for (const auto& seq : sequences) {
            LOG_DEBUG("SEQ id=", seq.id,
                     " dur=", seq.duration,
                     " flags=0x", std::hex, seq.flags, std::dec,
                     " moveSpd=", seq.movingSpeed,
                     " blend=", seq.blendTime,
                     " next=", seq.nextAnimation,
                     " alias=", seq.aliasNext);
        }
        LOG_DEBUG("=== End sequence table ===");

        std::set<uint32_t> forbiddenIds = {53, 54, 16};

        auto scoreNear = [](int a, int b) -> int {
            int d = std::abs(a - b);
            return (d <= 8) ? (20 - d) : 0;
        };

        auto isForbidden = [&](uint32_t id) {
            return forbiddenIds.count(id) != 0;
        };

        auto findSeqById = [&](uint32_t id) -> const pipeline::M2Sequence* {
            for (const auto& s : sequences) {
                if (s.id == id) return &s;
            }
            return nullptr;
        };

        uint32_t runId = findFirst({anim::RUN, anim::WALK});
        uint32_t standId = findFirst({anim::STAND});

        std::vector<uint32_t> loops;
        for (const auto& seq : sequences) {
            if (isForbidden(seq.id)) continue;
            bool isLoop = (seq.flags & 0x01) == 0;
            if (isLoop && seq.duration >= 350 && seq.duration <= 1000 &&
                seq.id != runId && seq.id != standId) {
                loops.push_back(seq.id);
            }
        }

        uint32_t loop = 0;
        if (!loops.empty()) {
            uint32_t best = loops[0];
            int bestScore = -999;
            for (uint32_t id : loops) {
                int sc = 0;
                sc += scoreNear(static_cast<int>(id), 38);
                const auto* s = findSeqById(id);
                if (s) sc += (s->duration >= 500 && s->duration <= 800) ? 5 : 0;
                if (sc > bestScore) {
                    bestScore = sc;
                    best = id;
                }
            }
            loop = best;
        }

        uint32_t start = 0, end = 0;
        int bestStart = -999, bestEnd = -999;

        for (const auto& seq : sequences) {
            if (isForbidden(seq.id)) continue;
            bool isLoop = (seq.flags & 0x01) == 0;
            if (isLoop) continue;

            if (seq.duration >= 450 && seq.duration <= 1100) {
                int sc = 0;
                if (loop) sc += scoreNear(static_cast<int>(seq.id), static_cast<int>(loop));
                if (loop && (seq.nextAnimation == static_cast<int16_t>(loop) || seq.aliasNext == loop)) sc += 30;
                if (loop && scoreNear(seq.nextAnimation, static_cast<int>(loop)) > 0) sc += 10;
                if (seq.blendTime > 400) sc -= 5;

                if (sc > bestStart) {
                    bestStart = sc;
                    start = seq.id;
                }
            }

            if (seq.duration >= 650 && seq.duration <= 1600) {
                int sc = 0;
                if (loop) sc += scoreNear(static_cast<int>(seq.id), static_cast<int>(loop));
                if (seq.nextAnimation == static_cast<int16_t>(runId) || seq.nextAnimation == static_cast<int16_t>(standId)) sc += 10;
                if (seq.nextAnimation < 0) sc += 5;
                if (sc > bestEnd) {
                    bestEnd = sc;
                    end = seq.id;
                }
            }
        }

        LOG_DEBUG("Property-based jump discovery: start=", start, " loop=", loop, " end=", end,
                 " scores: start=", bestStart, " end=", bestEnd);
        return std::make_tuple(start, loop, end);
    };

    auto [discoveredStart, discoveredLoop, discoveredEnd] = discoverJumpSet();

    // Build MountAnimSet for MountFSM
    MountFSM::MountAnimSet mountAnims;
    mountAnims.jumpStart = discoveredStart > 0 ? discoveredStart : findFirst({anim::FALL, anim::JUMP_START});
    mountAnims.jumpLoop  = discoveredLoop > 0 ? discoveredLoop : findFirst({anim::JUMP});
    mountAnims.jumpEnd   = discoveredEnd > 0 ? discoveredEnd : findFirst({anim::JUMP_END});
    mountAnims.rearUp    = findFirst({anim::MOUNT_SPECIAL, anim::RUN_RIGHT, anim::FALL});
    mountAnims.run       = findFirst({anim::RUN, anim::WALK});
    mountAnims.stand     = findFirst({anim::STAND});
    // Discover flight animations (flying mounts only - may all be 0 for ground mounts)
    //
    // A 3.3.5 flying mount has Fly for moving and Hover for staying put - or
    // Fly for that too, when it has no Hover (the wyvern) - and RunRight and
    // RunLeft for strafing. Those two are the bank it leans into in the air:
    // three or four seconds long, moving nothing, and carried by fliers alone -
    // no ground mount has them - so they are strafing in flight, not on the
    // ground. It has nothing for climbing, diving or backing up, so those stay
    // empty and the FSM falls back.
    mountAnims.flyForward   = findFirst({anim::FLY_FORWARD});
    mountAnims.flyIdle      = findFirst({anim::FLY_IDLE, anim::FLY_FORWARD});
    mountAnims.flyLeft      = findFirst({anim::RUN_LEFT});
    mountAnims.flyRight     = findFirst({anim::RUN_RIGHT});

    // Discover idle fidget animations using proper WoW M2 metadata
    core::Logger::getInstance().debug("Scanning for fidget animations in ", sequences.size(), " sequences");

    core::Logger::getInstance().debug("=== ALL potential fidgets (no metadata filter) ===");
    for (const auto& seq : sequences) {
        bool isLoop = (seq.flags & 0x01) == 0;
        bool isStationary = std::abs(seq.movingSpeed) < 0.05f;
        bool reasonableDuration = seq.duration >= 400 && seq.duration <= 2500;

        if (!isLoop && reasonableDuration && isStationary) {
            core::Logger::getInstance().debug("  ALL: id=", seq.id,
                " dur=", seq.duration, "ms",
                " freq=", seq.frequency,
                " replay=", seq.replayMin, "-", seq.replayMax,
                " flags=0x", std::hex, seq.flags, std::dec,
                " next=", seq.nextAnimation);
        }
    }

    for (const auto& seq : sequences) {
        bool isLoop = (seq.flags & 0x01) == 0;
        bool hasFrequency = seq.frequency > 0;
        bool hasReplay = seq.replayMax > 0;
        bool isStationary = std::abs(seq.movingSpeed) < 0.05f;
        bool reasonableDuration = seq.duration >= 400 && seq.duration <= 2500;

        if (!isLoop && reasonableDuration && isStationary && (hasFrequency || hasReplay)) {
            core::Logger::getInstance().debug("  Candidate: id=", seq.id,
                " dur=", seq.duration, "ms",
                " freq=", seq.frequency,
                " replay=", seq.replayMin, "-", seq.replayMax,
                " next=", seq.nextAnimation,
                " speed=", seq.movingSpeed);
        }

        bool isDeathOrWound = (seq.id >= 5 && seq.id <= 9);
        bool isAttackOrCombat = (seq.id >= 11 && seq.id <= 21);
        bool isSpecial = (seq.id == 2 || seq.id == 3);

        if (!isLoop && (hasFrequency || hasReplay) && isStationary && reasonableDuration &&
            !isDeathOrWound && !isAttackOrCombat && !isSpecial) {
            bool chainsToStand = (seq.nextAnimation == static_cast<int16_t>(mountAnims.stand)) ||
                                 (seq.aliasNext == mountAnims.stand) ||
                                 (seq.nextAnimation == -1);

            // Listed only. A mount idles in its Stand variations (the
            // renderer draws them by M2 frequency); native never plays
            // MountSpecial or other one-shots on an idle mount by itself.
            core::Logger::getInstance().debug("  one-shot (not played idle): id=", seq.id,
                (chainsToStand ? " (chains to stand)" : ""));
        }
    }

    if (mountAnims.run == 0) mountAnims.run = mountAnims.stand;

    core::Logger::getInstance().debug("Mount animation set: jumpStart=", mountAnims.jumpStart,
        " jumpLoop=", mountAnims.jumpLoop,
        " jumpEnd=", mountAnims.jumpEnd,
        " rearUp=", mountAnims.rearUp,
        " run=", mountAnims.run,
        " stand=", mountAnims.stand,
        " fidgets=", mountAnims.fidgets.size());

    // Configure MountFSM via CharacterAnimator.
    //
    // What is passed here is only what was true at this moment. The FSM takes
    // the per-frame answer as well now, and either counts - a taxi sets the
    // mount up before it says the flight has begun, so this one is false on
    // every flight.
    LOG_INFO("Mount configured: displayId=", mountDisplayId, " taxiFlight=", taxiFlight_);
    characterAnimator_.configureMountFSM(mountAnims, taxiFlight_);

    if (renderer_->getAudioCoordinator()->getMountSoundManager()) {
        bool isFlying = taxiFlight_;
        renderer_->getAudioCoordinator()->getMountSoundManager()->onMount(mountDisplayId, isFlying, modelPath);
    }
}

void AnimationController::clearMount() {
    LOG_INFO("Mount cleared: was taxiFlight=", taxiFlight_, " mountInstanceId=", mountInstanceId_);
    mountInstanceId_ = 0;
    mountHeightOffset_ = 0.0f;
    mountPitch_ = 0.0f;
    mountRoll_ = 0.0f;
    if (auto* characterRenderer = renderer_->getCharacterRenderer();
        characterRenderer && characterRenderer->hasInstance(renderer_->getCharacterInstanceId()))
        characterRenderer->setInstanceSeat(renderer_->getCharacterInstanceId(), 0);

    // Clear MountFSM via CharacterAnimator
    characterAnimator_.clearMountFSM();

    if (auto* cameraController = renderer_->getCameraController()) {
        cameraController->setMounted(false);
        cameraController->setMountHeightOffset(0.0f);
    }

    if (renderer_->getAudioCoordinator()->getMountSoundManager()) {
        renderer_->getAudioCoordinator()->getMountSoundManager()->onDismount();
    }
}

// ── Query helpers ────────────────────────────────────────────────────────────

bool AnimationController::isFootstepAnimationState() const {
    auto state = characterAnimator_.getLocomotion().getState();
    return state == LocomotionFSM::State::WALK || state == LocomotionFSM::State::RUN;
}

// ── Melee timers ─────────────────────────────────────────────────────────────

void AnimationController::updateMeleeTimers(float deltaTime) {
    if (meleeSwingCooldown_ > 0.0f) {
        meleeSwingCooldown_ = std::max(0.0f, meleeSwingCooldown_ - deltaTime);
    }
    if (meleeSwingTimer_ > 0.0f) {
        meleeSwingTimer_ = std::max(0.0f, meleeSwingTimer_ - deltaTime);
        if (meleeSwingTimer_ <= 0.0f) {
            specialAttackAnimId_ = 0;
            if (hasPendingMeleeSwing_) {
                const bool nextOffHand = pendingMeleeSwingOffHand_;
                hasPendingMeleeSwing_ = false;
                startLocalMeleeSwing(nextOffHand);
            }
        }
    }
    // Ranged shot timer (same pattern as melee)
    if (rangedShootTimer_ > 0.0f) {
        rangedShootTimer_ = std::max(0.0f, rangedShootTimer_ - deltaTime);
        if (rangedShootTimer_ <= 0.0f && restoreWeaponAfterRangedShot_) {
            restoreWeaponAfterRangedShot_ = false;
            if (rangedShotCompleteCallback_) rangedShotCompleteCallback_();
        }
    }
}

// ── Mount positioning helper ─────────────────────────────────────────────────

void AnimationController::applyMountPositioning(float mountBob, float mountRoll, float characterYaw) {
    auto* characterRenderer = renderer_->getCharacterRenderer();
    uint32_t characterInstanceId = renderer_->getCharacterInstanceId();
    if (!characterRenderer || characterInstanceId == 0) return;

    auto* cameraController = renderer_->getCameraController();
    const bool groundMountStrafe = !taxiFlight_ && cameraController &&
        !cameraController->isFlightAirborne() && !cameraController->isSwimming();
    float mountYawRad = glm::radians(characterYaw) +
        (groundMountStrafe ? renderer_->getCharacterLegYawOffsetRad() : 0.0f);

    // Position mount model
    if (mountInstanceId_ > 0) {
        const glm::vec3& characterPosition = renderer_->getCharacterPosition();
        characterRenderer->setInstancePosition(mountInstanceId_, characterPosition);
        characterRenderer->setInstanceRotation(mountInstanceId_, glm::vec3(mountPitch_, mountRoll, mountYawRad));
    }

    // The rider sits on the mount's MountMain as the mount's pose carries
    // it, as every other rider does (CharacterRenderer::setInstanceSeat,
    // rider_pose.hpp riderSeatPose). A mount whose M2 is still loading
    // answers no for a frame or two; it is asked again every frame.
    if (mountInstanceId_ > 0 && characterRenderer->hasAttachmentPoint(mountInstanceId_, 0)) {
        if (characterRenderer->instanceSeat(characterInstanceId) != mountInstanceId_)
            characterRenderer->setInstanceSeat(characterInstanceId, mountInstanceId_);
        return;
    }

    // No seat on the model: over the mount's origin at the estimated height.
    const glm::vec3& characterPosition = renderer_->getCharacterPosition();
    glm::mat4 mountRotation = glm::mat4(1.0f);
    mountRotation = glm::rotate(mountRotation, mountYawRad, glm::vec3(0.0f, 0.0f, 1.0f));
    mountRotation = glm::rotate(mountRotation, mountRoll, glm::vec3(1.0f, 0.0f, 0.0f));
    mountRotation = glm::rotate(mountRotation, mountPitch_, glm::vec3(0.0f, 1.0f, 0.0f));
    glm::vec3 localOffset(0.0f, 0.0f, mountHeightOffset_ + mountBob);
    glm::vec3 worldOffset = glm::vec3(mountRotation * glm::vec4(localOffset, 0.0f));
    characterRenderer->setInstancePosition(characterInstanceId, characterPosition + worldOffset);
    characterRenderer->setInstanceRotation(characterInstanceId, glm::vec3(mountPitch_, mountRoll, mountYawRad));
}

// ── Mounted animation update (uses MountFSM) ────────────────────────────────

void AnimationController::applyRiderSeatAnimation() {
    // The rider sits in Mount, in the air as on the ground: 2.4.3 has no
    // flight poses for a rider.
    auto* characterRenderer = renderer_->getCharacterRenderer();
    const uint32_t characterInstanceId = renderer_->getCharacterInstanceId();
    const uint32_t riderAnim = anim::MOUNT;
    uint32_t currentAnimId = 0;
    float currentAnimTimeMs = 0.0f, currentAnimDurationMs = 0.0f;
    const bool haveState = characterRenderer->getAnimationState(characterInstanceId, currentAnimId,
                                                                currentAnimTimeMs, currentAnimDurationMs);
    if (!haveState || currentAnimId != riderAnim) {
        characterRenderer->playAnimation(characterInstanceId, riderAnim, true);
        lastPlayerAnimRequest_ = riderAnim;
        lastPlayerAnimLoopRequest_ = true;
    }
}

// On the ground and in water a mount moves as its rider would on foot: the
// player's own LocomotionFSM picks Stand, Walk, Run, Walkbackwards, the turn
// shuffles, JumpStart/Jump/JumpEnd, Fall, SwimIdle and Swim from the mount
// model's sequences. Native 2.4.3, Brown Horse (RidingHorse.m2): backing up
// plays Walkbackwards, not the gallop; a running jump leaves the ground in
// its jump. Jump pressed standing still rears the mount (MountSpecial, 94)
// and does not jump; it is sent as CMSG_MOUNTSPECIAL_ANIM for others to see.
// No procedural bob or roll: native moves the mount by its animation alone.
void AnimationController::updateMountedGroundAnimation(float deltaTime, int turnDirection) {
    auto* cr = renderer_->getCharacterRenderer();
    auto* cc = renderer_->getCameraController();
    if (mountInstanceId_ == 0 || !cr || !cc || !cr->hasInstance(mountInstanceId_)) return;
    if (!mountCapsProbed_) {
        // A model still loading has no sequences yet; ask again next frame.
        std::vector<pipeline::M2Sequence> sequences;
        if (!cr->getAnimationSequences(mountInstanceId_, sequences) || sequences.empty()) return;
        mountCaps_ = AnimCapabilityProbe::probe(renderer_, mountInstanceId_);
        mountCapsProbed_ = true;
    }
    LocomotionFSM::Input in;
    in.moving = cc->isMoving();
    in.sprinting = cc->isSprinting();
    in.movingForward = cc->isMovingForward();
    in.movingBackward = cc->isMovingBackward();
    in.strafeLeft = cc->isStrafingLeft();
    in.strafeRight = cc->isStrafingRight();
    in.grounded = cc->isGrounded();
    in.jumping = cc->isJumping();
    in.swimming = cc->isSwimming();
    in.turnDirection = turnDirection;
    in.deltaTime = deltaTime;
    in.haveAnimState = cr->getAnimationState(mountInstanceId_, in.currentAnimId,
                                             in.currentAnimTime, in.currentAnimDuration);
    const auto before = mountLocomotion_.getState();
    const AnimOutput out = mountLocomotion_.resolve(in, mountCaps_);
    const auto after = mountLocomotion_.getState();
    using S = LocomotionFSM::State;
    const auto airborne = [](S st) { return st == S::JUMP_START || st == S::JUMP_MID; };
    if (auto* sounds = renderer_->getAudioCoordinator()->getMountSoundManager()) {
        if (after == S::JUMP_START && before != S::JUMP_START) sounds->playJumpSound();
        if (airborne(before) && !airborne(after) && in.grounded) sounds->playLandSound();
    }

    const bool jumpKey = cc->isJumpKeyPressed();
    if (jumpKey && !mountJumpKeyHeld_ && in.grounded && !in.moving && !in.swimming &&
        cr->hasAnimation(mountInstanceId_, anim::MOUNT_SPECIAL)) {
        cr->playAnimation(mountInstanceId_, anim::MOUNT_SPECIAL, false, 0, true);
        mountSpecialActive_ = true;
        if (auto* gh = core::Application::getInstance().getGameHandler()) gh->sendMountSpecialAnim();
        mountJumpKeyHeld_ = jumpKey;
        return;
    }
    mountJumpKeyHeld_ = jumpKey;
    if (mountSpecialActive_) {
        const bool finished = !in.haveAnimState || in.currentAnimId != anim::MOUNT_SPECIAL ||
                              in.currentAnimTime >= in.currentAnimDuration;
        if (!finished && !in.moving && in.grounded && !in.swimming) return;
        mountSpecialActive_ = false;
        lastMountAnimRequest_ = UINT32_MAX;
    }
    if (!out.valid) return;
    const bool drifted = in.haveAnimState && out.loop && in.currentAnimId != out.animId;
    if (out.animId != lastMountAnimRequest_ || out.loop != lastMountLoopRequest_ || drifted) {
        cr->playAnimation(mountInstanceId_, out.animId, out.loop, 0, !out.loop);
        lastMountAnimRequest_ = out.animId;
        lastMountLoopRequest_ = out.loop;
    }
}

void AnimationController::updateMountedAnimation(float deltaTime, int turnDirection) {
    auto* characterRenderer = renderer_->getCharacterRenderer();
    auto* cameraController = renderer_->getCameraController();
    float characterYaw = renderer_->getCharacterYaw();
    if (!taxiFlight_ && !cameraController->isFlightAirborne()) {
        updateMountedGroundAnimation(deltaTime, turnDirection);
        applyRiderSeatAnimation();
        applyMountPositioning(0.0f, 0.0f, characterYaw);
        return;
    }
    // In the air (flying mount) or on a taxi: MountFSM's flight animations.
    mountLocomotion_.reset();
    lastMountAnimRequest_ = UINT32_MAX;
    mountSpecialActive_ = false;
    auto& mountFSM = characterAnimator_.getMountFSM();

    // Build MountFSM input
    MountFSM::Input mountIn;
    mountIn.moving = cameraController->isMoving();
    mountIn.movingBackward = cameraController->isMovingBackward();
    mountIn.strafeLeft = cameraController->isStrafingLeft();
    mountIn.strafeRight = cameraController->isStrafingRight();
    mountIn.grounded = cameraController->isGrounded();
    mountIn.jumpKeyPressed = cameraController->isJumpKeyPressed();
    // In the air, not allowed to be: on the ground a flying mount runs.
    mountIn.flying = cameraController->isFlightAirborne();
    mountIn.swimming = cameraController->isSwimming();
    mountIn.ascending = cameraController->isAscending();
    mountIn.descending = cameraController->isDescending();
    mountIn.taxiFlight = taxiFlight_;
    mountIn.deltaTime = deltaTime;
    mountIn.characterYaw = characterYaw;
    // Mount animation state query
    if (mountInstanceId_ > 0 && characterRenderer) {
        mountIn.haveMountState = characterRenderer->getAnimationState(
            mountInstanceId_, mountIn.curMountAnim, mountIn.curMountTime, mountIn.curMountDuration);
    }

    // Evaluate MountFSM
    auto mountOut = mountFSM.evaluate(mountIn);

    // Apply mount animation if changed
    if (mountOut.mountAnimChanged && mountInstanceId_ > 0 && characterRenderer) {
        characterRenderer->playAnimation(mountInstanceId_, mountOut.mountAnimId, mountOut.mountAnimLoop);
    }

    applyRiderSeatAnimation();

    // Handle mount sounds
    auto* mountSoundMgr = renderer_->getAudioCoordinator()->getMountSoundManager();
    if (mountOut.playJumpSound && mountSoundMgr) {
        mountSoundMgr->playJumpSound();
    }
    if (mountOut.playLandSound && mountSoundMgr) {
        mountSoundMgr->playLandSound();
    }
    if (mountOut.playRearUpSound && mountSoundMgr) {
        mountSoundMgr->playRearUpSound();
    }
    if (mountOut.playIdleSound && mountSoundMgr) {
        mountSoundMgr->playIdleSound();
    }

    // Apply positioning (uses mountBob and mountRoll from MountFSM)
    // For taxi flights, use external mountRoll_ set by setMountPitchRoll
    // For ground mounts, use MountFSM's computed lean roll
    float finalRoll = taxiFlight_ ? mountRoll_ : mountOut.mountRoll;
    applyMountPositioning(mountOut.mountBob, finalRoll, characterYaw);
}

// ── Character animation state machine (delegates to CharacterAnimator) ──────────

void AnimationController::updateCharacterAnimation() {
    auto* characterRenderer = renderer_->getCharacterRenderer();
    auto* cameraController = renderer_->getCameraController();
    uint32_t characterInstanceId = renderer_->getCharacterInstanceId();
    const auto* handler = core::Application::getInstance().getGameHandler();
    const bool dead = handler && handler->isPlayerDead() && !handler->isPlayerGhost();

    // Lazy probe: populate capability set once per model.
    // Re-probe if melee capabilities are missing (model may not have been fully
    // loaded on the first probe attempt).
    if (characterRenderer && characterInstanceId != 0) {
        if (!capabilitiesProbed_) {
            probeCapabilities();
        } else if (meleeSwingTimer_ > 0.0f && !characterAnimator_.getCapabilities().hasMelee) {
            capabilitiesProbed_ = false;
            probeCapabilities();
        }
    }

    // Turning in place: the player turned the character's facing since the
    // last update, with the turn keys or by mouse-look. Build 8606 shuffles
    // its standing character's feet for both, toward the turn.
    const float yaw = renderer_->getCharacterYaw();
    int turnDirection = 0;
    if (lastAnimatedYaw_ && (cameraController->isTurningLeft() ||
                             cameraController->isTurningRight() ||
                             cameraController->isRightMouseHeld())) {
        float delta = yaw - *lastAnimatedYaw_;
        while (delta > 180.0f) delta -= 360.0f;
        while (delta < -180.0f) delta += 360.0f;
        if (delta != 0.0f) turnDirection = delta > 0.0f ? 1 : -1;
    }
    lastAnimatedYaw_ = yaw;

    // When mounted, delegate to MountFSM and handle positioning
    if (!dead && isMounted()) {
        updateMountedAnimation(lastDeltaTime_, turnDirection);
        return;
    }

    // Build FrameInput for CharacterAnimator from camera/renderer state
    CharacterAnimator::FrameInput fi;
    fi.dead = dead;
    if (characterInstanceId != deathTrackedInstance_) {
        deathTrackedInstance_ = characterInstanceId;
        seenAliveOnInstance_ = false;
    }
    // Alive only once the server's own unit says so: the instance is drawn
    // before the player's fields arrive, and a player who logs in dead must
    // not count as seen alive in those first frames.
    if (!dead && handler) {
        const auto self = std::dynamic_pointer_cast<game::Unit>(
            handler->getEntityManager().getEntity(handler->getPlayerGuid()));
        if (self && self->getHealth() > 0) seenAliveOnInstance_ = true;
    }
    fi.diedInView = dead && seenAliveOnInstance_;
    fi.moving = cameraController->isMoving();
    fi.sprinting = cameraController->isSprinting();
    fi.movingForward = cameraController->isMovingForward();
    fi.movingBackward = cameraController->isMovingBackward();
    fi.strafeLeft = cameraController->isStrafingLeft();
    fi.strafeRight = cameraController->isStrafingRight();
    // See setM2TransportRiding() comment: real physics correctly reports "not grounded"
    // over open track under a moving platform, but the animation FSM needs "grounded"
    // to avoid playing a falling animation for the whole ride.
    fi.grounded = m2TransportRiding_ ? true : cameraController->isGrounded();
    fi.jumping = cameraController->isJumping();
    fi.swimming = cameraController->isSwimming();
    fi.sitting = cameraController->isSitting();
    fi.ascending = cameraController->isAscending();
    fi.descending = cameraController->isDescending();
    fi.jumpKeyPressed = cameraController->isJumpKeyPressed();
    fi.characterYaw = yaw;
    fi.turnDirection = turnDirection;
    // Melee/ranged timers
    fi.meleeSwingTimer = meleeSwingTimer_;
    fi.meleeSwingOffHand = meleeSwingOffHand_;
    fi.meleeSwingSerial = meleeSwingSerial_;
    fi.rangedShootTimer = rangedShootTimer_;
    fi.specialAttackAnimId = specialAttackAnimId_;
    fi.rangedAnimId = rangedAnimId_;
    // Animation state query for one-shot completion detection
    if (characterRenderer && characterInstanceId > 0) {
        fi.haveAnimState = characterRenderer->getAnimationState(
            characterInstanceId, fi.currentAnimId, fi.currentAnimTime, fi.currentAnimDuration);
    }

    // Inject FrameInput and resolve animation via CharacterAnimator
    characterAnimator_.setFrameInput(fi);
    characterAnimator_.update(lastDeltaTime_);

    // Read the resolved animation output
    AnimOutput output = characterAnimator_.getLastOutput();

    // STAY policy: if CharacterAnimator returns invalid, keep current animation
    if (!output.valid) return;

    uint32_t animId = output.animId;
    bool loop = output.loop;

    // Apply animation to the character renderer
    uint32_t currentAnimId = 0;
    float currentAnimTimeMs = 0.0f;
    float currentAnimDurationMs = 0.0f;
    bool haveState = characterRenderer->getAnimationState(characterInstanceId, currentAnimId, currentAnimTimeMs, currentAnimDurationMs);
    const bool requestChanged = (lastPlayerAnimRequest_ != animId) || (lastPlayerAnimLoopRequest_ != loop);
    // Only re-assert looping animations if the renderer drifted (e.g., external
    // playAnimation call).  One-shot animations must NOT be re-asserted after the
    // animation reaches its end - the FSM consumes the completed duration and
    // chooses its successor while the renderer holds the final pose.
    const bool drifted = haveState && currentAnimId != animId && (loop || dead);
    const bool shouldPlay = requestChanged || drifted;

    // Debug: log animation decisions (only when animation changes or replays)
    static uint32_t dbgLastAnim = UINT32_MAX;
    if (shouldPlay || animId != dbgLastAnim) {
        LOG_DEBUG("[AnimDbg] FSM→", animId, " loop=", loop,
                  " cur=", currentAnimId, " t=", currentAnimTimeMs, "/", currentAnimDurationMs,
                  " haveState=", haveState,
                  " reqChanged=", requestChanged, " drifted=", drifted, " shouldPlay=", shouldPlay,
                  " lastReq=", lastPlayerAnimRequest_,
                  " locoState=", static_cast<int>(characterAnimator_.getLocomotion().getState()),
                  " actState=", static_cast<int>(characterAnimator_.getActivity().getState()),
                  " fwd=", fi.movingForward, " back=", fi.movingBackward,
                  " strafeL=", fi.strafeLeft, " strafeR=", fi.strafeRight,
                  " moving=", fi.moving, " sprinting=", fi.sprinting);
        dbgLastAnim = animId;
    }

    characterRenderer->setLocomotionSpeed(characterInstanceId,
        cameraController->getGroundLocomotionSpeed());

    if (shouldPlay) {
        characterRenderer->playAnimation(characterInstanceId, animId, loop, 0, true);
        lastPlayerAnimRequest_ = animId;
        lastPlayerAnimLoopRequest_ = loop;
    }

}

// ── Footstep update (delegated to FootstepDriver) ───────────────────────────

void AnimationController::updateFootsteps(float deltaTime) {
    footstepDriver_.update(deltaTime, renderer_, isMounted(), mountInstanceId_,
                            taxiFlight_, isFootstepAnimationState());
}

// ── Activity SFX state tracking ──────────────────────────────────────────────

void AnimationController::updateSfxState(float deltaTime) {
    sfxStateDriver_.update(deltaTime, renderer_, isMounted(), taxiFlight_,
                           footstepDriver_);
}

} // namespace rendering
} // namespace wowee
