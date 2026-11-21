#pragma once

#include "game/world_packets.hpp"
#include "game/opcode_table.hpp"
#include "game/spell_defines.hpp"
#include "network/packet.hpp"
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wowee {
namespace game {

class GameHandler;
class Entity;

class CombatHandler {
public:
    using PacketHandler = std::function<void(network::Packet&)>;
    using DispatchTable = std::unordered_map<LogicalOpcode, PacketHandler>;

    explicit CombatHandler(GameHandler& owner);

    void registerOpcodes(DispatchTable& table);

    // --- Public API (delegated from GameHandler) ---
    void startAutoAttack(uint64_t targetGuid);
    void stopAutoAttack();
    [[nodiscard]] bool isAutoAttacking() const { return autoAttacking_; }
    [[nodiscard]] bool hasAutoAttackIntent() const { return autoAttackRequested_; }
    [[nodiscard]] bool isInCombat() const { return autoAttacking_ || !hostileAttackers_.empty(); }
    [[nodiscard]] bool isInCombatWith(uint64_t guid) const {
        return guid != 0 &&
               ((autoAttacking_ && autoAttackTarget_ == guid) ||
                (hostileAttackers_.count(guid) > 0));
    }
    [[nodiscard]] uint64_t getAutoAttackTargetGuid() const { return autoAttackTarget_; }
    [[nodiscard]] bool isAggressiveTowardPlayer(uint64_t guid) const { return hostileAttackers_.count(guid) > 0; }
    [[nodiscard]] uint64_t getLastMeleeSwingMs() const { return lastMeleeSwingMs_; }

    // Floating combat text
    [[nodiscard]] const std::vector<CombatTextEntry>& getCombatText() const { return combatText_; }
    void updateCombatText(float deltaTime);

    // Combat log (persistent rolling history)
    [[nodiscard]] const std::deque<CombatLogEntry>& getCombatLog() const { return combatLog_; }
    /// How many entries the log has ever been given. It holds the last 500, so
    /// this, not its size, says how many arrived since a reader last looked.
    [[nodiscard]] uint64_t getCombatLogAppended() const { return combatLogAppended_; }

    // Threat
    struct ThreatEntry {
        uint64_t victimGuid = 0;
        uint32_t threat     = 0;
    };
    [[nodiscard]] const std::vector<ThreatEntry>* getThreatList(uint64_t unitGuid) const {
        auto it = threatLists_.find(unitGuid);
        return (it != threatLists_.end()) ? &it->second : nullptr;
    }

    // Hostile attacker tracking
    [[nodiscard]] bool isHostileAttacker(uint64_t guid) const { return hostileAttackers_.count(guid) > 0; }
    void clearHostileAttackers() { hostileAttackers_.clear(); }

    // Forced faction reactions
    [[nodiscard]] const std::unordered_map<uint32_t, uint8_t>& getForcedReactions() const { return forcedReactions_; }

    // Auto-attack timing state (read by GameHandler::update for retry/resend logic)
    bool& autoAttackOutOfRangeRef() { return autoAttackOutOfRange_; }
    float& autoAttackRangeWarnCooldownRef() { return autoAttackRangeWarnCooldown_; }

    // Combat text creation (used by other handlers, e.g. spell handler for periodic damage)
    /// `schoolMask` is the damage school (UNIT_COMBAT's fifth argument);
    /// `reduction` marks the absorbed/resisted/blocked part of the hit added
    /// just before; `unitCombat` false when the caller announces the hit
    /// itself (fireUnitCombat).
    void addCombatText(CombatTextEntry::Type type, int32_t amount, uint32_t spellId,
                       bool isPlayerSource, uint8_t powerType = 0,
                       uint64_t srcGuid = 0, uint64_t dstGuid = 0,
                       uint32_t schoolMask = 0, bool reduction = false, bool unitCombat = true);

    /// UNIT_COMBAT for one hit on `victim`, when the interface has a unit
    /// token for it - whoever struck it.
    void fireUnitCombat(uint64_t victim, const char* action, const char* flags, int32_t amount,
                        uint32_t schoolMask);

    // Spellsteal dedup (used by aura update handler)
    bool shouldLogSpellstealAura(uint64_t casterGuid, uint64_t victimGuid, uint32_t spellId);

    // Called from GameHandler::update() each frame
    void updateAutoAttack(float deltaTime);

    // --- Targeting ---
    /// False for creature corpses that are neither lootable nor skinnable -
    /// those cannot be selected. Living units, players and non-units are always selectable.
    [[nodiscard]] bool isSelectableUnit(uint64_t guid) const;
    void setTarget(uint64_t guid);
    void clearTarget();
    [[nodiscard]] std::shared_ptr<Entity> getTarget() const;
    void setFocus(uint64_t guid);
    void clearFocus();
    [[nodiscard]] std::shared_ptr<Entity> getFocus() const;
    void setMouseoverGuid(uint64_t guid);
    void targetLastTarget();
    void targetEnemy(bool reverse);
    void targetFriend(bool reverse);
    /// Nearest live attackable enemy using the Tab-target cone/range/screen rule,
    /// without changing target or resetting the Tab cycle timer. Returns 0 if none.
    [[nodiscard]] uint64_t findNearestEnemy() const;
    /// Resolve an attackable target for Attack / /startattack / auto-target skills:
    /// - Live attackable current target: returned as-is.
    /// - Dead target: refuses with ERR_INVALID_ATTACK_TARGET without switching.
    /// - No target or friendly/self target (when explicitTargetGuid == 0): tries
    ///   findNearestEnemy(), selects and returns it if found, else raises
    ///   ERR_NO_ATTACK_TARGET (no target) or ERR_INVALID_ATTACK_TARGET (friendly).
    [[nodiscard]] uint64_t acquireAttackTarget(uint64_t explicitTargetGuid = 0);

    /// The four narrower cycles WoW binds keys to, and which answered nothing.
    ///
    /// Same walk as the two above with a tighter test: only players, or only
    /// the people in your group. Bindings.xml gives each of them a key by
    /// default, so each was a key that did nothing at all.
    void targetNearestEnemyPlayer(bool reverse);
    void targetNearestFriendPlayer(bool reverse);
    void targetNearestPartyMember(bool reverse);
    void targetNearestRaidMember(bool reverse);

    /// Back to the last hostile, or the last friendly, target.
    ///
    /// TargetLastTarget remembers one target whatever it was; these remember one
    /// of each, which is what makes them useful - a healer's last friendly
    /// target survives half a fight's worth of tab-targeting.
    void targetLastEnemy();
    void targetLastFriend();
    void tabTarget();
    void assistTarget();

    // --- PvP ---
    void togglePvp();

    // --- Death / Resurrection ---
    void releaseSpirit();
    [[nodiscard]] bool canReclaimCorpse() const;
    [[nodiscard]] float getCorpseReclaimDelaySec() const;
    void reclaimCorpse();
    void useSelfRes();
    void activateSpiritHealer(uint64_t npcGuid);
    void acceptResurrect();
    void declineResurrect();

    // --- XP ---
    static uint32_t killXp(uint32_t playerLevel, uint32_t victimLevel);
    void handleXpGain(network::Packet& packet);

    // State management (for resets, entity cleanup)
    void resetAllCombatState();
    void removeHostileAttacker(uint64_t guid);
    void clearCombatText();
    void removeCombatTextForGuid(uint64_t guid);

private:
    /// Closes the loot window when it shows the current target, before the
    /// target changes (see setTarget).
    void releaseLootOfTarget();

    // Last target-cycling press (steady-clock ms); a pause longer than
    // kTabCycleResetMs restarts the cycle at the nearest unit.
    uint64_t lastTabTargetMs_ = 0;

    // One remembered target of each kind, for targetLastEnemy/Friend. Written
    // by setTarget as the old target is let go, so they are the target before
    // this one rather than this one.
    uint64_t lastEnemyTargetGuid_ = 0;
    uint64_t lastFriendTargetGuid_ = 0;

    /// The walk every cycle does: units `wanted` accepts that lie in the cone
    /// in front of the character, on screen and within range (tab_target.hpp),
    /// nearest first, then step past the current target. One predicate per
    /// cycle; the enemy cycle skips the dead and the friendly one does not.
    void cycleTarget(bool reverse, const char* noneMessage,
                     const std::function<bool(uint64_t, Entity&)>& wanted);

    /// Whether a guid is in the player's group.
    [[nodiscard]] bool isGroupMemberGuid(uint64_t guid) const;

    /// Whether Tab and TargetNearestEnemy may pick a living unit.
    [[nodiscard]] bool isEnemyTarget(uint64_t guid, const Unit& unit) const;

    // --- Packet handlers ---
    void handleAttackStart(network::Packet& packet);
    void handleAttackStop(network::Packet& packet);
    void handleAttackerStateUpdate(network::Packet& packet);
    void handleSpellDamageLog(network::Packet& packet);
    void handleSpellHealLog(network::Packet& packet);
    void handleSetForcedReactions(network::Packet& packet);
    void handleHealthUpdate(network::Packet& packet);
    void handlePowerUpdate(network::Packet& packet);
    void handleUpdateComboPoints(network::Packet& packet);
    void handlePvpCredit(network::Packet& packet);
    void handleProcResist(network::Packet& packet);
    void handleSpellDamageShield(network::Packet& packet);
    void handleSpellOrDamageImmune(network::Packet& packet);
    void handleResistLog(network::Packet& packet);
    void handlePetTameFailure(network::Packet& packet);
    void handlePetActionFeedback(network::Packet& packet);
    void handlePetCastFailed(network::Packet& packet);
    void handlePetBroken(network::Packet& packet);
    void handlePetLearnedSpell(network::Packet& packet);
    void handlePetUnlearnedSpell(network::Packet& packet);
    void handlePetMode(network::Packet& packet);
    void handleResurrectFailed(network::Packet& packet);

    void autoTargetAttacker(uint64_t attackerGuid);
    void announceLeaveCombat();

    GameHandler& owner_;

    // --- Combat state ---
    bool autoAttacking_ = false;
    bool autoAttackRequested_ = false;
    // PLAYER_ENTER_COMBAT has been fired and PLAYER_LEAVE_COMBAT not yet.
    // The original fires both from the server's SMSG_ATTACKSTART/STOP, not
    // from the local request; a stop for an attack that never started fires
    // nothing.
    bool enterCombatAnnounced_ = false;
    uint64_t autoAttackTarget_ = 0;
    bool autoAttackOutOfRange_ = false;
    float autoAttackRangeWarnCooldown_ = 0.0f;
    /// Seconds until "You are too far away!" is said again while the attack
    /// stays out of reach (kSwingRangeRepeatSeconds after the last one).
    float autoAttackRangeRepeat_ = 0.0f;
    std::unordered_set<uint64_t> hostileAttackers_;
    std::vector<CombatTextEntry> combatText_;
    /// Damage of the last hit announced on the player, for the partial
    /// absorb/resist/block line that follows it (-1 after anything else).
    int32_t incomingHitDamage_ = -1;
    static constexpr size_t MAX_COMBAT_LOG = 500;
    /// How often the original repeats "You are too far away!" while an
    /// attack stays out of melee reach.
    static constexpr float kSwingRangeRepeatSeconds = 4.0f;
    std::deque<CombatLogEntry> combatLog_;
    uint64_t combatLogAppended_ = 0;

    struct RecentSpellstealLogEntry {
        uint64_t casterGuid = 0;
        uint64_t victimGuid = 0;
        uint32_t spellId = 0;
        std::chrono::steady_clock::time_point timestamp{};
    };
    static constexpr size_t MAX_RECENT_SPELLSTEAL_LOGS = 32;
    std::deque<RecentSpellstealLogEntry> recentSpellstealLogs_;

    uint64_t lastMeleeSwingMs_ = 0;

    // unitGuid → sorted threat list (descending by threat value)
    std::unordered_map<uint64_t, std::vector<ThreatEntry>> threatLists_;
    /// Which mobs the player is currently top of the threat list for, so the
    /// aggro warning sounds on becoming that rather than on every update that
    /// says they still are. See the threat update handler.
    std::unordered_set<uint64_t> playerTopThreatOn_;
    /// Whether an aggro warning applies at all where the player is - the same
    /// question IsThreatWarningEnabled answers for the indicator, asked from
    /// C++ so the sound and the picture agree about when they are wanted.
    [[nodiscard]] bool threatWarningWanted() const;

    // Forced faction reactions
    std::unordered_map<uint32_t, uint8_t> forcedReactions_;
};

} // namespace game
} // namespace wowee
