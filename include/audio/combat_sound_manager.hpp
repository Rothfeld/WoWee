#pragma once

#include <glm/glm.hpp>
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wowee {
namespace pipeline {
class AssetManager;
}

namespace audio {

/// TBC 2.4.3 (Wow.exe build 8606) melee/ranged combat sound resolver and
/// keyframe-synchronized 3D/local combat outcome queue.
///
/// Loads:
///   - WeaponSwingSounds2.dbc (6 rows: WeaponSize 0..2 x Crit 0..1 -> SoundEntries 233..238)
///   - ItemSubClass.dbc (class == 2 WeaponSwingSize col 9 -> 0=Small, 1=Medium, 2=Large)
///   - WeaponImpactSounds.dbc (all 20 sound columns 3..22: normal + crit across all 10 surfaces)
///   - SheatheSoundLookups.dbc (33 rows) + Material.dbc (cols 1..4 including Tome 11597/11598)
///   - CreatureDisplayInfo/CreatureModelData/CreatureSoundData (col 25 CreatureImpactType + cols 1..5 vocals)
class CombatSoundManager {
public:
    CombatSoundManager() = default;
    ~CombatSoundManager() = default;

    bool initialize(pipeline::AssetManager* assets);
    void shutdown();

    void setVolumeScale(float scale);
    [[nodiscard]] float getVolumeScale() const { return volumeScale_; }

    // ItemSubClass.dbc col 9 WeaponSwingSize (0=Small, 1=Medium, 2=Large)
    enum class WeaponSize : uint8_t {
        SMALL = 0,   // Dagger (15), Fist/Unarmed (13) -> SoundEntries 233/234
        MEDIUM = 1,  // 1H Axe (0), 1H Mace (4), 1H Sword (7), Ranged -> SoundEntries 235/236
        LARGE = 2    // 2H Axe (1), 2H Mace (5), Polearm (6), 2H Sword (8), Staff (10), Spear (17), Fishing Pole (20) -> SoundEntries 237/238
    };

    // Exact 10-slot impact surface index matching Wow.exe 0xbe1b90 / 0x466ce0
    // (cols 3..12 normal, cols 13..22 critical in WeaponImpactSounds.dbc).
    enum class ImpactType : uint8_t {
        FLESH = 0,              // col 3 / crit col 13
        CHAIN = 1,              // col 4 / crit col 14
        PLATE = 2,              // col 5 / crit col 15
        SHIELD_METAL = 3,       // col 6 / crit col 16
        SHIELD_WOOD = 4,        // col 7 / crit col 17
        PARRY_METAL_METAL = 5,  // col 8 / crit col 18
        PARRY_METAL_WOOD = 6,   // col 9 / crit col 19
        WOOD = 7,               // col 10 / crit col 20
        STONE = 8,              // col 11 / crit col 21
        ETHEREAL = 9,           // col 12 / crit col 22

        // Backward-compatible aliases
        SHIELD = SHIELD_METAL,
        METAL_WEAPON = PARRY_METAL_METAL,
        WOOD_WEAPON = PARRY_METAL_WOOD
    };

    enum class CombatOutcome : uint8_t {
        HIT = 0,
        CRIT = 1,
        CRUSHING = 2,
        GLANCING = 3,
        MISS = 4,
        DODGE = 5,
        PARRY = 6,
        BLOCK = 7,
        EVADE_DEFLECT = 8,
        IMMUNE_OR_ABSORBED = 9
    };

    struct ResolvedWeaponAudio {
        uint32_t subClass = 13;  // 13 = Fist/Unarmed
        bool isMetal = false;
        int32_t material = 0;
    };

    struct QueuedCombatEvent {
        uint64_t attackerGuid = 0;
        uint64_t victimGuid = 0;
        glm::vec3 attackerRenderPos{0.0f};
        glm::vec3 victimRenderPos{0.0f};
        bool attackerIsLocal = false;
        bool victimIsLocal = false;
        bool attackerIsPlayer = false;
        bool victimIsPlayer = false;
        bool attackerUnderwater = false;
        bool offHand = false;
        bool isRanged = false;
        bool suppressVictimWound = false; // e.g. lethal hit

        uint32_t attackerDisplayId = 0;
        uint32_t victimDisplayId = 0;

        uint32_t attackerWeaponSubClass = 13;
        bool attackerWeaponIsMetal = false;
        int32_t attackerWeaponMaterial = 0;

        ImpactType victimImpactType = ImpactType::FLESH;
        bool defenderParryWeaponIsMetal = true;
        bool defenderShieldIsMetal = true;

        CombatOutcome outcome = CombatOutcome::HIT;

        // Internal lifecycle state managed by CombatSoundManager
        bool swingConsumed = false;
        bool hitConsumed = false;
        float ageSeconds = 0.0f;
        float swingFallbackDelay = 0.12f;
        float hitFallbackDelay = 0.28f;
    };

    using AttackerExertionCallback = std::function<void(
        uint64_t attackerGuid, uint32_t attackerDisplayId, const glm::vec3& renderPos,
        bool attackerIsPlayer, bool attackerIsLocal, bool isCrit)>;
    using VictimWoundCallback = std::function<void(
        uint64_t victimGuid, uint32_t victimDisplayId, const glm::vec3& renderPos,
        bool victimIsPlayer, bool victimIsLocal, CombatOutcome outcome)>;

    void setAttackerExertionCallback(AttackerExertionCallback cb) {
        attackerExertionCallback_ = std::move(cb);
    }
    void setVictimWoundCallback(VictimWoundCallback cb) {
        victimWoundCallback_ = std::move(cb);
    }
    [[nodiscard]] bool hasAttackerExertionCallback() const {
        return static_cast<bool>(attackerExertionCallback_);
    }
    [[nodiscard]] bool hasVictimWoundCallback() const {
        return static_cast<bool>(victimWoundCallback_);
    }

    // --- Pure TBC 2.4.3 DBC / binary table resolvers ---
    [[nodiscard]] WeaponSize getWeaponSizeForSubclass(uint32_t weaponSubClass) const;
    [[nodiscard]] static bool isTwoHandWeaponSubclass(uint32_t weaponSubClass);
    [[nodiscard]] uint32_t getWeaponSwingSoundId(WeaponSize size, bool isCrit) const;
    [[nodiscard]] uint32_t getWeaponImpactSoundId(uint32_t weaponSubClass, bool metalWeapon,
                                                  ImpactType impactType, bool isCrit) const;
    [[nodiscard]] uint32_t getWeaponMissSoundId(bool twoHanded) const;
    [[nodiscard]] uint32_t getDeflectSoundId() const;
    [[nodiscard]] uint32_t getBowPullSoundId(uint32_t rangedSubClass) const;
    [[nodiscard]] uint32_t getBowReleaseSoundId(uint32_t rangedSubClass) const;
    [[nodiscard]] uint32_t getSheatheSoundId(uint32_t itemClass, uint32_t itemSubClass,
                                             int32_t material, bool isShield, bool unsheathe) const;
    [[nodiscard]] uint32_t getMaterialSheatheSoundId(int32_t material, bool unsheathe) const;

    [[nodiscard]] bool isMetalMaterial(int32_t materialId) const;
    [[nodiscard]] ImpactType resolveArmorMaterialImpactType(int32_t chestMaterialId,
                                                            uint32_t armorSubClass = 0) const;
    [[nodiscard]] ImpactType getCreatureImpactTypeForDisplay(uint32_t displayId) const;
    [[nodiscard]] ImpactType getCreatureImpactTypeForSoundData(uint32_t creatureSoundDataId) const;
    [[nodiscard]] static ImpactType mapCreatureImpactEnum(uint32_t creatureImpactEnum);

    [[nodiscard]] uint32_t getCreatureExertionSoundId(uint32_t displayId, bool isCrit) const;
    [[nodiscard]] uint32_t getCreatureWoundSoundId(uint32_t displayId, bool isCrit) const;
    [[nodiscard]] uint32_t getCreatureCrushingWoundSoundId(uint32_t displayId) const;
    bool playCreatureCrushingWound(uint32_t displayId, const glm::vec3& renderPos, bool isLocal);

    [[nodiscard]] ResolvedWeaponAudio resolveWeaponFromDisplayInfo(uint32_t displayInfoId,
                                                                   uint8_t inventoryType) const;
    [[nodiscard]] ResolvedWeaponAudio resolveWeaponFromItemId(uint32_t itemId) const;
    [[nodiscard]] ImpactType resolveChestDisplayImpactType(uint32_t displayInfoId) const;
    [[nodiscard]] bool resolveShieldDisplayIsMetal(uint32_t displayInfoId) const;

    [[nodiscard]] static CombatOutcome classifyMeleeOutcome(
        bool isMiss, bool isCrit, bool isCrushing, bool isGlancing,
        uint32_t victimState, int32_t totalDamage, uint32_t blockedAmount);

    // --- Underwater tracking & M2 keyframe registration ---
    void setUnitUnderwater(uint64_t unitGuid, bool underwater);
    [[nodiscard]] bool isUnitUnderwater(uint64_t unitGuid) const;
    void setUnitHasKeyframeDriver(uint64_t unitGuid, bool hasDriver);
    [[nodiscard]] bool unitHasKeyframeDriver(uint64_t unitGuid) const;

    // --- Combat outcome queue & M2 keyframe entrypoints ---
    void queueCombatEvent(QueuedCombatEvent ev);
    void update(float deltaTime);
    void clearPendingCombatEvents();
    void clearUnitEvents(uint64_t unitGuid);
    [[nodiscard]] size_t pendingCombatEventCount(uint64_t attackerGuid = 0) const;

    /// M2 $CSS keyframe trigger: weapon swing whoosh (+ underwater 0.5f pitch per 0x467000).
    bool onUnitSwingEvent(uint64_t unitGuid, const glm::vec3& renderPos,
                          bool isLocalPlayer = false, bool offHand = false,
                          bool underwater = false, uint32_t fallbackWeaponSubClass = 13u);

    /// M2 $CAH / $AH0..3 keyframe trigger: pops queued combat outcome for attackerGuid
    /// and plays 3D/local WeaponImpactSounds / Miss / Dodge / Parry / Block / Deflect + victim wound.
    bool onUnitHitEvent(uint64_t attackerGuid,
                        const glm::vec3& attackerRenderPos = glm::vec3(0.0f),
                        bool isLocalAttacker = false);

    /// M2 $BWP keyframe trigger (0x62fbc0): Bow/Crossbow pull (1144) or Gun load (1147).
    bool onUnitBowPullEvent(uint64_t unitGuid, const glm::vec3& renderPos,
                            bool isLocalPlayer, uint32_t rangedSubClass);

    /// M2 $BWR keyframe trigger (0x61c254): Bow (1146), Gun (1148), Thrown (7140), Wand (5414).
    bool onUnitBowReleaseEvent(uint64_t unitGuid, const glm::vec3& renderPos,
                               bool isLocalPlayer, uint32_t rangedSubClass);

    /// M2 $SHL / $SHR keyframe trigger (0x467080): Material.dbc + SheatheSoundLookups.dbc.
    bool onUnitSheatheEvent(uint64_t unitGuid, const glm::vec3& renderPos,
                            bool isLocalPlayer, uint32_t itemClass, uint32_t itemSubClass,
                            int32_t material, bool isShield, bool unsheathe);

    // --- Direct playback API (backward-compatible + 3D/underwater overloads) ---
    void playWeaponSwing(WeaponSize size, bool isCrit = false, bool underwater = false);
    bool playWeaponSwing3D(WeaponSize size, bool isCrit, const glm::vec3& renderPos,
                           bool isLocal, bool underwater = false);
    void playWeaponMiss(bool twoHanded = false);
    bool playWeaponMiss3D(bool twoHanded, const glm::vec3& renderPos, bool isLocal);
    void playImpact(WeaponSize weaponSize, ImpactType impactType, bool isCrit = false);
    void playWeaponImpactBySubclass(uint32_t weaponSubClass, bool metalWeapon,
                                    ImpactType impactType, bool isCrit = false);
    bool playWeaponImpact3D(uint32_t weaponSubClass, bool metalWeapon,
                            ImpactType impactType, bool isCrit,
                            const glm::vec3& renderPos, bool isLocal);
    bool playSheathe(uint32_t itemClass, uint32_t itemSubClass, int32_t material,
                     bool isShield, bool unsheathe);
    bool playRangedShot(uint32_t rangedSubClass);

    // Diagnostics for tests
    [[nodiscard]] size_t weaponImpactRowCount() const { return weaponImpactRows_.size(); }
    [[nodiscard]] size_t sheatheLookupRowCount() const { return sheatheRows_.size(); }
    [[nodiscard]] size_t materialRowCount() const { return materials_.size(); }

    // Legacy unused enum preserved for source compatibility
    enum class PlayerRace {
        BLOOD_ELF_MALE,
        BLOOD_ELF_FEMALE,
        DRAENEI_MALE,
        DRAENEI_FEMALE
    };

private:
    struct WeaponImpactRow {
        std::array<uint32_t, 10> normal{};
        std::array<uint32_t, 10> crit{};
    };

    struct SheatheLookupRecord {
        uint32_t itemClass = 0;
        uint32_t itemSubClass = 0;
        int32_t itemEnvTypes = 0;
        uint32_t notShieldFlag = 0;
        uint32_t sheatheSoundId = 0;
        uint32_t unsheatheSoundId = 0;
    };

    struct MaterialRecord {
        uint32_t flags = 0;
        uint32_t foleySoundId = 0;
        uint32_t sheatheSoundId = 0;
        uint32_t unsheatheSoundId = 0;
    };

    struct CreatureCombatSoundRecord {
        uint32_t exertion = 0;
        uint32_t exertionCrit = 0;
        uint32_t injury = 0;
        uint32_t injuryCrit = 0;
        uint32_t injuryCrushing = 0;
        uint32_t creatureImpactEnum = 0;
    };

    struct ItemDisplayHint {
        std::string leftModel;
        std::string icon;
        uint32_t groupSoundId = 0;
    };

    void ensureWeaponDbcLoaded() const;
    bool playCombatSound(uint32_t soundId, const glm::vec3& renderPos,
                         bool isLocal, float pitch = 1.0f,
                         float overrideEntryVolume = -1.0f) const;
    bool executeSwingForEvent(QueuedCombatEvent& ev, const glm::vec3* overridePos,
                              bool hasUnderwaterOverride, bool underwaterOverride);
    bool executeHitForEvent(QueuedCombatEvent& ev);

    pipeline::AssetManager* assets_ = nullptr;
    float volumeScale_ = 1.0f;
    bool initialized_ = false;

    mutable bool weaponDbcBuilt_ = false;
    mutable std::array<std::array<uint32_t, 2>, 3> swingSoundIds_{{{233u, 234u}, {235u, 236u}, {237u, 238u}}};
    mutable std::unordered_map<uint32_t, WeaponSize> weaponSizeBySubClass_;
    mutable std::unordered_map<uint64_t, WeaponImpactRow> weaponImpactRows_;
    mutable std::vector<SheatheLookupRecord> sheatheRows_;
    mutable std::unordered_map<int32_t, MaterialRecord> materials_;
    mutable std::unordered_map<uint32_t, CreatureCombatSoundRecord> creatureSoundById_;
    mutable std::unordered_map<uint32_t, uint32_t> displayToCreatureSoundId_;
    struct ItemDbcEntry {
        uint32_t displayInfoId = 0;
        uint8_t inventoryType = 0;
    };

    mutable std::unordered_map<uint32_t, ItemDisplayHint> itemDisplayHints_;
    mutable std::unordered_map<uint32_t, ItemDbcEntry> itemDbcEntries_;
    mutable uint32_t miss1HSoundId_ = 7080u;
    mutable uint32_t miss2HSoundId_ = 7081u;
    mutable uint32_t deflectSoundId_ = 3262u;

    std::unordered_map<uint64_t, std::deque<QueuedCombatEvent>> pendingByAttacker_;
    std::unordered_map<uint64_t, float> recentUnqueuedSwingTimer_;
    std::unordered_set<uint64_t> underwaterUnits_;
    std::unordered_set<uint64_t> keyframeDrivenUnits_;

    AttackerExertionCallback attackerExertionCallback_;
    VictimWoundCallback victimWoundCallback_;
};

} // namespace audio
} // namespace wowee
