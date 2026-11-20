#include "audio/combat_sound_manager.hpp"
#include "audio/sample_load.hpp"
#include "audio/audio_engine.hpp"
#include "audio/sound_entries.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "core/logger.hpp"
#include <random>

namespace wowee {
namespace audio {

namespace {
    std::random_device rd;
    std::mt19937 gen(rd());
}

bool CombatSoundManager::initialize(pipeline::AssetManager* assets) {
    if (!assets) {
        LOG_ERROR("CombatSoundManager: AssetManager is null");
        return false;
    }
    assets_ = assets;
    weaponDbcBuilt_ = false;
    weaponImpactRows_.clear();
    sheatheRows_.clear();

    LOG_INFO("CombatSoundManager: Initializing...");

    // Load weapon swing sounds
    swingSmallSounds_.resize(3);
    loadSound("Sound\\Item\\Weapons\\WeaponSwings\\mWooshSmall1.wav", swingSmallSounds_[0], assets);
    loadSound("Sound\\Item\\Weapons\\WeaponSwings\\mWooshSmall2.wav", swingSmallSounds_[1], assets);
    loadSound("Sound\\Item\\Weapons\\WeaponSwings\\mWooshSmall3.wav", swingSmallSounds_[2], assets);

    swingMediumSounds_.resize(3);
    loadSound("Sound\\Item\\Weapons\\WeaponSwings\\mWooshMedium1.wav", swingMediumSounds_[0], assets);
    loadSound("Sound\\Item\\Weapons\\WeaponSwings\\mWooshMedium2.wav", swingMediumSounds_[1], assets);
    loadSound("Sound\\Item\\Weapons\\WeaponSwings\\mWooshMedium3.wav", swingMediumSounds_[2], assets);

    swingLargeSounds_.resize(3);
    loadSound("Sound\\Item\\Weapons\\WeaponSwings\\mWooshLarge1.wav", swingLargeSounds_[0], assets);
    loadSound("Sound\\Item\\Weapons\\WeaponSwings\\mWooshLarge2.wav", swingLargeSounds_[1], assets);
    loadSound("Sound\\Item\\Weapons\\WeaponSwings\\mWooshLarge3.wav", swingLargeSounds_[2], assets);

    swingSmallCritSounds_.resize(1);
    loadSound("Sound\\Item\\Weapons\\WeaponSwings\\mWooshSmallCrit.wav", swingSmallCritSounds_[0], assets);

    swingMediumCritSounds_.resize(1);
    loadSound("Sound\\Item\\Weapons\\WeaponSwings\\mWooshMediumCrit.wav", swingMediumCritSounds_[0], assets);

    swingLargeCritSounds_.resize(1);
    loadSound("Sound\\Item\\Weapons\\WeaponSwings\\mWooshLargeCrit.wav", swingLargeCritSounds_[0], assets);

    missWhoosh1HSounds_.resize(1);
    loadSound("Sound\\Item\\Weapons\\MissSwings\\MissWhoosh1Handed.wav", missWhoosh1HSounds_[0], assets);

    missWhoosh2HSounds_.resize(1);
    loadSound("Sound\\Item\\Weapons\\MissSwings\\MissWhoosh2Handed.wav", missWhoosh2HSounds_[0], assets);

    // Load impact sounds (using 1H axe as base)
    hitFleshSounds_.resize(3);
    loadSound("Sound\\Item\\Weapons\\Axe1H\\m1hAxeHitFlesh1a.wav", hitFleshSounds_[0], assets);
    loadSound("Sound\\Item\\Weapons\\Axe1H\\m1hAxeHitFlesh1b.wav", hitFleshSounds_[1], assets);
    loadSound("Sound\\Item\\Weapons\\Axe1H\\m1hAxeHitFlesh1c.wav", hitFleshSounds_[2], assets);

    hitChainSounds_.resize(3);
    loadSound("Sound\\Item\\Weapons\\Axe1H\\m1hAxeHitChain1a.wav", hitChainSounds_[0], assets);
    loadSound("Sound\\Item\\Weapons\\Axe1H\\m1hAxeHitChain1b.wav", hitChainSounds_[1], assets);
    loadSound("Sound\\Item\\Weapons\\Axe1H\\m1hAxeHitChain1c.wav", hitChainSounds_[2], assets);

    hitPlateSounds_.resize(3);
    loadSound("Sound\\Item\\Weapons\\Axe1H\\m1hAxeHitPlate1a.wav", hitPlateSounds_[0], assets);
    loadSound("Sound\\Item\\Weapons\\Axe1H\\m1hAxeHitPlate1b.wav", hitPlateSounds_[1], assets);
    loadSound("Sound\\Item\\Weapons\\Axe1H\\m1hAxeHitPlate1c.wav", hitPlateSounds_[2], assets);

    hitShieldSounds_.resize(3);
    loadSound("Sound\\Item\\Weapons\\Axe1H\\m1hAxeHitMetalShield1a.wav", hitShieldSounds_[0], assets);
    loadSound("Sound\\Item\\Weapons\\Axe1H\\m1hAxeHitMetalShield1b.wav", hitShieldSounds_[1], assets);
    loadSound("Sound\\Item\\Weapons\\Axe1H\\m1hAxeHitMetalShield1c.wav", hitShieldSounds_[2], assets);

    hitMetalWeaponSounds_.resize(1);
    loadSound("Sound\\Item\\Weapons\\Axe1H\\m1hAxeHitMetalWeapon1a.wav", hitMetalWeaponSounds_[0], assets);

    hitWoodSounds_.resize(3);
    loadSound("Sound\\Item\\Weapons\\Axe1H\\m1hAxeHitWood1A.wav", hitWoodSounds_[0], assets);
    loadSound("Sound\\Item\\Weapons\\Axe1H\\m1hAxeHitWood1B.wav", hitWoodSounds_[1], assets);
    loadSound("Sound\\Item\\Weapons\\Axe1H\\m1hAxeHitWood1C.wav", hitWoodSounds_[2], assets);

    hitStoneSounds_.resize(3);
    loadSound("Sound\\Item\\Weapons\\Axe1H\\m1hAxeHitStone1A.wav", hitStoneSounds_[0], assets);
    loadSound("Sound\\Item\\Weapons\\Axe1H\\m1hAxeHitStone1B.wav", hitStoneSounds_[1], assets);
    loadSound("Sound\\Item\\Weapons\\Axe1H\\m1hAxeHitStone1C.wav", hitStoneSounds_[2], assets);

    // Load crit impact sounds
    hitFleshCritSounds_.resize(1);
    loadSound("Sound\\Item\\Weapons\\Axe1H\\m1hAxeHitFleshCrit.wav", hitFleshCritSounds_[0], assets);

    hitChainCritSounds_.resize(1);
    loadSound("Sound\\Item\\Weapons\\Axe1H\\m1hAxeHitChainCrit.wav", hitChainCritSounds_[0], assets);

    hitPlateCritSounds_.resize(1);
    loadSound("Sound\\Item\\Weapons\\Axe1H\\m1hAxeHitPlateCrit.wav", hitPlateCritSounds_[0], assets);

    hitShieldCritSounds_.resize(1);
    loadSound("Sound\\Item\\Weapons\\Axe1H\\m1hAxeHitMetalShieldCrit.wav", hitShieldCritSounds_[0], assets);

    initialized_ = true;
    LOG_INFO("CombatSoundManager: Initialization complete");
    return true;
}

void CombatSoundManager::shutdown() {
    initialized_ = false;
    weaponDbcBuilt_ = false;
    weaponImpactRows_.clear();
    sheatheRows_.clear();
    assets_ = nullptr;
}

bool CombatSoundManager::loadSound(const std::string& path, CombatSample& sample, pipeline::AssetManager* assets) {
    // Quietly: not every sound in this bank ships with every install.
    return loadSampleFile(path, sample, assets, nullptr);
}

void CombatSoundManager::playSound(const std::vector<CombatSample>& library, float volumeMultiplier) {
    if (!initialized_ || library.empty() || !library[0].loaded) return;

    float volume = 0.8f * volumeScale_ * volumeMultiplier;
    AudioEngine::instance().playSound2D(library[0].data, volume, 1.0f);
}

void CombatSoundManager::playRandomSound(const std::vector<CombatSample>& library, float volumeMultiplier) {
    if (!initialized_) return;
    // Among the ones that loaded - see pickLoadedSample. The base volume is
    // this bank's own and stays here.
    const CombatSample* chosen = pickLoadedSample(library, gen);
    if (!chosen) return;
    const float volume = 0.8f * volumeScale_ * volumeMultiplier;
    AudioEngine::instance().playSound2D(chosen->data, volume, 1.0f);
}

void CombatSoundManager::setVolumeScale(float scale) {
    volumeScale_ = std::max(0.0f, std::min(1.0f, scale));
}

void CombatSoundManager::playWeaponSwing(WeaponSize size, bool isCrit) {
    if (isCrit) {
        switch (size) {
            case WeaponSize::SMALL:
                playSound(swingSmallCritSounds_);
                break;
            case WeaponSize::MEDIUM:
                playSound(swingMediumCritSounds_);
                break;
            case WeaponSize::LARGE:
                playSound(swingLargeCritSounds_);
                break;
        }
    } else {
        switch (size) {
            case WeaponSize::SMALL:
                playRandomSound(swingSmallSounds_);
                break;
            case WeaponSize::MEDIUM:
                playRandomSound(swingMediumSounds_);
                break;
            case WeaponSize::LARGE:
                playRandomSound(swingLargeSounds_);
                break;
        }
    }
}

void CombatSoundManager::playWeaponMiss(bool twoHanded) {
    if (twoHanded) {
        playSound(missWhoosh2HSounds_);
    } else {
        playSound(missWhoosh1HSounds_);
    }
}

void CombatSoundManager::playImpact([[maybe_unused]] WeaponSize weaponSize, ImpactType impactType, bool isCrit) {
    // Select appropriate impact sound library
    const std::vector<CombatSample>* normalLibrary = nullptr;
    const std::vector<CombatSample>* critLibrary = nullptr;

    switch (impactType) {
        case ImpactType::FLESH:
            normalLibrary = &hitFleshSounds_;
            critLibrary = &hitFleshCritSounds_;
            break;
        case ImpactType::CHAIN:
            normalLibrary = &hitChainSounds_;
            critLibrary = &hitChainCritSounds_;
            break;
        case ImpactType::PLATE:
            normalLibrary = &hitPlateSounds_;
            critLibrary = &hitPlateCritSounds_;
            break;
        case ImpactType::SHIELD:
            normalLibrary = &hitShieldSounds_;
            critLibrary = &hitShieldCritSounds_;
            break;
        case ImpactType::METAL_WEAPON:
            normalLibrary = &hitMetalWeaponSounds_;
            break;
        case ImpactType::WOOD:
            normalLibrary = &hitWoodSounds_;
            break;
        case ImpactType::STONE:
            normalLibrary = &hitStoneSounds_;
            break;
    }

    if (!normalLibrary) return;

    // Play crit version if available, otherwise use normal
    if (isCrit && critLibrary && !critLibrary->empty()) {
        playSound(*critLibrary, 1.2f);  // Crits slightly louder
    } else {
        playRandomSound(*normalLibrary);
    }
}

void CombatSoundManager::ensureWeaponDbcLoaded() {
    if (weaponDbcBuilt_ || !assets_ || !assets_->isInitialized()) return;
    weaponDbcBuilt_ = true;

    if (auto dbc = assets_->loadDBC("WeaponImpactSounds.dbc");
        dbc && dbc->isLoaded() && dbc->getFieldCount() >= 18) {
        for (uint32_t r = 0; r < dbc->getRecordCount(); ++r) {
            const uint32_t subClass = dbc->getUInt32(r, 1);
            const uint32_t parryType = dbc->getUInt32(r, 2) ? 1u : 0u;
            const uint64_t key = (static_cast<uint64_t>(subClass) << 1u) | parryType;
            weaponImpactRows_[key] = WeaponImpactRow{
                .flesh = dbc->getUInt32(r, 3),
                .chain = dbc->getUInt32(r, 4),
                .plate = dbc->getUInt32(r, 5),
                .shieldMetal = dbc->getUInt32(r, 6),
                .shieldWood = dbc->getUInt32(r, 7),
                .parry = dbc->getUInt32(r, 8),
                .critFlesh = dbc->getUInt32(r, 13),
                .critChain = dbc->getUInt32(r, 14),
                .critPlate = dbc->getUInt32(r, 15),
                .critShieldMetal = dbc->getUInt32(r, 16),
            };
        }
    }

    if (auto dbc = assets_->loadDBC("SheatheSoundLookups.dbc");
        dbc && dbc->isLoaded() && dbc->getFieldCount() >= 7) {
        for (uint32_t r = 0; r < dbc->getRecordCount(); ++r) {
            const uint32_t itemClass = dbc->getUInt32(r, 1);
            const uint32_t subClass = dbc->getUInt32(r, 2);
            const uint32_t matEnv = dbc->getUInt32(r, 3);
            const uint64_t key = (static_cast<uint64_t>(itemClass) << 16u) |
                                 (static_cast<uint64_t>(subClass & 0xFFu) << 8u) |
                                 (matEnv & 0xFFu);
            sheatheRows_[key] = SheatheSoundRow{
                .sheatheSoundId = dbc->getUInt32(r, 5),
                .unsheatheSoundId = dbc->getUInt32(r, 6),
            };
        }
    }
}

void CombatSoundManager::playWeaponImpactBySubclass(uint32_t weaponSubClass, bool metalWeapon,
                                                    ImpactType impactType, bool isCrit) {
    if (!initialized_ || volumeScale_ <= 0.0f) return;
    ensureWeaponDbcLoaded();

    const uint64_t keyPrimary = (static_cast<uint64_t>(weaponSubClass) << 1u) | (metalWeapon ? 1u : 0u);
    const uint64_t keyFallback = (static_cast<uint64_t>(weaponSubClass) << 1u) | (metalWeapon ? 0u : 1u);
    auto it = weaponImpactRows_.find(keyPrimary);
    if (it == weaponImpactRows_.end()) it = weaponImpactRows_.find(keyFallback);

    if (it != weaponImpactRows_.end() && assets_) {
        uint32_t soundId = 0;
        switch (impactType) {
            case ImpactType::FLESH:
                soundId = (isCrit && it->second.critFlesh) ? it->second.critFlesh : it->second.flesh;
                break;
            case ImpactType::CHAIN:
                soundId = (isCrit && it->second.critChain) ? it->second.critChain : it->second.chain;
                break;
            case ImpactType::PLATE:
                soundId = (isCrit && it->second.critPlate) ? it->second.critPlate : it->second.plate;
                break;
            case ImpactType::SHIELD:
                soundId = (isCrit && it->second.critShieldMetal)
                    ? it->second.critShieldMetal
                    : (it->second.shieldMetal ? it->second.shieldMetal : it->second.shieldWood);
                break;
            case ImpactType::METAL_WEAPON:
                soundId = it->second.parry;
                break;
            case ImpactType::WOOD:
            case ImpactType::STONE:
                break;
        }
        if (soundId != 0 && SoundEntryTable::instance().play2D(assets_, soundId, volumeScale_)) {
            return;
        }
    }

    playImpact(WeaponSize::MEDIUM, impactType, isCrit);
}

bool CombatSoundManager::playSheathe(uint32_t itemClass, uint32_t itemSubClass, int32_t material,
                                     bool isShield, bool unsheathe) {
    if (!initialized_ || !assets_ || volumeScale_ <= 0.0f) return false;
    ensureWeaponDbcLoaded();

    const uint32_t matEnv = isShield ? 0u : (material == 1 ? 1u : 2u);
    const uint64_t key = (static_cast<uint64_t>(itemClass) << 16u) |
                         (static_cast<uint64_t>(itemSubClass & 0xFFu) << 8u) |
                         (matEnv & 0xFFu);
    uint32_t soundId = 0;
    if (auto it = sheatheRows_.find(key); it != sheatheRows_.end()) {
        soundId = unsheathe ? it->second.unsheatheSoundId : it->second.sheatheSoundId;
    }
    if (soundId == 0) {
        if (isShield) soundId = unsheathe ? 701u : 696u;
        else if (material == 1) soundId = unsheathe ? 700u : 698u;
        else soundId = unsheathe ? 699u : 697u;
    }
    return SoundEntryTable::instance().play2D(assets_, soundId, volumeScale_);
}

bool CombatSoundManager::playRangedShot(uint32_t rangedSubClass) {
    if (!initialized_ || !assets_ || volumeScale_ <= 0.0f) return false;
    // TBC 2.4.3 SoundEntries IDs: 1146=BowShoot, 1148=GunFire01, 5414=WandShoot, 7140=ThrownThrow
    uint32_t soundId = 1146u;
    switch (rangedSubClass) {
        case 3:  soundId = 1148u; break; // Gun
        case 16: soundId = 7140u; break; // Thrown
        case 19: soundId = 5414u; break; // Wand
        case 2:                          // Bow
        case 18:                         // Crossbow
        default: soundId = 1146u; break;
    }
    return SoundEntryTable::instance().play2D(assets_, soundId, volumeScale_);
}
} // namespace audio
} // namespace wowee
