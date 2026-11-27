#include "game/combat_handler.hpp"
#include "game/combat_text_update.hpp"
#include "game/spell_handler.hpp"
#include "game/combat_log_event.hpp"
#include "game/classic_combat_chat.hpp"
#include "audio/ui_sound_manager.hpp"
#include "addons/lua_api_registrations.hpp"
#include "game/combat_text_filter.hpp"
#include "ui/framexml_takeover.hpp"
#include "game/game_handler.hpp"
#include "game/game_utils.hpp"
#include "game/packet_parsers.hpp"
#include "game/entity.hpp"
#include "game/tab_target.hpp"
#include "game/update_field_table.hpp"
#include "game/opcode_table.hpp"
#include "rendering/renderer.hpp"
#include "rendering/camera.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/combat_sound_manager.hpp"
#include "audio/activity_sound_manager.hpp"
#include "audio/npc_voice_manager.hpp"
#include "audio/sound_entries.hpp"
#include "core/application.hpp"
#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include "network/world_socket.hpp"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <ctime>
#include <random>

namespace wowee {
namespace game {

namespace {
/// Who a unit is to the player, in the terms 1.12 splits its combat chat by.
/// Another player counts as friendly unless the client knows otherwise, as
/// the combat log event's flags take them.
CombatRelation combatRelationOf(GameHandler& gh, uint64_t guid) {
    if (guid == 0) return CombatRelation::Creature;
    if (guid == gh.getPlayerGuid()) return CombatRelation::Self;
    if (guid == gh.getPetGuid()) return CombatRelation::Pet;
    if (gh.isInGroup()) {
        for (const auto& m : gh.getPartyData().members) {
            if (m.guid == guid) return CombatRelation::Party;
        }
    }
    if ((guid >> 48) != 0) return CombatRelation::Creature;
    const Unit* unit = gh.getUnitByGuid(guid);
    return (unit && gh.unitReactionToPlayer(*unit) <= 3)
        ? CombatRelation::HostilePlayer : CombatRelation::FriendlyPlayer;
}
}  // namespace

CombatHandler::CombatHandler(GameHandler& owner)
    : owner_(owner) {}

/// The same rule the threat indicator uses, kept in step with it.
///
/// threatWarning is 0 never, 1 in instances, 2 in a party, 3 always - and
/// IsThreatWarningEnabled answers the interface from exactly that. Reading it
/// here rather than inventing a second rule is what stops the sound and the
/// picture disagreeing about when a warning is wanted.
bool CombatHandler::threatWarningWanted() const {
    int setting = 3;
    const std::string stored = addons::storedCVarValue("threatWarning", "3");
    try { setting = std::stoi(stored); } catch (const std::exception&) {}
    switch (setting) {
        case 1:  return owner_.isInInstance();
        case 2:  return !owner_.getPartyData().members.empty();
        case 3:  return true;
        default: return false;   // 0, and anything unrecognised
    }
}

void CombatHandler::registerOpcodes(DispatchTable& table) {
    // ---- Combat clearing ----
    table[Opcode::SMSG_ATTACKSWING_DEADTARGET] = [this](network::Packet& /*packet*/) {
        stopAutoAttack();
    };
    table[Opcode::SMSG_THREAT_CLEAR] = [this](network::Packet& /*packet*/) {
        threatLists_.clear();
        if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("UNIT_THREAT_LIST_UPDATE", {});
    };
    table[Opcode::SMSG_THREAT_REMOVE] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(1)) return;
        uint64_t unitGuid   = packet.readPackedGuid();
        if (!packet.hasRemaining(1)) return;
        uint64_t victimGuid = packet.readPackedGuid();
        auto it = threatLists_.find(unitGuid);
        if (it != threatLists_.end()) {
            auto& list = it->second;
            list.erase(std::remove_if(list.begin(), list.end(),
                [victimGuid](const ThreatEntry& e){ return e.victimGuid == victimGuid; }),
                list.end());
            if (list.empty()) threatLists_.erase(it);
        }
    };
    table[Opcode::SMSG_CANCEL_COMBAT] = [this](network::Packet& /*packet*/) {
        autoAttacking_ = false;
        autoAttackTarget_ = 0;
        autoAttackRequested_ = false;
        hostileAttackers_.clear();
        announceLeaveCombat();
    };

    // ---- Attack/combat delegates ----
    table[Opcode::SMSG_ATTACKSTART] = [this](network::Packet& packet) { handleAttackStart(packet); };
    table[Opcode::SMSG_ATTACKSTOP] = [this](network::Packet& packet) { handleAttackStop(packet); };
    table[Opcode::SMSG_ATTACKSWING_NOTINRANGE] = [this](network::Packet& /*packet*/) {
        // Said every time: CMaNGOS sends it once per change of swing error,
        // and the original shows it even right after its own (startAutoAttack).
        autoAttackOutOfRange_ = true;
        owner_.errorGlobal("ERR_BADATTACKPOS");
        autoAttackRangeRepeat_ = kSwingRangeRepeatSeconds;
    };
    table[Opcode::SMSG_ATTACKSWING_BADFACING] = [this](network::Packet& /*packet*/) {
        autoAttackOutOfRange_ = false;
        // Reported, not corrected. This used to turn the character to face the
        // target, which is not something the server may do to a player: the
        // facing is the player's. Running away from something with auto-attack
        // still on had the server say "not facing" every swing timer and the
        // character spin round to face it each time, fighting the player for
        // control of their own heading.
        //
        // The real client says so and leaves the heading alone - the swing
        // simply does not land until the player turns back.
        if (autoAttackRangeWarnCooldown_ <= 0.0f) {
            owner_.errorGlobal("ERR_BADATTACKFACING");
            autoAttackRangeWarnCooldown_ = 1.25f;
        }
    };
    table[Opcode::SMSG_ATTACKSWING_NOTSTANDING] = [this](network::Packet& /*packet*/) {
        autoAttackOutOfRange_ = false;
        if (autoAttackRangeWarnCooldown_ <= 0.0f) {
            owner_.errorGlobal("ERR_CANTATTACK_NOTSTANDING");
            autoAttackRangeWarnCooldown_ = 1.25f;
        }
    };
    table[Opcode::SMSG_ATTACKSWING_CANT_ATTACK] = [this](network::Packet& /*packet*/) {
        stopAutoAttack();
        if (autoAttackRangeWarnCooldown_ <= 0.0f) {
            owner_.errorGlobal("ERR_INVALID_ATTACK_TARGET");
            autoAttackRangeWarnCooldown_ = 1.25f;
        }
    };
    table[Opcode::SMSG_ATTACKERSTATEUPDATE] = [this](network::Packet& packet) { handleAttackerStateUpdate(packet); };
    table[Opcode::SMSG_AI_REACTION] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(12)) return;
        uint64_t guid = packet.readUInt64();
        uint32_t reaction = packet.readUInt32();
        if (reaction == 2 && owner_.npcAggroCallbackRef()) {
            auto entity = owner_.getEntityManager().getEntity(guid);
            if (entity)
                owner_.npcAggroCallbackRef()(guid, glm::vec3(entity->getX(), entity->getY(), entity->getZ()));
        }
    };
    table[Opcode::SMSG_SPELLNONMELEEDAMAGELOG] = [this](network::Packet& packet) { handleSpellDamageLog(packet); };
    table[Opcode::SMSG_SPELLHEALLOG] = [this](network::Packet& packet) { handleSpellHealLog(packet); };

    // ---- Environmental damage ----
    table[Opcode::SMSG_ENVIRONMENTAL_DAMAGE_LOG] = [this](network::Packet& packet) {
        // uint64 victimGuid + uint8 envDmgType + uint32 damage + uint32 absorbed + uint32 resisted
        // envDmgType: 0=Exhausted(fatigue), 1=Drowning, 2=Fall, 3=Lava, 4=Slime, 5=Fire
        if (!packet.hasRemaining(21)) { packet.skipAll(); return; }
        uint64_t victimGuid  = packet.readUInt64();
        uint8_t envType      = packet.readUInt8();
        uint32_t dmg         = packet.readUInt32();
        uint32_t envAbs      = packet.readUInt32();
        uint32_t envRes      = packet.readUInt32();
        const uint32_t schoolMask = environmentalDamageSchoolMask(envType);
        if (dmg > 0 || envAbs > 0 || envRes > 0) {
            fireUnitCombat(victimGuid, "WOUND",
                           dmg > 0 ? "" : (envAbs > 0 ? "ABSORB" : "RESIST"),
                           static_cast<int32_t>(dmg), schoolMask);
        }
        if (victimGuid == owner_.getPlayerGuid()) {
            // Environmental damage: pass envType via powerType field for display differentiation
            if (dmg > 0)
                addCombatText(CombatTextEntry::ENVIRONMENTAL, static_cast<int32_t>(dmg), 0, false, envType, 0, victimGuid, schoolMask, false, false);
            if (envAbs > 0)
                addCombatText(CombatTextEntry::ABSORB, static_cast<int32_t>(envAbs), 0, false, 0, 0, victimGuid, schoolMask, dmg > 0, false);
            if (envRes > 0 && (dmg > 0 || envAbs == 0))
                addCombatText(CombatTextEntry::RESIST, static_cast<int32_t>(envRes), 0, false, 0, 0, victimGuid, schoolMask, dmg > 0, false);
            // Drowning damage -> play DROWN one-shot on player
            if (envType == 1 && dmg > 0 && owner_.emoteAnimCallbackRef())
                owner_.emoteAnimCallbackRef()(victimGuid, 131, /*isState=*/false, 0); // anim::DROWN
        }
        packet.skipAll();
    };

    // ---- Threat updates ----
    // The two do NOT share a format, though this read them as if they did.
    // Unit::SendThreatListUpdate writes the unit's packed guid and then the
    // count; SendChangeCurrentVictimOpcode writes the unit, *then the new
    // highest-threat target's packed guid*, and then the count. Reading the
    // second guid from both meant the plain update consumed its count as a
    // guid and then read the first victim's guid as the count, so the common
    // one of the two - the one sent on every threat change - produced a list
    // built from nothing.
    for (auto op : {Opcode::SMSG_HIGHEST_THREAT_UPDATE,
                    Opcode::SMSG_THREAT_UPDATE}) {
        const bool hasVictimGuid = (op == Opcode::SMSG_HIGHEST_THREAT_UPDATE);
        table[op] = [this, hasVictimGuid](network::Packet& packet) {
            if (!packet.hasRemaining(1)) return;
            uint64_t unitGuid = packet.readPackedGuid();
            if (hasVictimGuid) {
                if (!packet.hasRemaining(1)) return;
                (void)packet.readPackedGuid();  // the new highest-threat target
            }
            if (!packet.hasRemaining(4)) return;
            uint32_t cnt = packet.readUInt32();
            if (cnt > 100) { packet.skipAll(); return; } // sanity
            std::vector<ThreatEntry> list;
            list.reserve(cnt);
            for (uint32_t i = 0; i < cnt; ++i) {
                if (!packet.hasRemaining(1)) return;
                ThreatEntry entry;
                entry.victimGuid = packet.readPackedGuid();
                if (!packet.hasRemaining(4)) return;
                entry.threat = packet.readUInt32();
                list.push_back(entry);
            }
            // Sort descending by threat so highest is first
            std::sort(list.begin(), list.end(),
                [](const ThreatEntry& a, const ThreatEntry& b){ return a.threat > b.threat; });
            // Play Aggro Sounds, which the panel offers and nothing read -
            // here or in the interface. The threat indicator FrameXML draws is
            // silent by design; in the real client the warning noise is the
            // client's, which is why nothing in this tree ever made it.
            //
            // The sound marks the moment the player becomes the one being
            // fought, not every update while they are: the list arrives on
            // every threat change and a warning per packet would be a siren.
            const bool topNow = !list.empty() &&
                                list.front().victimGuid == owner_.getPlayerGuid();
            const bool topBefore = playerTopThreatOn_.count(unitGuid) != 0;
            if (topNow != topBefore) {
                if (topNow) playerTopThreatOn_.insert(unitGuid);
                else        playerTopThreatOn_.erase(unitGuid);
                if (topNow && threatWarningWanted() &&
                    addons::storedCVarValue("threatPlaySounds", "0") != "0") {
                    if (auto* ac = owner_.services().audioCoordinator) {
                        if (auto* sfx = ac->getUiSoundManager()) sfx->playError();
                    }
                }
            }

            threatLists_[unitGuid] = std::move(list);
            if (!owner_.addonEventCallbackRef()) return;
            auto fire = owner_.addonEventCallbackRef();
            // The list changed, and separately: whether each unit on it is
            // tanking. They are different events and unit frames read the
            // second one - UnitFrameThreatIndicator_OnEvent is registered for
            // UNIT_THREAT_SITUATION_UPDATE alone, and it is what puts the aggro
            // border on a frame. It was never fired, so no frame ever showed
            // one, and the list it would have been read from was misparsed
            // anyway.
            fire("UNIT_THREAT_LIST_UPDATE", {owner_.guidToUnitId(unitGuid)});
            // Named per unit, because the indicator compares the argument
            // against its own frame's unit and ignores anything else. Every
            // unit on the list that the interface has a token for, plus the mob
            // itself - a target frame's indicator is fed by the player's
            // situation against the target, so both ends have to be told.
            for (const ThreatEntry& e : threatLists_[unitGuid]) {
                const std::string who = owner_.guidToUnitId(e.victimGuid);
                if (!who.empty()) fire("UNIT_THREAT_SITUATION_UPDATE", {who});
            }
            if (const std::string mob = owner_.guidToUnitId(unitGuid); !mob.empty())
                fire("UNIT_THREAT_SITUATION_UPDATE", {mob});
        };
    }

    // ---- Forced faction reactions ----
    table[Opcode::SMSG_SET_FORCED_REACTIONS] = [this](network::Packet& packet) { handleSetForcedReactions(packet); };

    // ---- Entity delta updates: health / power / combo / PvP / proc ----
    table[Opcode::SMSG_HEALTH_UPDATE] = [this](network::Packet& p) { handleHealthUpdate(p); };
    table[Opcode::SMSG_POWER_UPDATE] = [this](network::Packet& p) { handlePowerUpdate(p); };
    table[Opcode::SMSG_UPDATE_COMBO_POINTS] = [this](network::Packet& p) { handleUpdateComboPoints(p); };
    table[Opcode::SMSG_PVP_CREDIT] = [this](network::Packet& p) { handlePvpCredit(p); };
    table[Opcode::SMSG_PROCRESIST] = [this](network::Packet& p) { handleProcResist(p); };

    // SMSG_ENVIRONMENTALDAMAGELOG is an alias for SMSG_ENVIRONMENTAL_DAMAGE_LOG
    // (registered above at line 108 with envType forwarding). No separate handler needed.
    table[Opcode::SMSG_SPELLDAMAGESHIELD] = [this](network::Packet& p) { handleSpellDamageShield(p); };
    table[Opcode::SMSG_SPELLORDAMAGE_IMMUNE] = [this](network::Packet& p) { handleSpellOrDamageImmune(p); };
    table[Opcode::SMSG_RESISTLOG] = [this](network::Packet& p) { handleResistLog(p); };

    // ---- Pet feedback ----
    table[Opcode::SMSG_PET_TAME_FAILURE] = [this](network::Packet& p) { handlePetTameFailure(p); };
    table[Opcode::SMSG_PET_ACTION_FEEDBACK] = [this](network::Packet& p) { handlePetActionFeedback(p); };
    table[Opcode::SMSG_PET_CAST_FAILED] = [this](network::Packet& p) { handlePetCastFailed(p); };
    table[Opcode::SMSG_PET_BROKEN] = [this](network::Packet& p) { handlePetBroken(p); };
    table[Opcode::SMSG_PET_LEARNED_SPELL] = [this](network::Packet& p) { handlePetLearnedSpell(p); };
    table[Opcode::SMSG_PET_UNLEARNED_SPELL] = [this](network::Packet& p) { handlePetUnlearnedSpell(p); };
    table[Opcode::SMSG_PET_MODE] = [this](network::Packet& p) { handlePetMode(p); };
    table[Opcode::SMSG_PET_ACTION_SOUND] = [this](network::Packet& p) { handlePetActionSound(p); };
    table[Opcode::SMSG_PET_DISMISS_SOUND] = [this](network::Packet& p) { handlePetDismissSound(p); };

    // ---- Resurrect ----
    table[Opcode::SMSG_RESURRECT_FAILED] = [this](network::Packet& p) { handleResurrectFailed(p); };
}

// ============================================================
// Auto-attack
// ============================================================

bool CombatHandler::canUseAutoRangedCombat() const {
    if (owner_.isPlayerDead() || owner_.isPlayerGhost()) return false;
    if (addons::storedCVarValue("autoRangedCombat", "1") == "0") return false;
    auto* sh = owner_.getSpellHandler();
    if (!sh || !sh->hasKnownSpell(75u)) return false;
    const auto& ranged = owner_.getInventory().getEquipSlot(EquipSlot::RANGED);
    if (ranged.empty()) return false;
    return ranged.item.inventoryType == InvType::RANGED_BOW ||
           isWeaponSubclass(ranged.item.itemClass, ranged.item.subClass, WEAPON_SUBCLASS_BOW) ||
           isWeaponSubclass(ranged.item.itemClass, ranged.item.subClass, WEAPON_SUBCLASS_GUN) ||
           isWeaponSubclass(ranged.item.itemClass, ranged.item.subClass, WEAPON_SUBCLASS_CROSSBOW);
}

void CombatHandler::startAutoAttack(uint64_t targetGuid, bool forceMelee) {
    // Can't attack yourself or while pacified (UNIT_FLAG_PACIFIED)
    if (targetGuid == owner_.getPlayerGuid()) return;
    if (targetGuid == 0) return;
    if (owner_.isPlayerPacified()) return;

    // Assist Attack: swinging at a friendly unit attacks whatever they are
    // fighting instead. The panel offers this and nothing read it, so an
    // attack on an ally was simply sent and refused by the server - a tank
    // pressing attack with a healer selected got nothing and no reason for it.
    //
    // Only when the friend has a target of their own, and only when that
    // target is hostile: assisting somebody who is fighting nothing, or who
    // has a friendly unit selected, would turn one refusal into another.
    if (addons::storedCVarValue("assistAttack", "0") != "0") {
        auto entity = owner_.getEntityManager().getEntity(targetGuid);
        if (auto* unit = dynamic_cast<Unit*>(entity.get())) {
            if (!owner_.canPlayerAttackUnit(*unit) && !isAggressiveTowardPlayer(targetGuid)) {
                const uint64_t theirTarget = unitTargetGuid(entity);
                auto their = owner_.getEntityManager().getEntity(theirTarget);
                auto* theirUnit = dynamic_cast<Unit*>(their.get());
                if (theirUnit && (theirUnit->isHostile() ||
                                  isAggressiveTowardPlayer(theirTarget))) {
                    targetGuid = theirTarget;
                } else {
                    return;   // nothing of theirs worth swinging at
                }
            }
        }
    }

    // Resolve assist first, then reject friendly targets before range checks
    // or local attack intent. A guard without gossip flags is still friendly.
    if (auto entity = owner_.getEntityManager().getEntity(targetGuid)) {
        if (auto* unit = dynamic_cast<Unit*>(entity.get());
            unit && owner_.unitReactionToPlayer(*unit) > 4) {
            return;
        }
    }

    // Range errors belong to the server. Attack intent remains armed while
    // approaching a target, so entering melee range needs no second click.
    if (owner_.isMounted()) {
        if (owner_.isPlayerFlying() && !owner_.isTaxiMountActive() &&
            addons::storedCVarValue("autoDismountFlying", "0") == "0") {
            owner_.addUIError("You can't do that while flying.");
            return;
        }
        owner_.dismount();
    }

    // Native TBC 2.4.3 CVar "autoRangedCombat" (default "1"): initiating attack
    // (right-click, /startattack, Attack button) on an enemy outside melee reach
    // with Auto Shot (75) and a bow/gun/crossbow starts Auto Shot instead of melee.
    if (!forceMelee && canUseAutoRangedCombat() && owner_.unitWithinMeleeReach(targetGuid) == false) {
        const auto bowRange = owner_.unitSpellRange(75u, targetGuid);
        if (!bowRange.has_value() || *bowRange != spellclass::SpellRangeCheck::TooClose) {
            if (owner_.isLootWindowOpen()) owner_.closeLoot();
            stopMeleeAutoAttackOnly();
            auto* sh = owner_.getSpellHandler();
            if (sh && sh->getAutoRepeatSpellId() != 75u) {
                sh->castSpell(75u, targetGuid);
                autoRangedSwitchCooldown_ = 0.35f;
            }
            return;
        }
    }

    if (auto* sh = owner_.getSpellHandler()) {
        sh->clearStunSuspendedAutoRepeat();
        if (sh->getAutoRepeatSpellId() == 75u) {
            sh->cancelAutoRepeatSpell();
        }
    }

    if (autoAttackRequested_ && autoAttackTarget_ == targetGuid) return;
    autoAttackRequested_ = true;
    // Server confirmation is distinct from local attack intent. Range errors
    // leave intent armed while the server waits for a valid melee swing.
    autoAttacking_ = false;
    autoAttackTarget_ = targetGuid;
    autoAttackOutOfRange_ = false;
    // Face the target, once, on the command to attack it. This is the only
    // place combat turns the player: it used to be re-aimed every fifth of a
    // second for as long as the attack lasted.
    //
    // Only with the Combat page's debug setting on. The original client does
    // not turn the player at all; attacking something behind them is refused
    // until they face it themselves.
    auto target = owner_.isAutoFaceTarget() ? owner_.getEntityManager().getEntity(targetGuid)
                                            : nullptr;
    if (target) {
        const float toTargetX = target->getLatestX() - owner_.movementInfoRef().x;
        const float toTargetY = target->getLatestY() - owner_.movementInfoRef().y;
        if (std::abs(toTargetX) > 0.01f || std::abs(toTargetY) > 0.01f) {
            owner_.movementInfoRef().orientation = std::atan2(-toTargetY, toTargetX);
            if (owner_.getState() == WorldState::IN_WORLD && owner_.getSocket()) {
                owner_.sendMovement(Opcode::MSG_MOVE_SET_FACING);
            }
        }
    }
    // Starting an attack closes the loot window in the original client
    // (observed, ledger #85). The server does not: CMaNGOS
    // HandleAttackSwingOpcode leaves the loot open, so the client releases it
    // the way the window's own close does (CMSG_LOOT_RELEASE, LOOT_CLOSED).
    if (owner_.isLootWindowOpen()) owner_.closeLoot();
    // Out of melee reach the original says so itself as the attack starts,
    // and again when the server's SMSG_ATTACKSWING_NOTINRANGE arrives a round
    // trip later (2.4.3, /startattack 26.47 yards from a boar with Onyxia's
    // reach 23.4: two "You are too far away!" 70 ms apart; none at 26.16).
    if (owner_.unitWithinMeleeReach(targetGuid) == false) owner_.errorGlobal("ERR_BADATTACKPOS");
    if (owner_.getState() == WorldState::IN_WORLD && owner_.getSocket()) {
        auto packet = AttackSwingPacket::build(targetGuid);
        owner_.getSocket()->send(packet);
    }
    // The attack state is local from here (IsCurrentAction on the Attack
    // button reads true in the same frame), but PLAYER_ENTER_COMBAT waits for
    // the server's SMSG_ATTACKSTART. Original 2.4.3: /startattack flips
    // IsCurrentAction at once and the event follows one round trip later;
    // Sinister Strike gives SENT, SUCCEEDED, then PLAYER_ENTER_COMBAT.
    LOG_INFO("Starting auto-attack on 0x", std::hex, targetGuid, std::dec);
}

void CombatHandler::stopMeleeAutoAttackOnly() {
    if (!autoAttacking_ && !autoAttackRequested_) return;
    autoAttackRequested_ = false;
    autoAttacking_ = false;
    autoAttackTarget_ = 0;
    autoAttackOutOfRange_ = false;
    // PLAYER_LEAVE_COMBAT comes with the server's SMSG_ATTACKSTOP, once.
    // Original 2.4.3: /stopattack fires it one round trip later, and firing it
    // here as well gave every stop two of them. With no server to answer,
    // nothing else would end the announced attack.
    if (owner_.getState() == WorldState::IN_WORLD && owner_.getSocket()) {
        auto packet = AttackStopPacket::build();
        owner_.getSocket()->send(packet);
    } else {
        announceLeaveCombat();
    }
    LOG_INFO("Stopping auto-attack");
}

void CombatHandler::stopAutoAttack() {
    stopMeleeAutoAttackOnly();
    if (auto* sh = owner_.getSpellHandler()) {
        sh->clearStunSuspendedAutoRepeat();
        if (sh->getAutoRepeatSpellId() == 75u) {
            sh->cancelAutoRepeatSpell();
        }
    }
}

void CombatHandler::announceLeaveCombat() {
    if (!enterCombatAnnounced_) return;
    enterCombatAnnounced_ = false;
    owner_.fireAddonEvent("PLAYER_LEAVE_COMBAT", {});
}

// ============================================================
// Combat text
// ============================================================

void CombatHandler::fireUnitCombat(uint64_t victim, const char* action, const char* flags, int32_t amount,
                                   uint32_t schoolMask) {
    if (victim == 0 || !owner_.addonEventCallbackRef()) return;
    const std::string unitId = owner_.guidToUnitId(victim);
    if (unitId.empty()) return;
    owner_.addonEventCallbackRef()("UNIT_COMBAT",
        {unitId, action, flags, std::to_string(amount), std::to_string(schoolMask)});
}

void CombatHandler::addCombatText(CombatTextEntry::Type type, int32_t amount, uint32_t spellId, bool isPlayerSource, uint8_t powerType,
                                  uint64_t srcGuid, uint64_t dstGuid, uint32_t schoolMask, bool reduction,
                                  bool unitCombat, uint32_t extraSpellId, bool periodicAura) {
    bool normalRanged = false;
    if (spellId != 0) {
        owner_.loadSpellNameCache();
        if (const auto it = owner_.spellNameCacheRef().find(spellId);
            it != owner_.spellNameCacheRef().end()) {
            normalRanged = isNormalRangedAttackSpell(it->second.attrEx3);
        }
    }
    const bool isSpellAbility = spellId != 0 && !normalRanged;

    // COMBAT_TEXT_UPDATE: what Blizzard_CombatText draws for the player.
    // Build 8606 fires it whatever enableCombatText says, so it is sent
    // before the panel's filters below, which govern the world numbers.
    {
        using T = CombatTextEntry;
        const bool hitsPlayer = dstGuid != 0 && dstGuid == owner_.getPlayerGuid();
        if (hitsPlayer || type == T::HONOR_GAIN) {
            // The partial forms take the damage of the hit this line is part of.
            // Match lua_UnitName("player") fallback (character list -> "Unknown") when
            // srcGuid is the local player so Blizzard_CombatText's `UnitName("player") ~= data`
            // never prints "+1500 []" on self-heals with fctFriendlyHealers enabled.
            std::string caster = srcGuid ? owner_.lookupName(srcGuid) : std::string{};
            if (caster.empty() && srcGuid != 0 && srcGuid == owner_.getPlayerGuid()) {
                for (const auto& c : owner_.getCharacters()) {
                    if (c.guid == srcGuid) { caster = c.name; break; }
                }
                if (caster.empty()) caster = "Unknown";
            }
            const bool fromSpellForUpdate = isSpellAbility && !periodicAura;
            const auto update = combatTextUpdateFor(type, amount, fromSpellForUpdate, powerType, caster,
                                                    reduction ? incomingHitDamage_ : -1,
                                                    isActiveExpansion("wotlk"));
            if (!reduction) {
                const bool damage = type == T::MELEE_DAMAGE || type == T::GLANCING || type == T::CRUSHING ||
                                    type == T::CRIT_DAMAGE || type == T::SPELL_DAMAGE || type == T::PERIODIC_DAMAGE;
                incomingHitDamage_ = damage && hitsPlayer ? amount : -1;
            }
            if (update && owner_.addonEventCallbackRef()) {
                // Absent data reaches Lua as nil: the addon tests arg3 for a partial block.
                // Preserve positional arg2 when arg3 is present (e.g. HEAL with unknown caster).
                std::vector<std::string> args{update->type};
                if (!update->arg3.empty()) {
                    args.push_back(update->arg2);
                    args.push_back(update->arg3);
                } else if (!update->arg2.empty()) {
                    args.push_back(update->arg2);
                }
                owner_.addonEventCallbackRef()("COMBAT_TEXT_UPDATE", args);
            }
        }
    }

    // The panel's world-text filters govern 3D floating combat text only;
    // UNIT_COMBAT portrait feedback and COMBAT_LOG_EVENT still fire.
    const auto rule = combatTextFilterFor(type, isPlayerSource, srcGuid, dstGuid,
                                          owner_.getPetGuid(), owner_.getTargetGuid(),
                                          isSpellAbility);
    const bool suppressWorldText =
        rule.cvar && addons::storedCVarValue(rule.cvar, rule.fallback) == "0";
    if (!suppressWorldText) {
        CombatTextEntry entry;
        entry.type = type;
        entry.amount = amount;
        entry.spellId = spellId;
        entry.extraSpellId = extraSpellId;
        entry.age = 0.0f;
        entry.isPlayerSource = isPlayerSource;
        entry.powerType = powerType;
        entry.srcGuid = srcGuid;
        entry.dstGuid = dstGuid;
        entry.reduction = reduction;
        entry.normalRangedAttack = normalRanged;
        combatText_.push_back(entry);
    }

    // UNIT_COMBAT - the same hit, for the frames that draw their own feedback
    // over a portrait (CombatFeedback.lua). Swings and spell damage logs
    // announce themselves (fireUnitCombat, for every unit with a token, as the
    // original does); this covers direct heals, environmental damage, periodic
    // damage/absorbs/resists, and spell misses/resists/dodges/parries/blocks.
    // Periodic heals (HoTs) and partial reductions never fire UNIT_COMBAT in build 8606.
    if (unitCombat && !reduction) {
        const char* action = nullptr;
        const char* flags  = "";
        int32_t ucAmount = amount;
        switch (type) {
            case CombatTextEntry::MELEE_DAMAGE:
            case CombatTextEntry::SPELL_DAMAGE:
            case CombatTextEntry::PERIODIC_DAMAGE:
            case CombatTextEntry::ENVIRONMENTAL: action = "WOUND"; break;
            case CombatTextEntry::CRIT_DAMAGE: action = "WOUND"; flags = "CRITICAL"; break;
            case CombatTextEntry::MISS: action = "MISS"; ucAmount = 0; break;
            case CombatTextEntry::DODGE: action = "DODGE"; ucAmount = 0; break;
            case CombatTextEntry::PARRY: action = "PARRY"; ucAmount = 0; break;
            case CombatTextEntry::EVADE: action = "EVADE"; ucAmount = 0; break;
            case CombatTextEntry::DEFLECT: action = "DEFLECT"; ucAmount = 0; break;
            case CombatTextEntry::REFLECT: action = "REFLECT"; ucAmount = 0; break;
            case CombatTextEntry::BLOCK: action = "BLOCK"; flags = "BLOCK"; ucAmount = 0; break;
            case CombatTextEntry::ABSORB: action = "WOUND"; flags = "ABSORB"; ucAmount = 0; break;
            case CombatTextEntry::RESIST:
                if (spellId != 0 && !periodicAura && amount == 0) {
                    action = "RESIST";
                } else {
                    action = "WOUND";
                    flags = "RESIST";
                }
                ucAmount = 0;
                break;
            case CombatTextEntry::HEAL: action = "HEAL"; break;
            case CombatTextEntry::CRIT_HEAL: action = "HEAL"; flags = "CRITICAL"; break;
            default: break;  // periodic heals, spell immune, energize, xp, honour - not combat feedback
        }
        if (action) fireUnitCombat(dstGuid, action, flags, ucAmount, schoolMask);
    }

    // Persistent combat log - use explicit GUIDs if provided, else fall back to
    // player/current-target (the old behaviour for events without specific participants).
    CombatLogEntry log;
    log.type     = type;
    log.amount   = amount;
    log.spellId  = spellId;
    log.extraSpellId = extraSpellId;
    log.isPlayerSource = isPlayerSource;
    log.powerType = powerType;
    log.timestamp = std::time(nullptr);
    // If the caller provided an explicit destination GUID but left source GUID as 0,
    // preserve "unknown/no source" (e.g. environmental damage) instead of
    // backfilling from current target.
    uint64_t effectiveSrc = (srcGuid != 0) ? srcGuid
                          : ((dstGuid != 0) ? 0 : (isPlayerSource ? owner_.getPlayerGuid() : owner_.getTargetGuid()));
    uint64_t effectiveDst = (dstGuid != 0) ? dstGuid
                          : (isPlayerSource ? owner_.getTargetGuid() : owner_.getPlayerGuid());
    log.sourceName = owner_.lookupName(effectiveSrc);
    log.targetName = (effectiveDst != 0) ? owner_.lookupName(effectiveDst) : std::string{};
    // Held for the addon event below. log is moved into combatLog_ here, which
    // leaves both name strings empty, so reading them afterwards sent every
    // COMBAT_LOG_EVENT_UNFILTERED to Lua with a blank source and target.
    const std::string sourceName = log.sourceName;
    const std::string targetName = log.targetName;
    if (combatLog_.size() >= MAX_COMBAT_LOG)
        combatLog_.pop_front();
    combatLog_.push_back(std::move(log));
    ++combatLogAppended_;

    // The combat log's own events, in the layout Blizzard_CombatLog parses.
    // The unfiltered one is what addons listen for; the filtered one is what the
    // log window itself is registered to, and nothing fired it, so the window
    // was empty however much fighting there was. No filter is applied here: the
    // window applies its own settings to what it is given.
    // 1.12 has no combat log event, so Classic and Turtle are not sent one.
    if (owner_.addonEventCallbackRef() && !isClassicLikeExpansion()) {
        CombatLogEventInput input;
        input.layout = isActiveExpansion("tbc") ? CombatLogLayout::Tbc : CombatLogLayout::Wotlk;
        input.type = type;
        input.amount = amount;
        input.spellId = spellId;
        input.extraSpellId = extraSpellId;
        input.spellSchoolMask = (spellId != 0) ? owner_.getSpellSchoolMask(spellId) : 0;
        input.extraSpellSchoolMask = (extraSpellId != 0) ? owner_.getSpellSchoolMask(extraSpellId) : 0;
        input.damageSchoolMask = schoolMask;
        input.powerType = powerType;
        input.srcGuid = effectiveSrc;
        input.dstGuid = effectiveDst;
        input.playerGuid = owner_.getPlayerGuid();
        input.petGuid = owner_.getPetGuid();
        input.srcName = sourceName;
        input.dstName = targetName;
        input.spellName = (spellId != 0) ? owner_.getSpellName(spellId) : std::string{};
        input.extraSpellName = (extraSpellId != 0) ? owner_.getSpellName(extraSpellId) : std::string{};
        input.timestamp = static_cast<int64_t>(std::time(nullptr));
        input.normalRangedAttack = normalRanged;
        const std::vector<std::string> args = buildCombatLogEvent(input);
        if (!args.empty()) {
            owner_.addonEventCallbackRef()("COMBAT_LOG_EVENT_UNFILTERED", args);
            owner_.addonEventCallbackRef()("COMBAT_LOG_EVENT", args);
        }
    }

    // 1.12's combat log is chat: a sentence on one of its CHAT_MSG_COMBAT_*
    // and CHAT_MSG_SPELL_* events, which the Combat Log tab and the damage
    // meters written for 1.12 both read.
    if (owner_.addonEventCallbackRef() && isClassicLikeExpansion()) {
        ClassicCombatInput in;
        in.type = type;
        in.amount = amount;
        in.powerType = powerType;
        in.schoolMask = (spellId != 0) ? owner_.getSpellSchoolMask(spellId) : 0;
        in.spellName = (spellId != 0) ? owner_.getSpellName(spellId) : std::string{};
        in.srcName = sourceName.empty() ? classic_combat::global("UNKNOWN") : sourceName;
        in.dstName = targetName.empty() ? classic_combat::global("UNKNOWN") : targetName;
        in.src = combatRelationOf(owner_, effectiveSrc);
        in.dst = combatRelationOf(owner_, effectiveDst);
        const ClassicCombatMessage line = buildClassicCombatMessage(in);
        if (!line.event.empty()) {
            std::vector<std::string> args{line.event, line.pattern};
            args.insert(args.end(), line.args.begin(), line.args.end());
            owner_.addonEventCallbackRef()(classic_combat::kLineEvent, args);
        }
    }
}

bool CombatHandler::shouldLogSpellstealAura(uint64_t casterGuid, uint64_t victimGuid, uint32_t spellId) {
    if (spellId == 0) return false;

    const auto now = std::chrono::steady_clock::now();
    constexpr auto kRecentWindow = std::chrono::seconds(1);
    while (!recentSpellstealLogs_.empty() &&
           now - recentSpellstealLogs_.front().timestamp > kRecentWindow) {
        recentSpellstealLogs_.pop_front();
    }

    for (auto it = recentSpellstealLogs_.begin(); it != recentSpellstealLogs_.end(); ++it) {
        if (it->casterGuid == casterGuid &&
            it->victimGuid == victimGuid &&
            it->spellId == spellId) {
            recentSpellstealLogs_.erase(it);
            return false;
        }
    }

    if (recentSpellstealLogs_.size() >= MAX_RECENT_SPELLSTEAL_LOGS)
        recentSpellstealLogs_.pop_front();
    recentSpellstealLogs_.push_back({.casterGuid = casterGuid, .victimGuid = victimGuid, .spellId = spellId, .timestamp = now});
    return true;
}

void CombatHandler::updateCombatText(float deltaTime) {
    for (auto& entry : combatText_) {
        entry.age += deltaTime;
    }
    combatText_.erase(
        std::remove_if(combatText_.begin(), combatText_.end(),
                       [](const CombatTextEntry& e) { return e.isExpired(); }),
        combatText_.end());
}

// ============================================================
// Packet handlers
// ============================================================

void CombatHandler::autoTargetAttacker(uint64_t attackerGuid) {
    if (attackerGuid == 0 || attackerGuid == owner_.getPlayerGuid()) return;
    if (owner_.getTargetGuid() != 0) return;
    if (!owner_.getEntityManager().hasEntity(attackerGuid)) return;
    owner_.setTarget(attackerGuid);
}

void CombatHandler::handleAttackStart(network::Packet& packet) {
    AttackStartData data;
    if (!AttackStartParser::parse(packet, data)) return;

    // CREEP is a client presentation flag, not an instruction to keep a unit
    // translucent after it has openly engaged. The stock client reveals both
    // combatants at ATTACKSTART.
    //
    // The reason recorded here used to be that "some legacy realms do not send
    // a matching UNIT_FIELD_BYTES_1 update when an NPC breaks stealth". That
    // was almost certainly this client's own fault: UNIT_FIELD_BYTES_1 was
    // being read from field 137 where the server writes 74, so the update
    // arrived and was taken from the wrong slot. Fixed 2026-08-05. This is
    // kept because the stock client does it anyway, but if a stealth-break
    // ever needs looking at again, start by checking whether the field now
    // arrives - the premise behind this workaround is gone. Clear the cached
    // presentation state here so the normal creature visual sync restores full
    // opacity instead of leaving an invisible monster in melee.
    auto revealCombatant = [this](uint64_t guid) {
        auto entity = owner_.getEntityManager().getEntity(guid);
        if (!entity || !entity->isUnit()) return;
        auto unit = std::static_pointer_cast<Unit>(entity);
        if (!unit->hasCreepVisibility()) return;
        unit->clearCreepVisibility();
        if (guid == owner_.getPlayerGuid() && owner_.stealthStateCallbackRef()) {
            owner_.stealthStateCallbackRef()(false);
        }
    };
    revealCombatant(data.attackerGuid);
    revealCombatant(data.victimGuid);

    if (auto attacker = owner_.getEntityManager().getEntity(data.attackerGuid);
        attacker && attacker->isUnit()) {
        std::static_pointer_cast<Unit>(attacker)->setMeleeVictimGuid(data.victimGuid);
    }

    if (data.attackerGuid == owner_.getPlayerGuid()) {
        autoAttackRequested_ = true;
        autoAttacking_ = true;
        autoAttackTarget_ = data.victimGuid;
        if (!enterCombatAnnounced_) {
            enterCombatAnnounced_ = true;
            owner_.fireAddonEvent("PLAYER_ENTER_COMBAT", {});
        }
    } else if (data.victimGuid == owner_.getPlayerGuid() && data.attackerGuid != 0) {
        hostileAttackers_.insert(data.attackerGuid);
        autoTargetAttacker(data.attackerGuid);

        // Play aggro sound when NPC attacks player
        if (owner_.npcAggroCallbackRef()) {
            auto entity = owner_.getEntityManager().getEntity(data.attackerGuid);
            if (entity && entity->getType() == ObjectType::UNIT) {
                glm::vec3 pos(entity->getX(), entity->getY(), entity->getZ());
                owner_.npcAggroCallbackRef()(data.attackerGuid, pos);
            }
        }
    }

    // Attack intent carries no facing. Movement and facing packets own each
    // participant's orientation, including a player attacked from behind.
}

void CombatHandler::handleAttackStop(network::Packet& packet) {
    AttackStopData data;
    if (!AttackStopParser::parse(packet, data)) return;

    if (auto attacker = owner_.getEntityManager().getEntity(data.attackerGuid);
        attacker && attacker->isUnit()) {
        std::static_pointer_cast<Unit>(attacker)->setMeleeVictimGuid(0);
    }

    // A server stop cancels attack mode; range errors do not send this packet.
    if (data.attackerGuid == owner_.getPlayerGuid()) {
        autoAttacking_ = false;
        autoAttackRequested_ = false;
        autoAttackTarget_ = 0;
        autoAttackOutOfRange_ = false;
        // Original 2.4.3: a stop answered while not attacking fires nothing.
        announceLeaveCombat();
        LOG_DEBUG("SMSG_ATTACKSTOP cleared auto-attack intent");
    } else if (data.victimGuid == owner_.getPlayerGuid()) {
        hostileAttackers_.erase(data.attackerGuid);
    }
}

// VictimState, the same in every expansion's servers.
constexpr uint32_t VICTIMSTATE_DODGE = 2, VICTIMSTATE_PARRY = 3, VICTIMSTATE_BLOCKS = 5,
                   VICTIMSTATE_EVADES = 6, VICTIMSTATE_IS_IMMUNE = 7, VICTIMSTATE_DEFLECTS = 8;

void CombatHandler::handleAttackerStateUpdate(network::Packet& packet) {
    AttackerStateUpdateData data;
    if (!owner_.getPacketParsers()->parseAttackerStateUpdate(packet, data)) return;
    LOG_DEBUG("Melee swing attacker=0x", std::hex, data.attackerGuid, " victim=0x", data.targetGuid,
              " hitInfo=0x", data.hitInfo, std::dec, " victimState=", data.victimState);

    bool isPlayerAttacker = (data.attackerGuid == owner_.getPlayerGuid());
    bool isPlayerTarget = (data.targetGuid == owner_.getPlayerGuid());

    constexpr uint32_t HITINFO_LEFTSWING = 0x4;  // CMaNGOS Unit.h HitInfo
    const bool isOffHandSwing = (data.hitInfo & HITINFO_LEFTSWING) != 0;
    bool suppressedForRangedShot = false;

    if (isPlayerAttacker) {
        autoAttackOutOfRange_ = false;
        lastMeleeSwingMs_ = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        // Skip melee animation if a ranged shot was just triggered from
        // SMSG_SPELL_GO (Auto Shot / Shoot / Throw), or if HITINFO_NOACTION
        // marks an on-next-swing spell result (Heroic Strike, Cleave, Maul,
        // Raptor Strike) whose animation already comes from SMSG_SPELL_GO.
        if (owner_.consumeSuppressMeleeSwingAnim()) {
            suppressedForRangedShot = true;
            LOG_DEBUG("Suppressed melee swing anim - ranged shot already triggered");
        } else if (!data.isNoAction()) {
            owner_.setCurrentMeleeSwingCrit(data.isCrit());
            if (owner_.meleeSwingCallbackRef()) owner_.meleeSwingCallbackRef()(0, isOffHandSwing);
            owner_.setCurrentMeleeSwingCrit(false);
        }
    }
    // Lethal blows suppress victim hit reactions and must not arm or retain a
    // dead melee target: CMaNGOS sends SMSG_ATTACKSTOP before
    // SMSG_ATTACKERSTATEUPDATE (or omits it on a one-shot), and
    // SMSG_UPDATE_OBJECT sets UNIT_FIELD_HEALTH=0 one tick later.
    const bool lethalHit = [&]() {
        if (data.overkill > 0) return true;
        const auto ent = owner_.getEntityManager().getEntity(data.targetGuid);
        if (!ent || !ent->isUnit()) return false;
        const auto unit = std::static_pointer_cast<Unit>(ent);
        if (isUnitCorpseState(unit->getHealth(), unit->getMaxHealth(), unit->getDynamicFlags())) {
            return true;
        }
        if (data.totalDamage <= 0) return false;
        return GameHandler::isLethalDamage(
            unit->getHealth(), unit->getMaxHealth(), data.totalDamage, data.overkill);
    }();
    if (lethalHit && data.targetGuid != owner_.getPlayerGuid()) {
        if (auto* victimUnit = owner_.getUnitByGuid(data.targetGuid)) {
            victimUnit->markPendingLethalHit();
        }
    }
    // A non-lethal swing puts its attacker in melee, as SMSG_ATTACKSTART does.
    // The server sends ATTACKSTART once, when the attack begins; a unit already
    // fighting when it came into view has only its swings. Build 8606 holds
    // such a unit in Stand until its first swing and in its ready stance
    // from then on (_experiments/20261118T091949Z.creature_swing_anim, reenter).
    if (auto attacker = owner_.getEntityManager().getEntity(data.attackerGuid);
        attacker && attacker->isUnit() && data.targetGuid != 0) {
        auto attackerUnit = std::static_pointer_cast<Unit>(attacker);
        if (lethalHit) {
            if (attackerUnit->getMeleeVictimGuid() == data.targetGuid) {
                attackerUnit->setMeleeVictimGuid(0);
            }
        } else {
            attackerUnit->setMeleeVictimGuid(data.targetGuid);
        }
    }
    if (!isPlayerAttacker && !data.isNoAction() && owner_.npcSwingCallbackRef()) {
        owner_.setCurrentMeleeSwingCrit(data.isCrit());
        owner_.npcSwingCallbackRef()(data.attackerGuid, isOffHandSwing);
        owner_.setCurrentMeleeSwingCrit(false);
    }
    // Observers must see the same victim reaction as the combatants. Miss,
    // evade and immunity do not wound; dodge/parry/block are explicit outcomes.
    // HITINFO_NOACTION (CMaNGOS sends it for on-next-swing spell results):
    // build 8606 shows the damage and leaves the victim still, no spurt
    // either (_experiments/20261119T1311Z.wound_rule, thugflags).
    if (owner_.hitReactionCallbackRef() && !data.isNoAction() && !lethalHit) {
        using HR = GameHandler::HitReaction;
        std::optional<HR> reaction;
        if (data.victimState == VICTIMSTATE_DODGE) reaction = HR::DODGE;
        else if (data.victimState == VICTIMSTATE_PARRY) reaction = HR::PARRY;
        else if (data.victimState == VICTIMSTATE_BLOCKS || data.blocked > 0) reaction = HR::BLOCK;
        else if (!data.isMiss() && data.victimState != VICTIMSTATE_EVADES &&
                 data.victimState != VICTIMSTATE_IS_IMMUNE && data.victimState != VICTIMSTATE_DEFLECTS)
            reaction = data.isCrit() ? HR::CRIT_WOUND : HR::WOUND;
        if (reaction) owner_.hitReactionCallbackRef()(data.attackerGuid, data.targetGuid, *reaction);
    }

    // Queue 3D/local combat audio for ALL visible units (local player, remote
    // players, pets, and NPC-vs-NPC combat) before the local-UI early return.
    enqueueMeleeCombatAudio(data, isOffHandSwing, suppressedForRangedShot, lethalHit);

    // School of the swing (the first sub-damage; a swing has one), and what
    // was taken off it.
    const uint32_t swingSchool = data.subDamages.empty() ? 0 : data.subDamages.front().schoolMask;
    uint32_t totalAbsorbed = 0, totalResisted = 0;
    for (const auto& sub : data.subDamages) {
        totalAbsorbed += sub.absorbed;
        totalResisted += sub.resisted;
    }
    // UNIT_COMBAT for every unit the interface has a token for, whoever swung:
    // the original announces another player's swings at its target as well
    // (pair 13 log, combat_text). Outcomes that stop the whole swing are their
    // own action with no amount; a full block is BLOCK flagged BLOCK. A swing
    // that lands is a WOUND of its damage, flagged by how it landed or, when
    // nothing got through, by what stopped it - the flags CombatFeedback reads.
    {
        const char* action = "WOUND";
        const char* flags = "";
        int32_t amount = 0;
        if (data.victimState == VICTIMSTATE_EVADES) action = "EVADE";
        else if (data.isMiss()) action = "MISS";
        else if (data.victimState == VICTIMSTATE_DODGE) action = "DODGE";
        else if (data.victimState == VICTIMSTATE_PARRY) action = "PARRY";
        else if (data.victimState == VICTIMSTATE_BLOCKS) { action = "BLOCK"; flags = "BLOCK"; }
        else if (data.victimState == VICTIMSTATE_IS_IMMUNE) action = "IMMUNE";
        else if (data.victimState == VICTIMSTATE_DEFLECTS) action = "DEFLECT";
        else {
            amount = data.totalDamage;
            if (amount != 0) {
                if (data.isCrit()) flags = "CRITICAL";
                else if (data.isCrushing()) flags = "CRUSHING";
                else if (data.isGlancing()) flags = "GLANCING";
            } else if (totalAbsorbed > 0) flags = "ABSORB";
            else if (data.blocked > 0) flags = "BLOCK";
            else if (totalResisted > 0) flags = "RESIST";
        }
        fireUnitCombat(data.targetGuid, action, flags, amount, swingSchool);
    }

    // The pet's swings are written over its victim too ("Pet Damage",
    // PetMeleeDamage): the original showed the imp's hits on its target.
    const bool isPetAttacker = owner_.getPetGuid() != 0 && data.attackerGuid == owner_.getPetGuid();

    // Local combat UI and auto-targeting remain local.
    if (!isPlayerAttacker && !isPlayerTarget && !isPetAttacker) return;

    if (isPlayerTarget && data.attackerGuid != 0) {
        hostileAttackers_.insert(data.attackerGuid);
        autoTargetAttacker(data.attackerGuid);
    }

    // An evade comes with the miss flag set as well, so it is asked first.
    const auto line = [&](CombatTextEntry::Type type, int32_t amount, bool reduction = false) {
        addCombatText(type, amount, 0, isPlayerAttacker, 0, data.attackerGuid, data.targetGuid,
                      swingSchool, reduction, false);
    };
    if (data.victimState == VICTIMSTATE_EVADES) {
        line(CombatTextEntry::EVADE, 0);
    } else if (data.isMiss()) {
        line(CombatTextEntry::MISS, 0);
    } else if (data.victimState == VICTIMSTATE_DODGE) {
        line(CombatTextEntry::DODGE, 0);
    } else if (data.victimState == VICTIMSTATE_PARRY) {
        line(CombatTextEntry::PARRY, 0);
    } else if (data.victimState == VICTIMSTATE_BLOCKS) {
        line(CombatTextEntry::BLOCK, static_cast<int32_t>(data.blocked));
    } else if (data.victimState == VICTIMSTATE_IS_IMMUNE) {
        line(CombatTextEntry::IMMUNE, 0);
    } else if (data.victimState == VICTIMSTATE_DEFLECTS) {
        line(CombatTextEntry::DEFLECT, 0);
    } else if (data.totalDamage == 0 && totalAbsorbed > 0) {
        // Nothing got through: the swing is named by what stopped it
        // (the original writes "Absorb" over the unit, nothing else).
        line(CombatTextEntry::ABSORB, static_cast<int32_t>(totalAbsorbed));
    } else if (data.totalDamage == 0 && data.blocked > 0) {
        line(CombatTextEntry::BLOCK, static_cast<int32_t>(data.blocked));
    } else if (data.totalDamage == 0 && totalResisted > 0) {
        line(CombatTextEntry::RESIST, static_cast<int32_t>(totalResisted));
    } else if (!data.isNoAction() || data.totalDamage > 0) {
        // An on-next-swing spell hit (Heroic Strike, Cleave, Maul, Raptor Strike)
        // arrives as a 0-damage HITINFO_NOACTION swing paired with SMSG_SPELLNONMELEEDAMAGELOG;
        // build 8606 emits no SWING_DAMAGE 0 or world "Miss" for that carrier packet
        // (verified on Pair 12, Cleave 15496). An on-next-swing miss carries HITINFO_MISS
        // (0x10012) or a dodge/parry victimState and is already handled above.
        CombatTextEntry::Type type;
        if (data.isCrit())
            type = CombatTextEntry::CRIT_DAMAGE;
        else if (data.isCrushing())
            type = CombatTextEntry::CRUSHING;
        else if (data.isGlancing())
            type = CombatTextEntry::GLANCING;
        else
            type = CombatTextEntry::MELEE_DAMAGE;
        line(type, data.totalDamage);
        // What was taken off a hit that still did damage, for the combat log
        // and COMBAT_TEXT_UPDATE's partial forms.
        if (totalAbsorbed > 0) line(CombatTextEntry::ABSORB, static_cast<int32_t>(totalAbsorbed), true);
        if (totalResisted > 0) line(CombatTextEntry::RESIST, static_cast<int32_t>(totalResisted), true);
        if (data.blocked > 0) line(CombatTextEntry::BLOCK, static_cast<int32_t>(data.blocked), true);
    }

}

std::optional<GameHandler::HitReaction> GameHandler::spellDamageHitReaction(
    uint32_t damage, bool isCrit, uint32_t schoolMask, bool hasImpactVisual) {
    if (damage == 0 || !hasImpactVisual) return std::nullopt;
    // Physical spells (SPELL_SCHOOL_MASK_NORMAL = 0x1) trigger melee-style wound + blood spurt;
    // magic school spells (Arcane, Fire, Frost, Shadow, Nature, Holy) trigger wound flinch without blood.
    if (schoolMask == 1u) {
        return isCrit ? HitReaction::CRIT_WOUND : HitReaction::WOUND;
    }
    return isCrit ? HitReaction::SPELL_CRIT_WOUND : HitReaction::SPELL_WOUND;
}

bool GameHandler::isLethalDamage(
    uint32_t victimHealth, uint32_t victimMaxHealth, int32_t damage, int32_t overkill) {
    if (overkill > 0) return true;
    if (damage <= 0) return false;
    if (victimMaxHealth == 0) return false;
    return victimHealth == 0 || static_cast<uint32_t>(damage) >= victimHealth;
}

void CombatHandler::handleSpellDamageLog(network::Packet& packet) {
    SpellDamageLogData data;
    if (!owner_.getPacketParsers()->parseSpellDamageLog(packet, data)) return;

    bool isPlayerSource = (data.attackerGuid == owner_.getPlayerGuid());
    bool isPlayerTarget = (data.targetGuid == owner_.getPlayerGuid());

    const bool lethalHit = [&]() {
        const int32_t dmg = static_cast<int32_t>(data.damage);
        const int32_t ok = static_cast<int32_t>(data.overkill);
        if (ok > 0) return true;
        if (dmg <= 0) return false;
        const auto ent = owner_.getEntityManager().getEntity(data.targetGuid);
        if (!ent || !ent->isUnit()) return false;
        const auto unit = std::static_pointer_cast<Unit>(ent);
        return GameHandler::isLethalDamage(unit->getHealth(), unit->getMaxHealth(), dmg, ok);
    }();
    if (lethalHit && data.targetGuid != owner_.getPlayerGuid()) {
        if (auto* victimUnit = owner_.getUnitByGuid(data.targetGuid)) {
            victimUnit->markPendingLethalHit();
        }
    }

    // Direct spell damage with a per-target SpellVisual impact kit triggers victim
    // hit-flinch (StandWound 8 / CombatWound 9 / CombatCritical 10) for local player,
    // remote players, and creatures alike — including spells whose impact kit omits
    // startAnimId (Arcane Missiles 7268/29956, Moonfire 8921, Shadow Bolt 686, Smite 585).
    // Spells without a per-target impact kit (Void Zone Consumption 28865, Four Horsemen
    // Mark 28836, Meteor 28884, periodic ground pulses), periodic DoT ticks
    // (SMSG_PERIODICAURALOG), and lethal blows do not flinch in TBC 2.4.3.
    const bool hasImpactVisual = owner_.getSpellHandler()
        ? owner_.getSpellHandler()->spellHasTargetImpactVisual(data.spellId)
        : (data.spellId != 0);
    if (owner_.hitReactionCallbackRef() && !lethalHit) {
        if (const auto reaction = GameHandler::spellDamageHitReaction(
                data.damage, data.isCrit, data.schoolMask, hasImpactVisual);
            reaction.has_value()) {
            owner_.hitReactionCallbackRef()(data.attackerGuid, data.targetGuid, *reaction);
        }
    }

    // UNIT_COMBAT whoever cast it, as for a swing: a WOUND of the damage
    // (`.damage 50 2 133` on the original: target|WOUND|CRITICAL|75|4).
    fireUnitCombat(data.targetGuid, "WOUND",
                   data.damage > 0 ? (data.isCrit ? "CRITICAL" : "")
                                   : data.absorbed > 0 ? "ABSORB"
                                   : data.blocked > 0 ? "BLOCK"
                                   : data.resisted > 0 ? "RESIST" : "",
                   static_cast<int32_t>(data.damage), data.schoolMask);

    // Queue 3D/local physical/ranged weapon impacts & victim wound vocals for all visible units.
    enqueuePhysicalSpellCombatAudio(data, lethalHit);

    const bool isPetSource = owner_.getPetGuid() != 0 && data.attackerGuid == owner_.getPetGuid();
    if (!isPlayerSource && !isPlayerTarget && !isPetSource) return;  // Not our combat

    if (isPlayerTarget && data.attackerGuid != 0) {
        hostileAttackers_.insert(data.attackerGuid);
        autoTargetAttacker(data.attackerGuid);
    }

    const auto line = [&](CombatTextEntry::Type type, uint32_t amount, bool reduction = false) {
        addCombatText(type, static_cast<int32_t>(amount), data.spellId, isPlayerSource, 0, data.attackerGuid,
                      data.targetGuid, data.schoolMask, reduction, false);
    };
    if (data.damage > 0) {
        line(data.isCrit ? CombatTextEntry::CRIT_DAMAGE : CombatTextEntry::SPELL_DAMAGE, data.damage);
        if (data.absorbed > 0) line(CombatTextEntry::ABSORB, data.absorbed, true);
        if (data.resisted > 0) line(CombatTextEntry::RESIST, data.resisted, true);
        if (data.blocked > 0) line(CombatTextEntry::BLOCK, data.blocked, true);
    } else if (data.absorbed > 0) {
        line(CombatTextEntry::ABSORB, data.absorbed);
    } else if (data.blocked > 0) {
        line(CombatTextEntry::BLOCK, data.blocked);
    } else if (data.resisted > 0) {
        line(CombatTextEntry::RESIST, data.resisted);
    }
}

void CombatHandler::handleSpellHealLog(network::Packet& packet) {
    SpellHealLogData data;
    if (!owner_.getPacketParsers()->parseSpellHealLog(packet, data)) return;

    bool isPlayerSource = (data.casterGuid == owner_.getPlayerGuid());
    bool isPlayerTarget = (data.targetGuid == owner_.getPlayerGuid());
    if (!isPlayerSource && !isPlayerTarget) return;  // Not our combat

    auto type = data.isCrit ? CombatTextEntry::CRIT_HEAL : CombatTextEntry::HEAL;
    addCombatText(type, static_cast<int32_t>(data.heal), data.spellId, isPlayerSource, 0, data.casterGuid, data.targetGuid);
    if (data.absorbed > 0)
        addCombatText(CombatTextEntry::ABSORB, static_cast<int32_t>(data.absorbed), data.spellId, isPlayerSource, 0, data.casterGuid, data.targetGuid);
}

void CombatHandler::handleSetForcedReactions(network::Packet& packet) {
    if (!packet.hasRemaining(4)) return;
    uint32_t count = packet.readUInt32();
    if (count > 64) {
        LOG_WARNING("SMSG_SET_FORCED_REACTIONS: suspicious count ", count, ", ignoring");
        packet.skipAll();
        return;
    }
    forcedReactions_.clear();
    for (uint32_t i = 0; i < count; ++i) {
        if (!packet.hasRemaining(8)) break;
        uint32_t factionId = packet.readUInt32();
        uint32_t reaction  = packet.readUInt32();
        forcedReactions_[factionId] = static_cast<uint8_t>(reaction);
        LOG_INFO("SMSG_SET_FORCED_REACTIONS: faction ", factionId, " rank ", reaction);
    }
    LOG_INFO("SMSG_SET_FORCED_REACTIONS: ", forcedReactions_.size(), " faction overrides");
    owner_.refreshUnitHostility();
}

// ============================================================
// Per-frame update
// ============================================================

void CombatHandler::updateAutoAttack(float deltaTime) {
    if (auto* ac = owner_.services().audioCoordinator) {
        if (auto* csm = ac->getCombatSoundManager()) {
            bindCombatAudioCallbacks();
            csm->update(deltaTime);
        }
    }

    // Decrement range warn cooldown
    if (autoAttackRangeWarnCooldown_ > 0.0f) {
        autoAttackRangeWarnCooldown_ = std::max(0.0f, autoAttackRangeWarnCooldown_ - deltaTime);
    }

    // CMaNGOS reports swing errors only when they change; the original says
    // "You are too far away!" again every 4 seconds while the attack stays out
    // of reach (2.4.3, /startattack 26.5 to 40 yards from a boar with Onyxia's
    // reach: 4.00 to 4.02 s after the server's). Attack intent is unchanged.
    // A successful swing, another swing error, or stopping clears the latch.
    if (autoAttackRangeRepeat_ > 0.0f)
        autoAttackRangeRepeat_ = std::max(0.0f, autoAttackRangeRepeat_ - deltaTime);
    if (autoRangedSwitchCooldown_ > 0.0f)
        autoRangedSwitchCooldown_ = std::max(0.0f, autoRangedSwitchCooldown_ - deltaTime);
    if (autoAttackRequested_ && autoAttackOutOfRange_ && autoAttackRangeRepeat_ <= 0.0f) {
        owner_.errorGlobal("ERR_BADATTACKPOS");
        autoAttackRangeRepeat_ = kSwingRangeRepeatSeconds;
    }

    // Native TBC 2.4.3 CVar "autoRangedCombat" (default "1"): automatically
    // switches between ranged Auto Shot (75) and melee auto-attack as distance
    // crosses the melee/ranged boundary.
    if (autoRangedSwitchCooldown_ <= 0.0f && canUseAutoRangedCombat()) {
        auto* sh = owner_.getSpellHandler();
        const uint32_t autoRepeat = sh ? sh->getAutoRepeatSpellId() : 0u;
        if (autoRepeat == 75u && !autoAttackRequested_) {
            const uint64_t tgt = owner_.getTargetGuid();
            if (tgt != 0 && tgt != owner_.getPlayerGuid()) {
                auto entity = owner_.getEntityManager().getEntity(tgt);
                auto* unit = dynamic_cast<Unit*>(entity.get());
                const bool alive = unit && !(unit->getHealth() == 0 && unit->getMaxHealth() > 0);
                if (alive && owner_.canPlayerAttackUnit(*unit)) {
                    const auto meleeReach = owner_.unitWithinMeleeReach(tgt);
                    const auto bowRange = owner_.unitSpellRange(75u, tgt);
                    if ((meleeReach && *meleeReach) ||
                        (bowRange && *bowRange == spellclass::SpellRangeCheck::TooClose)) {
                        sh->cancelAutoRepeatSpell();
                        startAutoAttack(tgt, /*forceMelee=*/true);
                        autoRangedSwitchCooldown_ = 0.35f;
                    }
                }
            }
        } else if (autoAttackRequested_ && autoAttackTarget_ != 0 && autoRepeat == 0u) {
            const uint64_t tgt = autoAttackTarget_;
            const auto meleeReach = owner_.unitWithinMeleeReach(tgt);
            const auto bowRange = owner_.unitSpellRange(75u, tgt);
            if ((meleeReach && !*meleeReach) &&
                (bowRange && *bowRange == spellclass::SpellRangeCheck::InRange)) {
                stopMeleeAutoAttackOnly();
                sh->castSpell(75u, tgt);
                autoRangedSwitchCooldown_ = 0.35f;
            }
        }
    }

    // Do not cancel or resend attacks based on a guessed melee radius.
    // CMaNGOS keeps melee intent while out of range and reports swing errors.

    // An attacker's facing is not turned toward this player here: a standing
    // unit faces its UNIT_FIELD_TARGET (creatureTargetFacing), a moving one
    // its spline. Build 8606 turns the Eye of C'Thun with the server's facing
    // splines during Dark Glare, while it is still on this player's attacker
    // list but has cleared its target
    // (_experiments/20261118T1800Z.cthun_beams in the workspace root).
}

// ============================================================
// State management
// ============================================================

void CombatHandler::resetAllCombatState() {
    if (auto* ac = owner_.services().audioCoordinator) {
        if (auto* csm = ac->getCombatSoundManager()) {
            csm->clearPendingCombatEvents();
        }
    }
    hostileAttackers_.clear();
    combatText_.clear();
    autoAttacking_ = false;
    autoAttackRequested_ = false;
    enterCombatAnnounced_ = false;
    autoAttackTarget_ = 0;
    autoAttackOutOfRange_ = false;
    autoAttackRangeWarnCooldown_ = 0.0f;
    autoRangedSwitchCooldown_ = 0.0f;
    autoAttackRangeRepeat_ = 0.0f;
    lastMeleeSwingMs_ = 0;
    incomingHitDamage_ = -1;
}

void CombatHandler::removeHostileAttacker(uint64_t guid) {
    hostileAttackers_.erase(guid);
}

void CombatHandler::clearCombatText() {
    combatText_.clear();
}

void CombatHandler::removeCombatTextForGuid(uint64_t guid) {
    combatText_.erase(
        std::remove_if(combatText_.begin(), combatText_.end(),
            [guid](const CombatTextEntry& e) {
                return e.dstGuid == guid;
            }),
        combatText_.end());
}

// ============================================================
// Moved opcode handlers (from GameHandler::registerOpcodeHandlers)
// ============================================================

void CombatHandler::handleHealthUpdate(network::Packet& packet) {
    const bool huTbc = isActiveExpansion("tbc");
    if (!packet.hasRemaining(huTbc ? 8u : 2u) ) return;
    uint64_t guid = huTbc ? packet.readUInt64() : packet.readPackedGuid();
    if (!packet.hasRemaining(4)) return;
    uint32_t hp = packet.readUInt32();
    if (auto* unit = owner_.getUnitByGuid(guid)) unit->setHealth(hp);
    if (guid != 0) {
        auto unitId = owner_.guidToUnitId(guid);
        if (!unitId.empty()) owner_.fireAddonEvent("UNIT_HEALTH", {unitId});
    }
}

void CombatHandler::handlePowerUpdate(network::Packet& packet) {
    const bool puTbc = isActiveExpansion("tbc");
    if (!packet.hasRemaining(puTbc ? 8u : 2u) ) return;
    uint64_t guid = puTbc ? packet.readUInt64() : packet.readPackedGuid();
    if (!packet.hasRemaining(5)) return;
    uint8_t  powerType = packet.readUInt8();
    uint32_t value     = packet.readUInt32();
    if (auto* unit = owner_.getUnitByGuid(guid)) {
        unit->setPowerByType(powerType, value);
    }
    if (guid != 0) {
        auto unitId = owner_.guidToUnitId(guid);
        if (!unitId.empty()) {
            static constexpr const char* kPowerEvents[] = {
                "UNIT_MANA", "UNIT_RAGE", "UNIT_FOCUS", "UNIT_ENERGY", "UNIT_HAPPINESS"
            };
            if (powerType < std::size(kPowerEvents))
                owner_.fireAddonEvent(kPowerEvents[powerType], {unitId});
            // ACTIONBAR_UPDATE_USABLE comes from SpellHandler::updateTimers,
            // only when a button's answer changed. Power FCT (MANA/RAGE/FOCUS/ENERGY)
            // comes strictly from SMSG_SPELLENERGIZELOG / SMSG_PERIODICAURALOG,
            // never from melee rage swings or natural regeneration ticks.
            if (guid == owner_.getPlayerGuid()) {
                owner_.fireAddonEvent("SPELL_UPDATE_USABLE", {});
            }
        }
    }
}

void CombatHandler::handleUpdateComboPoints(network::Packet& packet) {
    // TBC also uses a packed GUID (CMaNGOS Player::SendComboPoints).
    if (!packet.hasRemaining(2)) return;
    uint64_t target = packet.readPackedGuid();
    if (!packet.hasRemaining(1)) return;
    const uint8_t oldPoints = owner_.comboPointsRef();
    const uint64_t oldTarget = owner_.comboTargetRef();
    const uint8_t newPoints = packet.readUInt8();
    owner_.comboPointsRef() = newPoints;
    owner_.comboTargetRef() = target;
    LOG_DEBUG("SMSG_UPDATE_COMBO_POINTS: target=0x", std::hex, target,
              std::dec, " points=", static_cast<int>(owner_.comboPointsRef()));
    owner_.fireAddonEvent("PLAYER_COMBO_POINTS", {});
    owner_.fireAddonEvent("SPELL_UPDATE_USABLE", {});
    // Warrior Overpower (SPELL_ATTR_EX_COMBO_ON_BLOCK = 0x40000000) reuses
    // SMSG_UPDATE_COMBO_POINTS(target, 1) on target dodge; announce SPELL_ACTIVE
    // to Blizzard_CombatText when the reactive window opens.
    if (newPoints > 0 && (oldPoints == 0 || target != oldTarget)) {
        std::unordered_set<std::string> announced;
        for (uint32_t spellId : owner_.getKnownSpells()) {
            const auto* entry = owner_.getSpellNameEntry(spellId);
            if (!entry || (entry->attr & 0x40u) != 0) continue;
            if ((entry->attrEx & 0x40000000u) == 0) continue;
            if (!entry->name.empty() && announced.insert(entry->name).second)
                owner_.fireAddonEvent("COMBAT_TEXT_UPDATE", {"SPELL_ACTIVE", entry->name});
        }
    }
}

void CombatHandler::handlePvpCredit(network::Packet& packet) {
    if (!packet.hasRemaining(16)) return;
    const uint32_t honor      = packet.readUInt32();
    const uint64_t victimGuid = packet.readUInt64();
    const uint32_t rank       = packet.readUInt32();
    LOG_INFO("SMSG_PVP_CREDIT: honor=", honor, " victim=0x", std::hex, victimGuid, std::dec, " rank=", rank);

    auto resolvePvpRankTitle = [&](uint32_t r, bool alliance, bool female) -> std::string {
        const uint32_t clamped = std::min(r, 19u);
        const std::string key = "PVP_RANK_" + std::to_string(clamped) + "_" +
                                (alliance ? "1" : "0") + (female ? "_FEMALE" : "");
        if (std::string gs = owner_.globalString(key.c_str()); !gs.empty()) {
            return gs;
        }
        static constexpr const char* kHordeRanks[20] = {
            "Scout", "Pariah", "Outlaw", "Exiled", "Dishonored",
            "Scout", "Grunt", "Sergeant", "Senior Sergeant", "First Sergeant",
            "Stone Guard", "Blood Guard", "Legionnaire", "Centurion", "Champion",
            "Lieutenant General", "General", "Warlord", "High Warlord", "Leader"
        };
        static constexpr const char* kAllianceRanks[20] = {
            "Private", "Pariah", "Outlaw", "Exiled", "Dishonored",
            "Private", "Corporal", "Sergeant", "Master Sergeant", "Sergeant Major",
            "Knight", "Knight-Lieutenant", "Knight-Captain", "Knight-Champion", "Lieutenant Commander",
            "Commander", "Marshal", "Field Marshal", "Grand Marshal", "Leader"
        };
        return alliance ? kAllianceRanks[clamped] : kHordeRanks[clamped];
    };

    std::string msg;
    if (victimGuid == 0) {
        if (std::string line = owner_.globalString("COMBATLOG_HONORAWARD"); !line.empty()) {
            char buf[256];
            std::snprintf(buf, sizeof(buf), line.c_str(), static_cast<int>(honor));
            msg = buf;
        } else {
            msg = "You have been awarded " + std::to_string(honor) + " honor points.";
        }
    } else {
        std::string victimName = owner_.lookupName(victimGuid);
        uint8_t race = owner_.lookupPlayerRace(victimGuid);
        uint8_t gender = owner_.lookupPlayerGender(victimGuid);
        if (auto entity = owner_.getEntityManager().getEntity(victimGuid)) {
            if (victimName.empty()) {
                if (auto* unit = dynamic_cast<Unit*>(entity.get())) {
                    victimName = unit->getName();
                }
            }
            const uint16_t bytes0Idx = fieldIndex(UF::UNIT_FIELD_BYTES_0);
            if (bytes0Idx != 0xFFFF) {
                const uint32_t bytes0 = entity->getField(bytes0Idx);
                if (race == 0) race = static_cast<uint8_t>(bytes0 & 0xFFu);
                if (gender == 0xFF) gender = static_cast<uint8_t>((bytes0 >> 16) & 0xFFu);
            }
        }
        if (victimName.empty()) {
            if (((victimGuid >> 48) & 0xFFFFu) == 0) {
                owner_.queryPlayerName(victimGuid);
            }
            victimName = "Unknown";
        }
        bool victimAlliance = !owner_.isPlayerAlliance();
        if (race != 0) {
            victimAlliance = (race == 1 || race == 3 || race == 4 || race == 7 || race == 11);
        }
        const std::string rankTitle = resolvePvpRankTitle(rank, victimAlliance, gender == 1);
        char buf[320];
        if (std::string pat = owner_.globalString("COMBATLOG_HONORGAIN"); !pat.empty()) {
            std::snprintf(buf, sizeof(buf), pat.c_str(), victimName.c_str(), rankTitle.c_str(),
                          static_cast<int>(honor));
        } else {
            std::snprintf(buf, sizeof(buf), "%s dies, honorable kill Rank: %s (%u Honor Points)",
                          victimName.c_str(), rankTitle.c_str(), honor);
        }
        msg = buf;
    }

    owner_.addLocalChatLine(ChatType::COMBAT_HONOR_GAIN, msg);
    if (honor > 0) addCombatText(CombatTextEntry::HONOR_GAIN, static_cast<int32_t>(honor), 0, true);
}

void CombatHandler::handleProcResist(network::Packet& packet) {
    const bool prUsesFullGuid = isActiveExpansion("tbc");
    auto readPrGuid = [&]() -> uint64_t {
        if (prUsesFullGuid)
            return (packet.hasRemaining(8)) ? packet.readUInt64() : 0;
        return packet.readPackedGuid();
    };
    if (!packet.hasRemaining(prUsesFullGuid ? 8u : 1u)             || (!prUsesFullGuid && !packet.hasFullPackedGuid())) { packet.skipAll(); return; }
    uint64_t caster = readPrGuid();
    if (!packet.hasRemaining(prUsesFullGuid ? 8u : 1u)             || (!prUsesFullGuid && !packet.hasFullPackedGuid())) { packet.skipAll(); return; }
    uint64_t victim = readPrGuid();
    if (!packet.hasRemaining(4)) return;
    uint32_t spellId = packet.readUInt32();
    if (victim == owner_.getPlayerGuid())       addCombatText(CombatTextEntry::RESIST, 0, spellId, false, 0, caster, victim, 0, false, false);
    else if (caster == owner_.getPlayerGuid())  addCombatText(CombatTextEntry::RESIST, 0, spellId, true,  0, caster, victim, 0, false, false);
    packet.skipAll();
}

// ============================================================
// Environmental / reflect / immune / resist
// ============================================================


void CombatHandler::handleSpellDamageShield(network::Packet& packet) {
    // Classic: packed_guid victim + packed_guid caster + spellId(4) + damage(4) + schoolMask(4)
    // TBC:     uint64 victim + uint64 caster + spellId(4) + damage(4) + schoolMask(4)
    // WotLK:   packed_guid victim + packed_guid caster + spellId(4) + damage(4) + absorbed(4) + schoolMask(4)
    const bool shieldTbc = isActiveExpansion("tbc");
    const bool shieldWotlkLike = !isClassicLikeExpansion() && !shieldTbc;
    const auto shieldRem = [&]() { return packet.getRemainingSize(); };
    const size_t shieldMinSz = shieldTbc ? 24u : 2u;
    if (!packet.hasRemaining(shieldMinSz)) {
        packet.skipAll(); return;
    }
    if (!shieldTbc && (!packet.hasFullPackedGuid())) {
        packet.skipAll(); return;
    }
    uint64_t victimGuid = shieldTbc
        ? packet.readUInt64() : packet.readPackedGuid();
    if (!packet.hasRemaining(shieldTbc ? 8u : 1u)             || (!shieldTbc && !packet.hasFullPackedGuid())) {
        packet.skipAll(); return;
    }
    uint64_t casterGuid = shieldTbc
        ? packet.readUInt64() : packet.readPackedGuid();
    const size_t shieldTailSize = shieldWotlkLike ? 16u : 12u;
    if (shieldRem() < shieldTailSize) {
        packet.skipAll(); return;
    }
    uint32_t shieldSpellId = packet.readUInt32();
    uint32_t damage        = packet.readUInt32();
    if (shieldWotlkLike)
        /*uint32_t absorbed =*/ packet.readUInt32();
    const uint32_t school = packet.readUInt32();
    // Show combat text: damage shield reflect
    if (casterGuid == owner_.getPlayerGuid()) {
        // We have a damage shield that reflected damage
        addCombatText(CombatTextEntry::SPELL_DAMAGE, static_cast<int32_t>(damage), shieldSpellId, true, 0, casterGuid, victimGuid, school);
    } else if (victimGuid == owner_.getPlayerGuid()) {
        // A damage shield hit us (e.g. target's Thorns)
        addCombatText(CombatTextEntry::SPELL_DAMAGE, static_cast<int32_t>(damage), shieldSpellId, false, 0, casterGuid, victimGuid, school);
    }
}

void CombatHandler::handleSpellOrDamageImmune(network::Packet& packet) {
    // WotLK/Classic/Turtle: packed casterGuid + packed victimGuid + uint32 spellId + uint8 saveType
    // TBC:                  full uint64 casterGuid + full uint64 victimGuid + uint32 + uint8
    const bool immuneUsesFullGuid = isActiveExpansion("tbc");
    const size_t minSz = immuneUsesFullGuid ? 21u : 2u;
    if (!packet.hasRemaining(minSz)) {
        packet.skipAll(); return;
    }
    if (!immuneUsesFullGuid && !packet.hasFullPackedGuid()) {
        packet.skipAll(); return;
    }
    uint64_t casterGuid = immuneUsesFullGuid
        ? packet.readUInt64() : packet.readPackedGuid();
    if (!packet.hasRemaining(immuneUsesFullGuid ? 8u : 2u)             || (!immuneUsesFullGuid && !packet.hasFullPackedGuid())) {
        packet.skipAll(); return;
    }
    uint64_t victimGuid = immuneUsesFullGuid
        ? packet.readUInt64() : packet.readPackedGuid();
    if (!packet.hasRemaining(5)) return;
    uint32_t immuneSpellId = packet.readUInt32();
    /*uint8_t saveType =*/ packet.readUInt8();
    // Show IMMUNE text when the player is the caster (we hit an immune target)
    // or the victim (we are immune)
    if (casterGuid == owner_.getPlayerGuid() || victimGuid == owner_.getPlayerGuid()) {
        addCombatText(CombatTextEntry::IMMUNE, 0, immuneSpellId,
                      casterGuid == owner_.getPlayerGuid(), 0, casterGuid, victimGuid, 0, false, false);
    }
}

void CombatHandler::handleResistLog(network::Packet& packet) {
    // WotLK/Classic/Turtle: uint32 hitInfo + packed_guid attacker + packed_guid victim + uint32 spellId
    //                      + float resistFactor + uint32 targetRes + uint32 resistedValue + ...
    // TBC:                 same layout but full uint64 GUIDs
    // Show RESIST combat text when player resists an incoming spell.
    const bool rlUsesFullGuid = isActiveExpansion("tbc");
    auto rl_rem = [&]() { return packet.getRemainingSize(); };
    if (rl_rem() < 4) { packet.skipAll(); return; }
    /*uint32_t hitInfo =*/ packet.readUInt32();
    if (rl_rem() < (rlUsesFullGuid ? 8u : 1u)
        || (!rlUsesFullGuid && !packet.hasFullPackedGuid())) {
        packet.skipAll(); return;
    }
    uint64_t attackerGuid = rlUsesFullGuid
        ? packet.readUInt64() : packet.readPackedGuid();
    if (rl_rem() < (rlUsesFullGuid ? 8u : 1u)
        || (!rlUsesFullGuid && !packet.hasFullPackedGuid())) {
        packet.skipAll(); return;
    }
    uint64_t victimGuid = rlUsesFullGuid
        ? packet.readUInt64() : packet.readPackedGuid();
    if (rl_rem() < 4) { packet.skipAll(); return; }
    uint32_t spellId = packet.readUInt32();
    // Resist payload includes:
    // float resistFactor + uint32 targetResistance + uint32 resistedValue.
    // Require the full payload so truncated packets cannot synthesize
    // zero-value resist events.
    if (rl_rem() < 12) { packet.skipAll(); return; }
    /*float resistFactor =*/ packet.readFloat();
    /*uint32_t targetRes =*/ packet.readUInt32();
    int32_t resistedAmount = static_cast<int32_t>(packet.readUInt32());
    // Show RESIST when the player is involved on either side.
    if (resistedAmount > 0 && victimGuid == owner_.getPlayerGuid()) {
        addCombatText(CombatTextEntry::RESIST, resistedAmount, spellId, false, 0, attackerGuid, victimGuid, 0, false, false);
    } else if (resistedAmount > 0 && attackerGuid == owner_.getPlayerGuid()) {
        addCombatText(CombatTextEntry::RESIST, resistedAmount, spellId, true, 0, attackerGuid, victimGuid, 0, false, false);
    }
    packet.skipAll();
}

// ============================================================
// Pet feedback
// ============================================================

void CombatHandler::handlePetTameFailure(network::Packet& packet) {
    // PetTameFailureReason (CMaNGOS SharedDefines.h) values are the
    // GlobalStrings PETTAME_* names (not observed natively).
    static constexpr const char* kReasons[] = {nullptr,
        "PETTAME_INVALIDCREATURE", "PETTAME_TOOMANY", "PETTAME_CREATUREALREADYOWNED",
        "PETTAME_NOTTAMEABLE", "PETTAME_ANOTHERSUMMONACTIVE", "PETTAME_UNITSCANTTAME",
        "PETTAME_NOPETAVAILABLE", "PETTAME_INTERNALERROR", "PETTAME_TOOHIGHLEVEL",
        "PETTAME_DEAD", "PETTAME_NOTDEAD", "PETTAME_UNKNOWNERROR"};
    if (packet.hasRemaining(1)) {
        const uint8_t reason = packet.readUInt8();
        if (reason > 0 && reason < std::size(kReasons)) owner_.errorGlobal(kReasons[reason]);
        else LOG_WARNING("SMSG_PET_TAME_FAILURE: unknown reason ", int(reason));
    }
}

void CombatHandler::handlePetActionFeedback(network::Packet& packet) {
    // ActionFeedback (CMaNGOS Pet.h): 1 pet dead is ERR_PET_SPELL_DEAD;
    // TBC GlobalStrings has no line for 2-4 (nothing to attack, can't attack
    // target, no path), so they print nothing (not observed natively).
    if (!packet.hasRemaining(1)) return;
    const uint8_t msg = packet.readUInt8();
    if (msg == 1) owner_.errorGlobal("ERR_PET_SPELL_DEAD");
    else LOG_DEBUG("SMSG_PET_ACTION_FEEDBACK ", int(msg));
    packet.skipAll();
}

void CombatHandler::handlePetActionSound(network::Packet& packet) {
    // CMaNGOS Unit::SendPetTalk: uint64 petGuid + uint32 petTalk
    // (0 = PET_TALK_SPECIAL_SPELL -> PetOrder col 29, 1 = PET_TALK_ATTACK -> PetAttack col 28)
    if (!packet.hasRemaining(12)) { packet.skipAll(); return; }
    const uint64_t petGuid = packet.readUInt64();
    const uint32_t petTalk = packet.readUInt32();
    LOG_DEBUG("SMSG_PET_ACTION_SOUND: petGuid=0x", std::hex, petGuid, std::dec, " petTalk=", petTalk);
    if (petActionSoundCallback_) {
        petActionSoundCallback_(petGuid, petTalk);
    }
    packet.skipAll();
}

void CombatHandler::handlePetDismissSound(network::Packet& packet) {
    // CMaNGOS Unit::SendPetDismiss: uint32 creatureSoundDataId + float x + float y + float z
    if (!packet.hasRemaining(16)) { packet.skipAll(); return; }
    const uint32_t creatureSoundDataId = packet.readUInt32();
    const float x = packet.readFloat();
    const float y = packet.readFloat();
    const float z = packet.readFloat();
    LOG_DEBUG("SMSG_PET_DISMISS_SOUND: creatureSoundDataId=", creatureSoundDataId,
              " pos=(", x, ",", y, ",", z, ")");
    if (creatureSoundDataId != 0 && petDismissSoundCallback_) {
        petDismissSoundCallback_(creatureSoundDataId, glm::vec3(x, y, z));
    }
    packet.skipAll();
}

void CombatHandler::handlePetCastFailed(network::Packet& packet) {
    // WotLK: castCount(1) + spellId(4) + reason(1)
    // Classic/TBC: spellId(4) + reason(1) (no castCount)
    const bool hasCount = isActiveExpansion("wotlk");
    const size_t minSize = hasCount ? 6u : 5u;
    if (packet.hasRemaining(minSize)) {
        if (hasCount) /*uint8_t castCount =*/ packet.readUInt8();
        uint32_t spellId   = packet.readUInt32();
        uint8_t  rawReason = (packet.hasRemaining(1))
                                 ? packet.readUInt8() : 0;
        const uint8_t reason = owner_.getPacketParsers()->castResult(rawReason);
        uint32_t arg1 = 0, arg2 = 0;
        readCastResultArgs(packet, reason, arg1, arg2);
        LOG_DEBUG("SMSG_PET_CAST_FAILED: spell=", spellId,
                  " reason=", static_cast<int>(rawReason), " mapped=", static_cast<int>(reason));
        // The same GlobalStrings line as the player's own failure, on the
        // same error line. Not yet compared with the original for a pet.
        if (reason != 0) {
            if (const std::string text = castFailureText(owner_, spellId, reason, arg1, arg2); !text.empty())
                owner_.addUIError(text, /*playUiErrorSound=*/false);
        }
    }
    packet.skipAll();
}

void CombatHandler::handlePetBroken(network::Packet& packet) {
    // Pet bond broken (died or forcibly dismissed) - clear pet state
    owner_.petGuidRef() = 0;
    owner_.petSpellListRef().clear();
    owner_.petAutocastSpellsRef().clear();
    memset(owner_.petActionSlotsRef(), 0, sizeof(owner_.petActionSlotsRef()));
    // Sent when the pet abandons its owner (CMaNGOS Pet loyalty):
    // ERR_PET_BROKEN, "Your pet has run away" (not observed natively).
    owner_.systemGlobal("ERR_PET_BROKEN");
    // The same pair SMSG_PET_SPELLS fires, because the same two things have to
    // be redrawn - the pet frame and the pet action bar. Losing the pet
    // outright announced nothing while *learning one spell* announced
    // PET_BAR_UPDATE, so a dead pet's frame and its whole ability bar stayed on
    // screen until the next login.
    owner_.fireAddonEvent("UNIT_PET", {"player"});
    owner_.fireAddonEvent("PET_UI_UPDATE", {});
    LOG_INFO("SMSG_PET_BROKEN: pet bond broken");
    packet.skipAll();
}

void CombatHandler::handlePetLearnedSpell(network::Packet& packet) {
    if (packet.hasRemaining(4)) {
        uint32_t spellId = packet.readUInt32();
        owner_.petSpellListRef().push_back(spellId);
        const std::string& sname = owner_.getSpellName(spellId);
        // No line: TBC GlobalStrings has none for a pet learning a spell.
        (void)sname;
        LOG_DEBUG("SMSG_PET_LEARNED_SPELL: spellId=", spellId);
        owner_.fireAddonEvent("PET_BAR_UPDATE", {});
    }
    packet.skipAll();
}

void CombatHandler::handlePetUnlearnedSpell(network::Packet& packet) {
    if (packet.hasRemaining(4)) {
        uint32_t spellId = packet.readUInt32();
        owner_.petSpellListRef().erase(
            std::remove(owner_.petSpellListRef().begin(), owner_.petSpellListRef().end(), spellId),
            owner_.petSpellListRef().end());
        owner_.petAutocastSpellsRef().erase(spellId);
        LOG_DEBUG("SMSG_PET_UNLEARNED_SPELL: spellId=", spellId);
    }
    packet.skipAll();
}

void CombatHandler::handlePetMode(network::Packet& packet) {
    // uint64 petGuid, uint32 mode
    // mode bits: low byte = command state, next byte = react state
    if (packet.hasRemaining(12)) {
        uint64_t modeGuid = packet.readUInt64();
        uint32_t mode     = packet.readUInt32();
        if (modeGuid == owner_.petGuidRef() ||
            (owner_.getPossessedGuid() != 0 && modeGuid == owner_.getPossessedGuid())) {
            // COMMAND_ATTACK is 2. The pet frame lights its attack indicator on
            // the start and clears it on the stop, so only the crossing matters
            // - this packet arrives for every mode change, and firing on each
            // would relight an indicator that is already lit.
            constexpr uint8_t kCommandAttack = 2;
            const bool wasAttacking = owner_.petCommandRef() == kCommandAttack;
            owner_.petCommandRef() = static_cast<uint8_t>(mode & 0xFF);
            owner_.petReactRef()   = static_cast<uint8_t>((mode >> 8) & 0xFF);
            const bool nowAttacking = owner_.petCommandRef() == kCommandAttack;
            if (nowAttacking != wasAttacking && owner_.addonEventCallbackRef()) {
                owner_.addonEventCallbackRef()(
                    nowAttacking ? "PET_ATTACK_START" : "PET_ATTACK_STOP", {});
            }
            owner_.fireAddonEvent("PET_BAR_UPDATE", {});
            owner_.fireAddonEvent("ACTIONBAR_UPDATE_STATE", {});
            LOG_DEBUG("SMSG_PET_MODE: command=", static_cast<int>(owner_.petCommandRef()),
                      " react=", static_cast<int>(owner_.petReactRef()));
        }
    }
    packet.skipAll();
}

// ============================================================
// Resurrect
// ============================================================

void CombatHandler::handleResurrectFailed(network::Packet& packet) {
    if (packet.hasRemaining(4)) {
        uint32_t reason = packet.readUInt32();
        // TBC GlobalStrings has no resurrection-failed line; nothing printed.
        LOG_WARNING("SMSG_RESURRECT_FAILED: reason ", reason);
    }
}

// ============================================================
// Targeting
// ============================================================

bool CombatHandler::isSelectableUnit(uint64_t guid) const {
    // A game object is not a target, whoever asks.
    //
    // WoW selects units, players and corpses; a door, a mailbox, a chest or a
    // spell's ground effect is used or walked into, and none of them can be
    // the target. Refused here rather than at the click, because every way in
    // reaches this - the click, tab, /target, a macro - and each one that
    // targeted an object put a selection circle on the floor around it and
    // sent CMSG_SET_SELECTION for a guid that is not a unit.
    if (auto entity = owner_.getEntityManager().getEntity(guid)) {
        const auto type = entity->getType();
        if (type == ObjectType::GAMEOBJECT || type == ObjectType::DYNAMICOBJECT) {
            return false;
        }
        if (entity->isUnit()) {
            const auto* unit = static_cast<const Unit*>(entity.get());
            if ((unit->getUnitFlags() & UNIT_FLAG_NOT_SELECTABLE) != 0) {
                return false;
            }
        }
    }

    // Everything the player clicks is selectable; the server arbitrates whether a
    // corpse actually holds loot when we send CMSG_LOOT. We deliberately do NOT
    // gate dead creature corpses on UNIT_DYNFLAG_LOOTABLE.
    //
    // The reason given here was that the bit is computed per-viewer and goes
    // stale across a death/resurrect cycle, and that gating on it left a
    // genuinely lootable corpse permanently unclickable. The symptom was real;
    // the explanation was not the whole of it. UNIT_DYNAMIC_FLAGS was being
    // read from field 147 - which is UNIT_FIELD_PADDING - where the server
    // writes 79, so the bit was never right in the first place. Fixed
    // 2026-08-05.
    //
    // Not re-gated here, because that would be trading a working behaviour for
    // a theory. Anyone who wants the gate back should first confirm against a
    // live corpse that the flag now reads what it should.
    // Empty corpses remain selectable until the server removes them.
    return true;
}

void CombatHandler::setTarget(uint64_t guid) {
    if (guid == owner_.getTargetGuid()) return;
    // Apply the same unit-selection rules as scene picking.
    if (!isSelectableUnit(guid)) return;

    // Save previous target - once as "the last one whatever it was", which is
    // what TargetLastTarget flips back to, and once under its kind, which is
    // what makes TargetLastEnemy and TargetLastFriend worth having: a healer's
    // last friendly target survives half a fight's worth of tab-targeting.
    if (owner_.getTargetGuid() != 0) {
        const uint64_t previous = owner_.getTargetGuid();
        owner_.lastTargetGuidRef() = previous;
        auto entity = owner_.getEntityManager().getEntity(previous);
        if (auto* unit = dynamic_cast<Unit*>(entity.get())) {
            if (!owner_.isFriendlyFaction(unit->getFactionTemplate()) ||
                isAggressiveTowardPlayer(previous)) {
                lastEnemyTargetGuid_ = previous;
            } else {
                lastFriendTargetGuid_ = previous;
            }
        }
    }

    // Selecting something else gives up the corpse being looted. Original
    // 2.4.3 with corpse A's loot open: a right-click on corpse B sends
    // CMSG_LOOT_RELEASE A, CMSG_SET_SELECTION 0 (the release dropping A as
    // target, see closeLoot), CMSG_SET_SELECTION B, CMSG_LOOT B; F1 the same
    // with the player's guid. CMaNGOS does not release A when B is looted and
    // answers neither, so the client must, and B's window opens on a frame
    // that has closed: LootFrame only rebuilds its buttons in OnShow.
    releaseLootOfTarget();

    // Stop Auto Attack On Target Change, which the panel offers and nothing
    // read. Changing target left the swing running - correct for the setting's
    // default, and the only behaviour available whatever it said.
    //
    // Only when the target actually changes, and only when something is being
    // swung at: re-selecting the same unit is not a change, and stopping an
    // attack nobody started would send a cancel for nothing.
    if (guid != owner_.getTargetGuid() &&
        addons::storedCVarValue("stopAutoAttackOnTargetChange", "0") != "0") {
        if (autoAttackRequested_) {
            stopAutoAttack();
        }
        if (owner_.getSpellHandler() && owner_.getSpellHandler()->getAutoRepeatSpellId() != 0) {
            owner_.getSpellHandler()->cancelAutoRepeatSpell();
        }
    }

    owner_.setTargetGuidRaw(guid);
    if (owner_.getSpellHandler()) owner_.getSpellHandler()->onTargetChanged(guid);

    // TBC sends visible auras in object fields, before selection. Preserve
    // that authoritative cache instead of waiting for a WotLK-only update.
    if (owner_.getSpellHandler()) owner_.getSpellHandler()->restoreTargetAuras(guid);

    // Clear previous target's cast bar on target change
    // (the new target's cast state is naturally fetched from spellHandler_->unitCastStates_ by GUID)

    // Inform server of target selection
    if (owner_.isInWorld()) {
        auto packet = SetSelectionPacket::build(guid);
        owner_.getSocket()->send(packet);
    }

    if (guid != 0) {
        LOG_INFO("Target set: 0x", std::hex, guid, std::dec);
    }
    owner_.fireAddonEvent("PLAYER_TARGET_CHANGED", {});

    // Report the interface once, the first time there is a target to report it
    // against. A target frame is only right or wrong with something targeted,
    // and that is not a moment anything can be scheduled for - which is why
    // this one has been diagnosed from readings taken when nothing was
    // selected, where correct and broken look identical.
    static bool reportedOnce = false;
    if (!reportedOnce && guid != 0) {
        reportedOnce = true;
        ui::frameXmlRequestCheck();
        // What UnitExists("target") resolves through, said plainly. The Lua
        // probe was meant to answer this and has not run; this is the same
        // question asked where it can actually be answered - the interface
        // reports "target" as existing exactly when this guid resolves to a
        // unit.
        auto entity = owner_.getEntityManager().getEntity(guid);
        LOG_WARNING("first target: guid=0x", std::hex, guid, std::dec,
                    " entity=", (entity ? "found" : "MISSING"),
                    " unit=", (entity && dynamic_cast<Unit*>(entity.get())
                                   ? "yes" : "NO"));
    }
}

void CombatHandler::releaseLootOfTarget() {
    const uint64_t current = owner_.getTargetGuid();
    if (current != 0 && owner_.isLootWindowOpen() &&
        owner_.getCurrentLoot().lootGuid == current) {
        owner_.closeLoot();
    }
}

void CombatHandler::clearTarget() {
    // Deselecting the looted corpse (a click on empty ground) closes its loot
    // too. Original: CMSG_LOOT_RELEASE, then CMSG_SET_SELECTION 0 twice - the
    // release's own and the deselect's.
    releaseLootOfTarget();
    stopAutoAttack();
    if (owner_.isInWorld()) {
        auto packet = SetSelectionPacket::build(0);
        owner_.getSocket()->send(packet);
    }
    if (owner_.getTargetGuid() != 0) {
        LOG_INFO("Target cleared");
        // Zero the GUID before firing the event so callbacks/addons that query
        // the current target see null (consistent with setTarget which updates
        // targetGuid before the event).
        owner_.setTargetGuidRaw(0);
        owner_.fireAddonEvent("PLAYER_TARGET_CHANGED", {});
    } else {
        owner_.setTargetGuidRaw(0);
    }
    owner_.tabCycleIndexRef() = -1;
    owner_.tabCycleStaleRef() = true;
}

std::shared_ptr<Entity> CombatHandler::getTarget() const {
    if (owner_.getTargetGuid() == 0) return nullptr;
    return owner_.getEntityManager().getEntity(owner_.getTargetGuid());
}

void CombatHandler::setFocus(uint64_t guid) {
    if (guid != 0 && !isSelectableUnit(guid)) return;
    owner_.focusGuidRef() = guid;
    owner_.fireAddonEvent("PLAYER_FOCUS_CHANGED", {});
    if (guid != 0) {
        auto entity = owner_.getEntityManager().getEntity(guid);
        if (entity) {
            std::string name;
            auto unit = std::dynamic_pointer_cast<Unit>(entity);
            if (unit && !unit->getName().empty()) {
                name = unit->getName();
            }
            if (name.empty()) name = owner_.lookupName(guid);
            if (name.empty()) name = "Unknown";
            owner_.addSystemChatMessage("Focus set: " + name);
            LOG_INFO("Focus set: 0x", std::hex, guid, std::dec);
        }
    }
}

void CombatHandler::clearFocus() {
    if (owner_.focusGuidRef() != 0) {
        owner_.addSystemChatMessage("Focus cleared.");
        LOG_INFO("Focus cleared");
    }
    owner_.focusGuidRef() = 0;
    owner_.fireAddonEvent("PLAYER_FOCUS_CHANGED", {});
}

std::shared_ptr<Entity> CombatHandler::getFocus() const {
    if (owner_.focusGuidRef() == 0) return nullptr;
    return owner_.getEntityManager().getEntity(owner_.focusGuidRef());
}

void CombatHandler::setMouseoverGuid(uint64_t guid) {
    if (owner_.mouseoverGuidRef() != guid) {
        owner_.mouseoverGuidRef() = guid;
        owner_.fireAddonEvent("UPDATE_MOUSEOVER_UNIT", {});
    }
}

void CombatHandler::targetLastTarget() {
    if (owner_.lastTargetGuidRef() == 0) {
        return;
    }

    // Swap current and last target
    uint64_t temp = owner_.getTargetGuid();
    setTarget(owner_.lastTargetGuidRef());
    owner_.lastTargetGuidRef() = temp;
}

void CombatHandler::cycleTarget(bool reverse,
                                const std::function<bool(uint64_t, Entity&)>& wanted) {
    // The rule is in tab_target.hpp with the measurements it came from.
    auto* renderer = owner_.services().renderer;
    const bool checkScreen = renderer && renderer->getCamera();
    const glm::mat4 viewProjection = checkScreen ? renderer->getCamera()->getViewProjectionMatrix()
                                                 : glm::mat4(1.0f);

    const auto& movement = owner_.getMovementInfo();
    const glm::vec3 player(movement.x, movement.y, movement.z);

    std::vector<TabCandidate> candidates;
    for (const auto& [guid, entity] : owner_.getEntityManager().getEntities()) {
        if (!entity || guid == owner_.getPlayerGuid()) continue;
        const glm::vec3 pos(entity->getX(), entity->getY(), entity->getZ());
        if (!isInTabRange(player, pos) || !isInTabCone(player, movement.orientation, pos)) continue;
        if (checkScreen && !isOnScreen(viewProjection, core::coords::canonicalToRender(pos))) continue;
        if (!wanted(guid, *entity)) continue;
        candidates.push_back({.guid = guid, .distance = glm::distance(player, pos)});
    }

    const uint64_t nowMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    const bool restart = nowMs - lastTabTargetMs_ > kTabCycleResetMs;
    lastTabTargetMs_ = nowMs;

    const uint64_t next = pickTabTarget(std::move(candidates), owner_.getTargetGuid(), restart, reverse);
    if (next == 0) {
        return;
    }
    setTarget(next);
}

bool CombatHandler::isGroupMemberGuid(uint64_t guid) const {
    for (const auto& m : owner_.getPartyData().members) {
        if (m.guid == guid) return true;
    }
    return false;
}

void CombatHandler::targetEnemy(bool reverse) {
    // Corpses are never candidates: native skipped a creature the player had
    // just killed.
    cycleTarget(reverse, [this](uint64_t guid, Entity& e) {
        const auto t = e.getType();
        if (t != ObjectType::UNIT && t != ObjectType::PLAYER) return false;
        auto* unit = dynamic_cast<Unit*>(&e);
        if (!unit || unit->getHealth() == 0) return false;
        return isEnemyTarget(guid, *unit);
    });
}

uint64_t CombatHandler::findNearestEnemy() const {
    auto* renderer = owner_.services().renderer;
    const bool checkScreen = renderer && renderer->getCamera();
    const glm::mat4 viewProjection = checkScreen ? renderer->getCamera()->getViewProjectionMatrix()
                                                 : glm::mat4(1.0f);

    const auto& movement = owner_.getMovementInfo();
    const glm::vec3 player(movement.x, movement.y, movement.z);

    std::vector<TabCandidate> candidates;
    for (const auto& [guid, entity] : owner_.getEntityManager().getEntities()) {
        if (!entity || guid == owner_.getPlayerGuid()) continue;
        const auto t = entity->getType();
        if (t != ObjectType::UNIT && t != ObjectType::PLAYER) continue;
        auto* unit = dynamic_cast<Unit*>(entity.get());
        if (!unit || unit->getHealth() == 0) continue;
        if (!isEnemyTarget(guid, *unit)) continue;
        const glm::vec3 pos(entity->getX(), entity->getY(), entity->getZ());
        if (!isInTabRange(player, pos) || !isInTabCone(player, movement.orientation, pos)) continue;
        if (checkScreen && !isOnScreen(viewProjection, core::coords::canonicalToRender(pos))) continue;
        candidates.push_back({.guid = guid, .distance = glm::distance(player, pos)});
    }

    return pickTabTarget(std::move(candidates), 0, /*restart=*/true, /*reverse=*/false);
}

uint64_t CombatHandler::acquireAttackTarget(uint64_t explicitTargetGuid) {
    if (owner_.isPlayerDead()) {
        owner_.errorGlobal("ERR_ATTACK_DEAD");
        return 0;
    }

    const uint64_t current = explicitTargetGuid != 0 ? explicitTargetGuid : owner_.getTargetGuid();
    if (current == 0) {
        if (const uint64_t found = findNearestEnemy(); found != 0) {
            setTarget(found);
            return found;
        }
        owner_.errorGlobal("ERR_NO_ATTACK_TARGET");
        return 0;
    }

    // Self or friendly target: native 2.4.3 auto-switches to the nearest enemy
    // in cone when no explicit unit was named, else refuses with
    // "You cannot attack that target." (ERR_INVALID_ATTACK_TARGET).
    if (current == owner_.getPlayerGuid()) {
        if (explicitTargetGuid == 0) {
            if (const uint64_t found = findNearestEnemy(); found != 0) {
                setTarget(found);
                return found;
            }
        }
        owner_.errorGlobal("ERR_INVALID_ATTACK_TARGET");
        return 0;
    }

    if (auto entity = owner_.getEntityManager().getEntity(current)) {
        const auto t = entity->getType();
        auto* unit = dynamic_cast<Unit*>(entity.get());
        if (!unit || (t != ObjectType::UNIT && t != ObjectType::PLAYER)) {
            owner_.errorGlobal("ERR_INVALID_ATTACK_TARGET");
            return 0;
        }
        // Dead corpse selected: native 2.4.3 does NOT auto-switch to another
        // enemy; it keeps the corpse targeted and raises ERR_INVALID_ATTACK_TARGET.
        if (unit->getHealth() == 0) {
            owner_.errorGlobal("ERR_INVALID_ATTACK_TARGET");
            return 0;
        }
        if (!isEnemyTarget(current, *unit)) {
            // Assist Attack: when attacking a friendly unit that has a hostile
            // target, let startAutoAttack handle assistAttack if enabled.
            if (addons::storedCVarValue("assistAttack", "0") != "0") {
                const uint64_t theirTarget = unitTargetGuid(entity);
                if (auto their = owner_.getEntityManager().getEntity(theirTarget)) {
                    if (auto* theirUnit = dynamic_cast<Unit*>(their.get());
                        theirUnit && theirUnit->getHealth() > 0 &&
                        (theirUnit->isHostile() || isAggressiveTowardPlayer(theirTarget))) {
                        return current;
                    }
                }
            }
            if (explicitTargetGuid == 0) {
                if (const uint64_t found = findNearestEnemy(); found != 0) {
                    setTarget(found);
                    return found;
                }
            }
            owner_.errorGlobal("ERR_INVALID_ATTACK_TARGET");
            return 0;
        }
    }

    return current;
}

bool CombatHandler::isEnemyTarget(uint64_t guid, const Unit& unit) const {
    return isTabTargetable(unit.isHostile(),
                           owner_.isFriendlyFaction(unit.getFactionTemplate()),
                           isAggressiveTowardPlayer(guid),
                           owner_.getCreatureType(unit.getEntry()),
                           unit.getUnitFlags());
}

void CombatHandler::targetFriend(bool reverse) {
    // Dead friends stay targetable - you need to select them to resurrect them.
    // Native /targetfriend picked Stormwind City Guards, so creatures count too.
    cycleTarget(reverse, [this](uint64_t, Entity& e) {
        const auto t = e.getType();
        return (t == ObjectType::PLAYER || t == ObjectType::UNIT) &&
            owner_.unitReactionToPlayer(static_cast<const Unit&>(e)) >= 5;
    });
}

void CombatHandler::targetNearestEnemyPlayer(bool reverse) {
    cycleTarget(reverse, [this](uint64_t guid, Entity& e) {
        if (e.getType() != ObjectType::PLAYER) return false;
        auto* unit = dynamic_cast<Unit*>(&e);
        if (!unit || unit->getHealth() == 0) return false;
        return unit->isHostile() || isAggressiveTowardPlayer(guid);
    });
}

void CombatHandler::targetNearestFriendPlayer(bool reverse) {
    cycleTarget(reverse, [](uint64_t, Entity& e) {
        if (e.getType() != ObjectType::PLAYER) return false;
        auto* unit = dynamic_cast<Unit*>(&e);
        return unit && !unit->isHostile();
    });
}

void CombatHandler::targetNearestPartyMember(bool reverse) {
    cycleTarget(reverse, [this](uint64_t guid, Entity&) {
        return isGroupMemberGuid(guid);
    });
}

void CombatHandler::targetNearestRaidMember(bool reverse) {
    // The same roster: a raid is what the group list calls itself when it has
    // grown past five, and the members arrive in the same list either way.
    cycleTarget(reverse, [this](uint64_t guid, Entity&) {
        return isGroupMemberGuid(guid);
    });
}

void CombatHandler::targetLastEnemy() {
    if (lastEnemyTargetGuid_ == 0) {
        return;
    }
    setTarget(lastEnemyTargetGuid_);
}

void CombatHandler::targetLastFriend() {
    if (lastFriendTargetGuid_ == 0) {
        return;
    }
    setTarget(lastFriendTargetGuid_);
}

void CombatHandler::tabTarget() {
    // The Tab binding is TARGETNEARESTENEMY, which is TargetNearestEnemy().
    targetEnemy(false);
}

void CombatHandler::assistTarget() {
    if (owner_.getState() != WorldState::IN_WORLD) {
        LOG_WARNING("Cannot assist: not in world");
        return;
    }

    if (owner_.getTargetGuid() == 0) {
        owner_.errorGlobal("ERR_GENERIC_NO_TARGET");
        return;
    }

    auto target = getTarget();
    if (!target) {
        owner_.raiseUiError("Invalid target.");
        return;
    }

    // Get target name
    std::string targetName = "Target";
    if (target->getType() == ObjectType::PLAYER) {
        auto player = std::static_pointer_cast<Player>(target);
        if (!player->getName().empty()) {
            targetName = player->getName();
        }
    } else if (target->getType() == ObjectType::UNIT) {
        auto unit = std::static_pointer_cast<Unit>(target);
        targetName = unit->getName();
    }

    // Who they have selected, read the one way it is read. See targetGuidOfUnit.
    const uint64_t assistTargetGuid = unitTargetGuid(target);

    if (assistTargetGuid == 0) {
        LOG_INFO("Assist: ", targetName, " has no target");
        return;
    }

    // Set our target to their target
    setTarget(assistTargetGuid);
    LOG_INFO("Assisting ", targetName, ", now targeting GUID: 0x", std::hex, assistTargetGuid, std::dec);
}

// ============================================================
// PvP
// ============================================================

void CombatHandler::togglePvp() {
    if (!owner_.isInWorld()) {
        LOG_WARNING("Cannot toggle PvP: not in world or not connected");
        return;
    }

    auto packet = TogglePvpPacket::build();
    owner_.getSocket()->send(packet);
    LOG_INFO("Requested PvP preference toggle");
}

// ============================================================
// Death / Resurrection
// ============================================================

void CombatHandler::releaseSpirit() {
    if (owner_.getSocket() && owner_.getState() == WorldState::IN_WORLD) {
        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (owner_.repopPendingRef() && now - static_cast<int64_t>(owner_.lastRepopRequestMsRef()) < 1000) {
            return;
        }
        auto packet = RepopRequestPacket::build();
        owner_.getSocket()->send(packet);
        owner_.selfResAvailableRef() = false;
        owner_.selfResSpellIdRef() = 0;
        owner_.repopPendingRef() = true;
        owner_.lastRepopRequestMsRef() = static_cast<uint64_t>(now);
        LOG_INFO("Sent CMSG_REPOP_REQUEST (Release Spirit)");
        network::Packet cq(wireOpcode(Opcode::MSG_CORPSE_QUERY));
        owner_.getSocket()->send(cq);
    }
}

bool CombatHandler::canReclaimCorpse() const {
    if (!owner_.releasedSpiritRef() || owner_.corpseGuidRef() == 0 ||
        !owner_.corpsePositionValidRef()) return false;
    if (owner_.currentMapIdRef() != owner_.corpseMapIdRef()) return false;
    float dx = owner_.movementInfoRef().x - owner_.corpseYRef();
    float dy = owner_.movementInfoRef().y - owner_.corpseXRef();
    float dz = owner_.movementInfoRef().z - owner_.corpseZRef();
    return (dx*dx + dy*dy + dz*dz) <= (40.0f * 40.0f);
}

float CombatHandler::getCorpseReclaimDelaySec() const {
    if (owner_.corpseReclaimAvailableMsRef() == 0) return 0.0f;
    auto nowMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    if (nowMs >= owner_.corpseReclaimAvailableMsRef()) return 0.0f;
    return static_cast<float>(owner_.corpseReclaimAvailableMsRef() - nowMs) / 1000.0f;
}

void CombatHandler::reclaimCorpse() {
    if (!canReclaimCorpse() || !owner_.getSocket()) return;
    if (owner_.corpseGuidRef() == 0) {
        LOG_WARNING("reclaimCorpse: corpse GUID not yet known (corpse object not received); cannot reclaim");
        return;
    }
    auto packet = ReclaimCorpsePacket::build(owner_.corpseGuidRef());
    owner_.getSocket()->send(packet);
    LOG_INFO("Sent CMSG_RECLAIM_CORPSE for corpse guid=0x", std::hex, owner_.corpseGuidRef(), std::dec);
}

void CombatHandler::useSelfRes() {
    if (!owner_.canSelfRes() || !owner_.getSocket()) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_SELF_RES));
    owner_.getSocket()->send(pkt);
    owner_.selfResAvailableRef() = false;
    owner_.selfResSpellIdRef() = 0;
    LOG_INFO("Sent CMSG_SELF_RES (Soulstone / Reincarnation / Twisting Nether)");
}

void CombatHandler::activateSpiritHealer(uint64_t npcGuid) {
    if (!owner_.isInWorld()) return;
    owner_.pendingSpiritHealerGuidRef() = npcGuid;
    auto packet = SpiritHealerActivatePacket::build(npcGuid);
    owner_.getSocket()->send(packet);
    owner_.resurrectPendingRef() = true;
    LOG_INFO("Sent CMSG_SPIRIT_HEALER_ACTIVATE for 0x", std::hex, npcGuid, std::dec);
}

void CombatHandler::acceptResurrect() {
    // Says which of the three refused it. Accepting a resurrect and nothing
    // happening looks the same from the chair whether the click never arrived,
    // the offer had already been cleared, or the packet went out and the server
    // ignored it - and only the last of those is not this function's fault.
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket() ||
        !owner_.resurrectRequestPendingRef()) {
        LOG_WARNING("acceptResurrect refused: inWorld=",
                    owner_.getState() == WorldState::IN_WORLD,
                    " socket=", owner_.getSocket() != nullptr,
                    " offerPending=", owner_.resurrectRequestPendingRef());
        return;
    }
    if (owner_.resurrectIsSpiritHealerRef()) {
        auto activate = SpiritHealerActivatePacket::build(owner_.resurrectCasterGuidRef());
        owner_.getSocket()->send(activate);
        LOG_INFO("Sent CMSG_SPIRIT_HEALER_ACTIVATE for 0x",
                 std::hex, owner_.resurrectCasterGuidRef(), std::dec);
    } else {
        auto resp = ResurrectResponsePacket::build(owner_.resurrectCasterGuidRef(), true);
        owner_.getSocket()->send(resp);
        LOG_INFO("Sent CMSG_RESURRECT_RESPONSE (accept) for 0x",
                 std::hex, owner_.resurrectCasterGuidRef(), std::dec);
    }
    owner_.resurrectRequestPendingRef() = false;
    owner_.resurrectPendingRef() = true;
}

void CombatHandler::declineResurrect() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket() || !owner_.resurrectRequestPendingRef()) return;
    auto resp = ResurrectResponsePacket::build(owner_.resurrectCasterGuidRef(), false);
    owner_.getSocket()->send(resp);
    LOG_INFO("Sent CMSG_RESURRECT_RESPONSE (decline) for 0x",
             std::hex, owner_.resurrectCasterGuidRef(), std::dec);
    owner_.resurrectRequestPendingRef() = false;
}

// ============================================================
// XP
// ============================================================

uint32_t CombatHandler::killXp(uint32_t playerLevel, uint32_t victimLevel) {
    if (playerLevel == 0 || victimLevel == 0) return 0;

    int32_t grayLevel;
    if (playerLevel <= 5)        grayLevel = 0;
    else if (playerLevel <= 39)  grayLevel = static_cast<int32_t>(playerLevel) - 5 - static_cast<int32_t>(playerLevel) / 10;
    else if (playerLevel <= 59)  grayLevel = static_cast<int32_t>(playerLevel) - 1 - static_cast<int32_t>(playerLevel) / 5;
    else                         grayLevel = static_cast<int32_t>(playerLevel) - 9;

    if (static_cast<int32_t>(victimLevel) <= grayLevel) return 0;

    uint32_t baseXp = 45 + 5 * victimLevel;

    int32_t diff = static_cast<int32_t>(victimLevel) - static_cast<int32_t>(playerLevel);
    float multiplier = 1.0f + diff * 0.05f;
    if (multiplier < 0.1f) multiplier = 0.1f;
    if (multiplier > 2.0f) multiplier = 2.0f;

    return static_cast<uint32_t>(baseXp * multiplier);
}

void CombatHandler::handleXpGain(network::Packet& packet) {
    XpGainData data;
    if (!XpGainParser::parse(packet, data)) return;

    addCombatText(CombatTextEntry::XP_GAIN, static_cast<int32_t>(data.totalXp), 0, true);

    std::string msg;
    if (data.victimGuid != 0 && data.type == 0) {
        std::string victimName = owner_.lookupName(data.victimGuid);
        if (!victimName.empty())
            msg = victimName + " dies, you gain " + std::to_string(data.totalXp) + " experience.";
        else
            msg = "You gain " + std::to_string(data.totalXp) + " experience.";
    } else {
        msg = "You gain " + std::to_string(data.totalXp) + " experience.";
    }
    if (data.restedBonus > 0) {
        msg += " (+" + std::to_string(data.restedBonus) + " exp Rested bonus)";
    } else if (data.rafBonus && data.totalXp > data.baseXp) {
        msg += " (+" + std::to_string(data.totalXp - data.baseXp) + " exp Refer-A-Friend bonus)";
    }
    if (data.groupBonus > 0) {
        msg += " (+" + std::to_string(data.groupBonus) + " group bonus)";
    }
    owner_.addLocalChatLine(ChatType::COMBAT_XP_GAIN, msg);
}

namespace {

constexpr float kCombatAudioChestHeightOffset = 2.0f;
constexpr float kMaxRemoteCombatAudioDistSq = 100.0f * 100.0f;

struct UnitCombatPositionInfo {
    bool known = false;
    glm::vec3 canonicalPos{0.0f};
    glm::vec3 renderPos{0.0f};
    std::shared_ptr<Entity> entity;
    Unit* unit = nullptr;
    bool isLocal = false;
    bool isPlayer = false;
    uint32_t displayId = 0;
};

UnitCombatPositionInfo resolveUnitCombatPosition(GameHandler& gh, uint64_t guid) {
    UnitCombatPositionInfo out;
    if (guid == 0u) return out;
    out.isLocal = (guid == gh.getPlayerGuid());
    out.entity = gh.getEntityManager().getEntity(guid);
    if (out.entity && out.entity->isUnit()) {
        out.unit = static_cast<Unit*>(out.entity.get());
        out.isPlayer = (out.entity->getType() == ObjectType::PLAYER) || out.isLocal;
        out.displayId = out.unit->getDisplayId();
        out.canonicalPos = glm::vec3(
            out.entity->getLatestX(), out.entity->getLatestY(), out.entity->getLatestZ());
        out.known = true;
    } else if (out.isLocal) {
        const auto& mi = gh.getMovementInfo();
        out.canonicalPos = glm::vec3(mi.x, mi.y, mi.z);
        out.isPlayer = true;
        out.known = true;
    }
    if (out.known) {
        out.renderPos = core::coords::canonicalToRender(out.canonicalPos) +
                        glm::vec3(0.0f, 0.0f, kCombatAudioChestHeightOffset);
    }
    return out;
}

bool withinCombatAudioRange(GameHandler& gh, const UnitCombatPositionInfo& a,
                            const UnitCombatPositionInfo& b) {
    if (a.isLocal || b.isLocal) return true;
    const auto& mi = gh.getMovementInfo();
    const glm::vec3 listenerCanonical(mi.x, mi.y, mi.z);
    if (a.known) {
        const glm::vec3 da = a.canonicalPos - listenerCanonical;
        if (glm::dot(da, da) <= kMaxRemoteCombatAudioDistSq) return true;
    }
    if (b.known) {
        const glm::vec3 db = b.canonicalPos - listenerCanonical;
        if (glm::dot(db, db) <= kMaxRemoteCombatAudioDistSq) return true;
    }
    return false;
}

audio::CombatSoundManager::ResolvedWeaponAudio resolveUnitWeaponAudio(
        GameHandler& gh, const audio::CombatSoundManager& csm,
        const UnitCombatPositionInfo& uinfo, bool offHand, bool ranged) {
    using RWA = audio::CombatSoundManager::ResolvedWeaponAudio;
    if (uinfo.isLocal) {
        const EquipSlot slot = ranged ? EquipSlot::RANGED
                             : (offHand ? EquipSlot::OFF_HAND : EquipSlot::MAIN_HAND);
        const auto& es = gh.getInventory().getEquipSlot(slot);
        if (es.empty()) return RWA{};
        if (const auto* q = gh.getItemInfo(es.item.itemId); q && q->valid) {
            if (q->itemClass == 2u) {
                return RWA{q->subClass, csm.isMetalMaterial(q->material), q->material};
            }
            return RWA{};
        }
        if (es.item.itemClass == 2u) {
            auto guessed = csm.resolveWeaponFromDisplayInfo(es.item.displayInfoId, es.item.inventoryType);
            guessed.subClass = es.item.subClass;
            return guessed;
        }
        if (es.item.itemClass == 0u) {
            if (es.item.displayInfoId != 0u || es.item.inventoryType != 0u) {
                return csm.resolveWeaponFromDisplayInfo(es.item.displayInfoId, es.item.inventoryType);
            }
            return csm.resolveWeaponFromItemId(es.item.itemId);
        }
        return RWA{};
    }

    if (uinfo.isPlayer) {
        std::array<uint32_t, 19> dispIds{};
        std::array<uint8_t, 19> invTypes{};
        if (gh.getOtherPlayerEquipment(uinfo.entity ? uinfo.entity->getGuid() : 0u, dispIds, invTypes)) {
            const size_t idx = ranged ? 17u : (offHand ? 16u : 15u);
            if (dispIds[idx] != 0u) {
                return csm.resolveWeaponFromDisplayInfo(dispIds[idx], invTypes[idx]);
            }
        }
        return RWA{};
    }

    if (uinfo.unit) {
        const uint16_t slots = fieldIndex(UF::UNIT_VIRTUAL_ITEM_SLOT_ID);
        const uint16_t info = fieldIndex(UF::UNIT_VIRTUAL_ITEM_INFO);
        const uint16_t slotIdx = ranged ? 2u : (offHand ? 1u : 0u);
        if (slots != 0xFFFF && uinfo.unit->getField(static_cast<uint16_t>(slots + slotIdx)) != 0u) {
            if (info != 0xFFFF) {
                // TBC / Classic UNIT_VIRTUAL_ITEM_INFO:
                // byte 0 = classId, byte 1 = subClassId, byte 2 = soundOverrideSubClassId, byte 3 = materialId
                const uint32_t info0 = uinfo.unit->getField(static_cast<uint16_t>(info + slotIdx * 2u));
                const uint8_t classId = static_cast<uint8_t>(info0 & 0xFFu);
                const uint8_t subClassId = static_cast<uint8_t>((info0 >> 8u) & 0xFFu);
                const uint8_t soundOverride = static_cast<uint8_t>((info0 >> 16u) & 0xFFu);
                const int32_t matId = static_cast<int32_t>((info0 >> 24u) & 0xFFu);
                if (classId == 2u) {
                    const uint32_t effectiveSub =
                        (soundOverride != 0xFFu && soundOverride <= 20u) ? soundOverride : subClassId;
                    return RWA{effectiveSub, csm.isMetalMaterial(matId), matId};
                }
            } else {
                const uint32_t itemEntry = uinfo.unit->getField(static_cast<uint16_t>(slots + slotIdx));
                if (const auto* q = gh.getItemInfo(itemEntry); q && q->valid && q->itemClass == 2u) {
                    return RWA{q->subClass, csm.isMetalMaterial(q->material), q->material};
                }
            }
        }
    }
    return RWA{};
}

void populateDefenderSurfaceAudio(
        GameHandler& gh, const audio::CombatSoundManager& csm,
        const UnitCombatPositionInfo& vinfo,
        audio::CombatSoundManager::QueuedCombatEvent& ev) {
    using IT = audio::CombatSoundManager::ImpactType;
    ev.victimImpactType = IT::FLESH;
    ev.defenderParryWeaponIsMetal = true;
    ev.defenderShieldIsMetal = true;

    if (vinfo.isLocal) {
        const auto& chest = gh.getInventory().getEquipSlot(EquipSlot::CHEST);
        if (!chest.empty()) {
            if (const auto* q = gh.getItemInfo(chest.item.itemId); q && q->valid) {
                ev.victimImpactType = csm.resolveArmorMaterialImpactType(q->material, q->subClass);
            } else {
                ev.victimImpactType = csm.resolveArmorMaterialImpactType(0, chest.item.subClass);
            }
        }
        const auto& mh = gh.getInventory().getEquipSlot(EquipSlot::MAIN_HAND);
        if (!mh.empty()) {
            if (const auto* q = gh.getItemInfo(mh.item.itemId); q && q->valid) {
                ev.defenderParryWeaponIsMetal = csm.isMetalMaterial(q->material);
            }
        }
        const auto& oh = gh.getInventory().getEquipSlot(EquipSlot::OFF_HAND);
        if (!oh.empty()) {
            if (const auto* q = gh.getItemInfo(oh.item.itemId); q && q->valid) {
                if (q->itemClass == 4u) {
                    ev.defenderShieldIsMetal = csm.isMetalMaterial(q->material);
                }
            }
        }
        return;
    }

    if (vinfo.isPlayer) {
        std::array<uint32_t, 19> dispIds{};
        std::array<uint8_t, 19> invTypes{};
        if (gh.getOtherPlayerEquipment(vinfo.entity ? vinfo.entity->getGuid() : 0u, dispIds, invTypes)) {
            ev.victimImpactType = csm.resolveChestDisplayImpactType(dispIds[4]);
            if (dispIds[15] != 0u) {
                ev.defenderParryWeaponIsMetal =
                    csm.resolveWeaponFromDisplayInfo(dispIds[15], invTypes[15]).isMetal;
            }
            if (dispIds[16] != 0u) {
                ev.defenderShieldIsMetal = csm.resolveShieldDisplayIsMetal(dispIds[16]);
            }
        }
        return;
    }

    if (vinfo.unit) {
        ev.victimImpactType = csm.getCreatureImpactTypeForDisplay(vinfo.displayId);
        const uint16_t slots = fieldIndex(UF::UNIT_VIRTUAL_ITEM_SLOT_ID);
        const uint16_t info = fieldIndex(UF::UNIT_VIRTUAL_ITEM_INFO);
        if (slots != 0xFFFF && info != 0xFFFF) {
            if (vinfo.unit->getField(slots) != 0u) {
                const uint32_t info0 = vinfo.unit->getField(info);
                if ((info0 & 0xFFu) == 2u) {
                    ev.defenderParryWeaponIsMetal =
                        csm.isMetalMaterial(static_cast<int32_t>((info0 >> 24u) & 0xFFu));
                }
            }
            if (vinfo.unit->getField(static_cast<uint16_t>(slots + 1u)) != 0u) {
                const uint32_t info0 = vinfo.unit->getField(static_cast<uint16_t>(info + 2u));
                if ((info0 & 0xFFu) == 4u) {
                    ev.defenderShieldIsMetal =
                        csm.isMetalMaterial(static_cast<int32_t>((info0 >> 24u) & 0xFFu));
                }
            }
        }
    }
}

} // namespace

void CombatHandler::bindCombatAudioCallbacks() {
    auto* ac = owner_.services().audioCoordinator;
    auto* csm = ac ? ac->getCombatSoundManager() : nullptr;
    if (!csm) return;

    if (!csm->hasAttackerExertionCallback()) {
        csm->setAttackerExertionCallback(
            [this](uint64_t attackerGuid, uint32_t attackerDisplayId,
                   const glm::vec3& renderPos, bool attackerIsPlayer,
                   bool attackerIsLocal, bool /*isCrit*/) {
                auto* coord = owner_.services().audioCoordinator;
                if (!coord || coord->getSfxVolumeScale() <= 0.0f) return;
                if (attackerIsLocal) {
                    if (auto* asm_ = coord->getActivitySoundManager()) {
                        asm_->playAttackGrunt();
                    }
                } else if (!attackerIsPlayer && attackerDisplayId != 0u) {
                    if (auto* nvm = coord->getNpcVoiceManager()) {
                        nvm->playCombatAttack(attackerGuid, attackerDisplayId, renderPos);
                    }
                }
            });
    }

    if (!csm->hasVictimWoundCallback()) {
        csm->setVictimWoundCallback(
            [this](uint64_t victimGuid, uint32_t victimDisplayId,
                   const glm::vec3& renderPos, bool victimIsPlayer,
                   bool victimIsLocal, audio::CombatSoundManager::CombatOutcome outcome) {
                auto* coord = owner_.services().audioCoordinator;
                auto* soundMgr = coord ? coord->getCombatSoundManager() : nullptr;
                if (!coord || !soundMgr || coord->getSfxVolumeScale() <= 0.0f) return;

                if (outcome == audio::CombatSoundManager::CombatOutcome::CRUSHING &&
                    victimDisplayId != 0u) {
                    if (soundMgr->playCreatureCrushingWound(victimDisplayId, renderPos, victimIsLocal)) {
                        return;
                    }
                }

                const bool isCrit = (outcome == audio::CombatSoundManager::CombatOutcome::CRIT) ||
                                    (outcome == audio::CombatSoundManager::CombatOutcome::CRUSHING);
                if (victimIsLocal) {
                    if (auto* asm_ = coord->getActivitySoundManager()) {
                        asm_->playWound(isCrit);
                    }
                } else if (!victimIsPlayer && victimDisplayId != 0u) {
                    if (auto* nvm = coord->getNpcVoiceManager()) {
                        nvm->playWound(victimGuid, victimDisplayId, isCrit, renderPos);
                    }
                } else if (victimIsPlayer && victimDisplayId != 0u && owner_.services().assetManager) {
                    const uint32_t sid = soundMgr->getCreatureWoundSoundId(victimDisplayId, isCrit);
                    if (sid != 0u) {
                        audio::SoundEntryTable::instance().play3D(
                            owner_.services().assetManager, sid, renderPos, coord->getSfxVolumeScale());
                    }
                }
            });
    }
}

void CombatHandler::enqueueMeleeCombatAudio(
        const AttackerStateUpdateData& data, bool isOffHandSwing,
        bool isRangedAutoShot, bool lethalHit) {
    auto* ac = owner_.services().audioCoordinator;
    auto* csm = ac ? ac->getCombatSoundManager() : nullptr;
    if (!csm || ac->getSfxVolumeScale() <= 0.0f) return;
    bindCombatAudioCallbacks();

    const UnitCombatPositionInfo atk = resolveUnitCombatPosition(owner_, data.attackerGuid);
    const UnitCombatPositionInfo vic = resolveUnitCombatPosition(owner_, data.targetGuid);
    if (!atk.known && !vic.known) return;
    if (!withinCombatAudioRange(owner_, atk, vic)) return;

    audio::CombatSoundManager::QueuedCombatEvent ev;
    ev.attackerGuid = data.attackerGuid;
    ev.victimGuid = data.targetGuid;
    ev.attackerRenderPos = atk.known ? atk.renderPos : vic.renderPos;
    ev.victimRenderPos = vic.known ? vic.renderPos : atk.renderPos;
    ev.attackerIsLocal = atk.isLocal;
    ev.victimIsLocal = vic.isLocal;
    ev.attackerIsPlayer = atk.isPlayer;
    ev.victimIsPlayer = vic.isPlayer;
    ev.attackerUnderwater = (atk.isLocal && owner_.isSwimming()) ||
                            csm->isUnitUnderwater(data.attackerGuid);
    ev.offHand = isOffHandSwing;
    ev.isRanged = isRangedAutoShot;
    ev.suppressVictimWound = lethalHit || data.isNoAction();
    ev.attackerDisplayId = atk.displayId;
    ev.victimDisplayId = vic.displayId;

    const auto weap = resolveUnitWeaponAudio(owner_, *csm, atk, isOffHandSwing, isRangedAutoShot);
    ev.attackerWeaponSubClass = weap.subClass;
    ev.attackerWeaponIsMetal = weap.isMetal;
    ev.attackerWeaponMaterial = weap.material;

    populateDefenderSurfaceAudio(owner_, *csm, vic, ev);
    ev.outcome = audio::CombatSoundManager::classifyMeleeOutcome(
        data.isMiss(), data.isCrit(), data.isCrushing(), data.isGlancing(),
        data.victimState, data.totalDamage, data.blocked);

    if (lethalHit && data.targetGuid != 0u) {
        csm->clearUnitEvents(data.targetGuid);
    }
    csm->queueCombatEvent(std::move(ev));
}

void CombatHandler::enqueuePhysicalSpellCombatAudio(
        const SpellDamageLogData& data, bool lethalHit) {
    auto* ac = owner_.services().audioCoordinator;
    auto* csm = ac ? ac->getCombatSoundManager() : nullptr;
    if (!csm || ac->getSfxVolumeScale() <= 0.0f) return;
    bindCombatAudioCallbacks();

    const UnitCombatPositionInfo atk = resolveUnitCombatPosition(owner_, data.attackerGuid);
    const UnitCombatPositionInfo vic = resolveUnitCombatPosition(owner_, data.targetGuid);
    if (!atk.known && !vic.known) return;
    if (!withinCombatAudioRange(owner_, atk, vic)) return;

    // Non-physical spells get their spell impact sound from SpellVisualKit; only
    // trigger victim wound vocal here when damage landed on a non-lethal hit.
    if (data.schoolMask != 1u) {
        if (!lethalHit && data.damage > 0u && vic.known) {
            if (vic.isLocal) {
                if (auto* asm_ = ac->getActivitySoundManager()) {
                    asm_->playWound(data.isCrit);
                }
            } else if (!vic.isPlayer && vic.displayId != 0u) {
                if (auto* nvm = ac->getNpcVoiceManager()) {
                    nvm->playWound(data.targetGuid, vic.displayId, data.isCrit, vic.renderPos);
                }
            } else if (vic.isPlayer && vic.displayId != 0u && owner_.services().assetManager) {
                const uint32_t sid = csm->getCreatureWoundSoundId(vic.displayId, data.isCrit);
                if (sid != 0u) {
                    audio::SoundEntryTable::instance().play3D(
                        owner_.services().assetManager, sid, vic.renderPos, ac->getSfxVolumeScale());
                }
            }
        }
        return;
    }

    if (data.damage == 0u && data.blocked == 0u) return;

    bool isRanged = (data.spellId == 75u || data.spellId == 3018u || data.spellId == 2764u);
    if (!isRanged && atk.unit) {
        const uint16_t b2 = fieldIndex(UF::UNIT_FIELD_BYTES_2);
        if (b2 != 0xFFFF && (atk.unit->getField(b2) & 0xFFu) == 2u) {
            isRanged = true;
        }
    }

    audio::CombatSoundManager::QueuedCombatEvent ev;
    ev.attackerGuid = data.attackerGuid;
    ev.victimGuid = data.targetGuid;
    ev.attackerRenderPos = atk.known ? atk.renderPos : vic.renderPos;
    ev.victimRenderPos = vic.known ? vic.renderPos : atk.renderPos;
    ev.attackerIsLocal = atk.isLocal;
    ev.victimIsLocal = vic.isLocal;
    ev.attackerIsPlayer = atk.isPlayer;
    ev.victimIsPlayer = vic.isPlayer;
    ev.attackerUnderwater = (atk.isLocal && owner_.isSwimming()) ||
                            csm->isUnitUnderwater(data.attackerGuid);
    ev.isRanged = isRanged;
    ev.suppressVictimWound = lethalHit;
    ev.attackerDisplayId = atk.displayId;
    ev.victimDisplayId = vic.displayId;

    const auto weap = resolveUnitWeaponAudio(owner_, *csm, atk, /*offHand=*/false, isRanged);
    ev.attackerWeaponSubClass = weap.subClass;
    ev.attackerWeaponIsMetal = weap.isMetal;
    ev.attackerWeaponMaterial = weap.material;

    populateDefenderSurfaceAudio(owner_, *csm, vic, ev);
    ev.outcome = (data.damage == 0u && data.blocked > 0u)
        ? audio::CombatSoundManager::CombatOutcome::BLOCK
        : (data.isCrit ? audio::CombatSoundManager::CombatOutcome::CRIT
                       : audio::CombatSoundManager::CombatOutcome::HIT);

    if (lethalHit && data.targetGuid != 0u) {
        csm->clearUnitEvents(data.targetGuid);
    }
    csm->queueCombatEvent(std::move(ev));
}

bool CombatHandler::onUnitSwingEvent(uint64_t unitGuid, bool offHand) {
    auto* ac = owner_.services().audioCoordinator;
    auto* csm = ac ? ac->getCombatSoundManager() : nullptr;
    if (!csm || ac->getSfxVolumeScale() <= 0.0f) return false;
    bindCombatAudioCallbacks();

    const UnitCombatPositionInfo uinfo = resolveUnitCombatPosition(owner_, unitGuid);
    if (!uinfo.known) return false;
    const auto weap = resolveUnitWeaponAudio(owner_, *csm, uinfo, offHand, /*ranged=*/false);
    const bool underwater = (uinfo.isLocal && owner_.isSwimming()) || csm->isUnitUnderwater(unitGuid);
    return csm->onUnitSwingEvent(unitGuid, uinfo.renderPos, uinfo.isLocal, offHand, underwater, weap.subClass);
}

bool CombatHandler::onUnitHitEvent(uint64_t attackerGuid) {
    auto* ac = owner_.services().audioCoordinator;
    auto* csm = ac ? ac->getCombatSoundManager() : nullptr;
    if (!csm || ac->getSfxVolumeScale() <= 0.0f) return false;
    bindCombatAudioCallbacks();

    const UnitCombatPositionInfo uinfo = resolveUnitCombatPosition(owner_, attackerGuid);
    return csm->onUnitHitEvent(attackerGuid, uinfo.renderPos, uinfo.isLocal);
}

bool CombatHandler::onUnitBowPullEvent(uint64_t unitGuid) {
    auto* ac = owner_.services().audioCoordinator;
    auto* csm = ac ? ac->getCombatSoundManager() : nullptr;
    if (!csm || ac->getSfxVolumeScale() <= 0.0f) return false;

    const UnitCombatPositionInfo uinfo = resolveUnitCombatPosition(owner_, unitGuid);
    if (!uinfo.known) return false;
    const auto weap = resolveUnitWeaponAudio(owner_, *csm, uinfo, /*offHand=*/false, /*ranged=*/true);
    return csm->onUnitBowPullEvent(unitGuid, uinfo.renderPos, uinfo.isLocal, weap.subClass);
}

bool CombatHandler::onUnitBowReleaseEvent(uint64_t unitGuid) {
    auto* ac = owner_.services().audioCoordinator;
    auto* csm = ac ? ac->getCombatSoundManager() : nullptr;
    if (!csm || ac->getSfxVolumeScale() <= 0.0f) return false;

    const UnitCombatPositionInfo uinfo = resolveUnitCombatPosition(owner_, unitGuid);
    if (!uinfo.known) return false;
    const auto weap = resolveUnitWeaponAudio(owner_, *csm, uinfo, /*offHand=*/false, /*ranged=*/true);
    return csm->onUnitBowReleaseEvent(unitGuid, uinfo.renderPos, uinfo.isLocal, weap.subClass);
}

bool CombatHandler::onUnitSheatheEvent(uint64_t unitGuid, bool offHandSlot, bool unsheathe) {
    auto* ac = owner_.services().audioCoordinator;
    auto* csm = ac ? ac->getCombatSoundManager() : nullptr;
    if (!csm || ac->getSfxVolumeScale() <= 0.0f) return false;

    const UnitCombatPositionInfo uinfo = resolveUnitCombatPosition(owner_, unitGuid);
    if (!uinfo.known) return false;

    if (uinfo.isLocal) {
        const EquipSlot slot = offHandSlot ? EquipSlot::OFF_HAND : EquipSlot::MAIN_HAND;
        const auto& es = owner_.getInventory().getEquipSlot(slot);
        if (es.empty()) return false;
        if (const auto* q = owner_.getItemInfo(es.item.itemId); q && q->valid) {
            const bool isShield = (q->inventoryType == InvType::SHIELD) ||
                                  (q->itemClass == 4u && (q->subClass == 5u || q->subClass == 6u));
            return csm->onUnitSheatheEvent(
                unitGuid, uinfo.renderPos, /*isLocalPlayer=*/true,
                q->itemClass, q->subClass, q->material, isShield, unsheathe);
        }
        const bool isShield = (es.item.inventoryType == InvType::SHIELD);
        const int32_t fallbackMat = (es.item.inventoryType == InvType::HOLDABLE) ? 7 : 1;
        return csm->onUnitSheatheEvent(
            unitGuid, uinfo.renderPos, /*isLocalPlayer=*/true,
            es.item.itemClass, es.item.subClass, fallbackMat, isShield, unsheathe);
    }

    if (uinfo.unit && !uinfo.isPlayer) {
        const uint16_t slots = fieldIndex(UF::UNIT_VIRTUAL_ITEM_SLOT_ID);
        const uint16_t info = fieldIndex(UF::UNIT_VIRTUAL_ITEM_INFO);
        const uint16_t slotIdx = offHandSlot ? 1u : 0u;
        if (slots != 0xFFFF && info != 0xFFFF &&
            uinfo.unit->getField(static_cast<uint16_t>(slots + slotIdx)) != 0u) {
            const uint32_t info0 = uinfo.unit->getField(static_cast<uint16_t>(info + slotIdx * 2u));
            const uint32_t info1 = uinfo.unit->getField(static_cast<uint16_t>(info + slotIdx * 2u + 1u));
            const uint8_t classId = static_cast<uint8_t>(info0 & 0xFFu);
            const uint8_t subClassId = static_cast<uint8_t>((info0 >> 8u) & 0xFFu);
            const int32_t matId = static_cast<int32_t>((info0 >> 24u) & 0xFFu);
            const uint8_t invType = static_cast<uint8_t>(info1 & 0xFFu);
            const bool isShield = (invType == InvType::SHIELD) || (classId == 4u && subClassId == 6u);
            return csm->onUnitSheatheEvent(
                unitGuid, uinfo.renderPos, /*isLocalPlayer=*/false,
                classId, subClassId, matId, isShield, unsheathe);
        }
    }

    const auto weap = resolveUnitWeaponAudio(owner_, *csm, uinfo, offHandSlot, /*ranged=*/false);
    return csm->onUnitSheatheEvent(
        unitGuid, uinfo.renderPos, uinfo.isLocal, 2u, weap.subClass,
        weap.material > 0 ? weap.material : 1, /*isShield=*/false, unsheathe);
}

} // namespace game
} // namespace wowee
