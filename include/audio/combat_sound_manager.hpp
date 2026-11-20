#pragma once

#include <vector>
#include <unordered_map>
#include <memory>
#include <string>
#include <cstdint>

namespace wowee {
namespace pipeline {
class AssetManager;
}

namespace audio {

class CombatSoundManager {
public:
    CombatSoundManager() = default;
    ~CombatSoundManager() = default;

    // Initialization
    bool initialize(pipeline::AssetManager* assets);
    void shutdown();

    // Volume control
    void setVolumeScale(float scale);
    [[nodiscard]] float getVolumeScale() const { return volumeScale_; }

    // Weapon swing sounds (whoosh sounds before impact)
    enum class WeaponSize {
        SMALL,   // 1H weapons (daggers, swords, maces)
        MEDIUM,  // 2H weapons (2H swords, axes)
        LARGE    // 2H heavy weapons (polearms, staves)
    };

    void playWeaponSwing(WeaponSize size, bool isCrit = false);
    void playWeaponMiss(bool twoHanded = false);

    // Impact sounds (when weapon hits target)
    enum class ImpactType {
        FLESH,        // Hit unarmored/cloth
        CHAIN,        // Hit chain armor
        PLATE,        // Hit plate armor
        SHIELD,       // Hit shield
        METAL_WEAPON, // Parry/weapon clash
        WOOD,         // Hit wooden objects
        STONE         // Hit stone/rock
    };

    void playImpact(WeaponSize weaponSize, ImpactType impactType, bool isCrit = false);
    void playWeaponImpactBySubclass(uint32_t weaponSubClass, bool metalWeapon,
                                    ImpactType impactType, bool isCrit = false);
    bool playSheathe(uint32_t itemClass, uint32_t itemSubClass, int32_t material,
                     bool isShield, bool unsheathe);
    bool playRangedShot(uint32_t rangedSubClass);


    // Player character vocals
    enum class PlayerRace {
        BLOOD_ELF_MALE,
        BLOOD_ELF_FEMALE,
        DRAENEI_MALE,
        DRAENEI_FEMALE
    };


private:
    struct CombatSample {
        std::string path;
        std::vector<uint8_t> data;
        bool loaded;
    };

    // Weapon swing sound libraries
    std::vector<CombatSample> swingSmallSounds_;
    std::vector<CombatSample> swingMediumSounds_;
    std::vector<CombatSample> swingLargeSounds_;
    std::vector<CombatSample> swingSmallCritSounds_;
    std::vector<CombatSample> swingMediumCritSounds_;
    std::vector<CombatSample> swingLargeCritSounds_;
    std::vector<CombatSample> missWhoosh1HSounds_;
    std::vector<CombatSample> missWhoosh2HSounds_;

    // Impact sound libraries (using 1H axe as base - similar across weapon types)
    std::vector<CombatSample> hitFleshSounds_;
    std::vector<CombatSample> hitChainSounds_;
    std::vector<CombatSample> hitPlateSounds_;
    std::vector<CombatSample> hitShieldSounds_;
    std::vector<CombatSample> hitMetalWeaponSounds_;
    std::vector<CombatSample> hitWoodSounds_;
    std::vector<CombatSample> hitStoneSounds_;
    std::vector<CombatSample> hitFleshCritSounds_;
    std::vector<CombatSample> hitChainCritSounds_;
    std::vector<CombatSample> hitPlateCritSounds_;
    std::vector<CombatSample> hitShieldCritSounds_;

    // Emote sounds

    // Player character vocal libraries




    // State tracking
    pipeline::AssetManager* assets_ = nullptr;
    float volumeScale_ = 1.0f;
    bool initialized_ = false;
    bool weaponDbcBuilt_ = false;

    struct WeaponImpactRow {
        uint32_t flesh = 0, chain = 0, plate = 0, shieldMetal = 0, shieldWood = 0, parry = 0;
        uint32_t critFlesh = 0, critChain = 0, critPlate = 0, critShieldMetal = 0;
    };
    std::unordered_map<uint64_t, WeaponImpactRow> weaponImpactRows_; // (subClass << 1) | parrySoundType

    struct SheatheSoundRow {
        uint32_t sheatheSoundId = 0;
        uint32_t unsheatheSoundId = 0;
    };
    std::unordered_map<uint64_t, SheatheSoundRow> sheatheRows_; // (itemClass << 16) | (subClass << 8) | matEnv

    void ensureWeaponDbcLoaded();

    // Helper methods
    bool loadSound(const std::string& path, CombatSample& sample, pipeline::AssetManager* assets);
    void playSound(const std::vector<CombatSample>& library, float volumeMultiplier = 1.0f);
    void playRandomSound(const std::vector<CombatSample>& library, float volumeMultiplier = 1.0f);
};

} // namespace audio
} // namespace wowee
