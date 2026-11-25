#pragma once

#include <vector>
#include <memory>
#include <string>
#include <cstdint>

namespace wowee {
namespace pipeline {
class AssetManager;
}

namespace audio {

/// Map TBC 2.4.3 Spell.dbc SchoolMask to its SoundEntries.dbc SpellFizzle ID
/// (1424 Fire, 1425 Frost, 1426 Nature, 1427 Shadow, 1428 Holy/Arcane; 0 for Physical).
[[nodiscard]] inline constexpr uint32_t spellFizzleSoundIdForSchoolMask(uint32_t schoolMask) noexcept {
    if (schoolMask & 0x04u) return 1424u; // Fire -> SpellFizzleFire
    if (schoolMask & 0x10u) return 1425u; // Frost -> SpellFizzleFrost
    if (schoolMask & 0x08u) return 1426u; // Nature -> SpellFizzleNature
    if (schoolMask & 0x20u) return 1427u; // Shadow -> SpellFizzleShadow
    if (schoolMask & (0x02u | 0x40u)) return 1428u; // Holy / Arcane -> SpellFizzleHoly
    return 0u;
}

class SpellSoundManager {
public:
    SpellSoundManager() = default;
    ~SpellSoundManager() = default;

    // Initialization
    bool initialize(pipeline::AssetManager* assets);
    void shutdown();

    // Volume control
    void setVolumeScale(float scale);
    [[nodiscard]] float getVolumeScale() const { return volumeScale_; }

    // Magic school types
    enum class MagicSchool {
        FIRE,
        FROST,
        HOLY,
        NATURE,
        SHADOW,
        ARCANE,
        PHYSICAL  // Non-magical abilities
    };

    // Spell power level
    enum class SpellPower {
        LOW,      // Weak spells, low level
        MEDIUM,   // Standard spells
        HIGH      // Powerful spells
    };

    // Spell casting sounds
    void playPrecast(MagicSchool school, SpellPower power);  // Channeling/preparation
    void stopPrecast();                                       // Stop precast sound early
    void playCast(MagicSchool school);                        // When spell fires
    void playImpact(MagicSchool school, SpellPower power);    // When spell hits target


private:
    struct SpellSample {
        std::string path;
        std::vector<uint8_t> data;
        bool loaded;
    };

    // Precast sound libraries (channeling)
    std::vector<SpellSample> precastFireLowSounds_;
    std::vector<SpellSample> precastFireMediumSounds_;
    std::vector<SpellSample> precastFireHighSounds_;
    std::vector<SpellSample> precastFrostLowSounds_;
    std::vector<SpellSample> precastFrostMediumSounds_;
    std::vector<SpellSample> precastFrostHighSounds_;
    std::vector<SpellSample> precastHolyLowSounds_;
    std::vector<SpellSample> precastHolyMediumSounds_;
    std::vector<SpellSample> precastHolyHighSounds_;
    std::vector<SpellSample> precastNatureLowSounds_;
    std::vector<SpellSample> precastNatureMediumSounds_;
    std::vector<SpellSample> precastNatureHighSounds_;
    std::vector<SpellSample> precastShadowLowSounds_;
    std::vector<SpellSample> precastShadowMediumSounds_;
    std::vector<SpellSample> precastShadowHighSounds_;
    std::vector<SpellSample> precastArcaneSounds_;

    // Cast sound libraries (spell release)
    std::vector<SpellSample> castFireSounds_;
    std::vector<SpellSample> castFrostSounds_;
    std::vector<SpellSample> castHolySounds_;
    std::vector<SpellSample> castNatureSounds_;
    std::vector<SpellSample> castShadowSounds_;

    // Impact sound libraries (spell hits)
    std::vector<SpellSample> impactFireballSounds_;
    std::vector<SpellSample> impactBlizzardSounds_;
    std::vector<SpellSample> impactHolySounds_;
    std::vector<SpellSample> impactArcaneMissileSounds_;

    // State tracking
    float volumeScale_ = 1.0f;
    bool initialized_ = false;
    uint32_t activePrecastId_ = 0;  // Handle from AudioEngine::playSound2DStoppable()

    // Helper methods
    bool loadSound(const std::string& path, SpellSample& sample, pipeline::AssetManager* assets);
    void playSound(const std::vector<SpellSample>& library, float volumeMultiplier = 1.0f);
    void playRandomSound(const std::vector<SpellSample>& library, float volumeMultiplier = 1.0f);
};

} // namespace audio
} // namespace wowee
