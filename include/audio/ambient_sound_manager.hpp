#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>
#include <glm/vec3.hpp>

namespace wowee {
namespace pipeline {
class AssetManager;
}

namespace audio {

struct LiquidProximityCandidate {
    glm::vec3 position{0.0f};
    float distanceSq = 0.0f;
    uint8_t basicType = 0;     // 0=Water, 1=Ocean, 2=Magma, 3=Slime
    uint8_t soundSubtype = 0;  // 0=Still, 4=Slow, 8=Fast (SoundWaterType.dbc col 2)
};

class AmbientSoundManager {
public:
    static constexpr float kMaxLiquidProximityRange = 60.0f;

    AmbientSoundManager() = default;
    ~AmbientSoundManager() = default;

    // Initialization
    bool initialize(pipeline::AssetManager* assets);
    void shutdown();

    // Main update loop - called from AudioCoordinator
    void update(float deltaTime, const glm::vec3& cameraPos, bool isIndoor, bool isSwimming = false);

    /// Nearest wet liquid-body candidates around the 3D listener (fed from
    /// WaterRenderer::queryLiquidProximity before update()).
    void setLiquidProximity(std::span<const LiquidProximityCandidate> candidates);
    void clearLiquidProximity();

    /// Resolve TBC SoundWaterType.dbc (basicType 0..3, soundSubtype 0/4/8) -> SoundEntries.dbc ID.
    [[nodiscard]] uint32_t resolveWaterSoundEntryId(uint8_t basicType, uint8_t soundSubtype) const;
    [[nodiscard]] size_t waterSoundRuleCount() const { return waterTypeRules_.size(); }

    /// The ambience file to loop: the WMO room's or the area's SoundAmbience
    /// day or night sound, as AudioCoordinator picks it; empty for none.
    /// Loaded and restarted on change.
    void setAmbience(const std::string& path, float entryVolume = 1.0f);

    // Weather control
    enum class WeatherType {
        NONE,
        RAIN_LIGHT, RAIN_MEDIUM, RAIN_HEAVY,
        SNOW_LIGHT, SNOW_MEDIUM, SNOW_HEAVY,
        STORM_LIGHT, STORM_MEDIUM, STORM_HEAVY
    };
    void setWeather(WeatherType type);
    [[nodiscard]] WeatherType getCurrentWeather() const { return currentWeather_; }

    // The capital whose bells toll, from an AreaTable zone id.
    void setZoneId(uint32_t zoneId);

    // Capital for the bell tolls
    enum class CityType {
        NONE,
        STORMWIND,
        IRONFORGE,
        DARNASSUS,
        ORGRIMMAR,
        UNDERCITY,
        THUNDERBLUFF
    };
    void setCityType(CityType type);
    [[nodiscard]] CityType getCurrentCity() const { return currentCity_; }

    // Emitter management
    enum class AmbientType {
        FIREPLACE_SMALL,
        FIREPLACE_LARGE,
        TORCH,
        FOUNTAIN,
        WATER_SURFACE,
        RIVER,
        WATERFALL,
        WIND,
        BIRD_DAY,
        CRICKET_NIGHT,
        OWL_NIGHT
    };

    uint64_t addEmitter(const glm::vec3& position, AmbientType type);

    // Volume control
    void setVolumeScale(float scale);
    [[nodiscard]] float getVolumeScale() const { return volumeScale_; }
    void setBellVolumeScale(float scale) { bellVolumeScale_ = scale; }
    [[nodiscard]] float getBellVolumeScale() const { return bellVolumeScale_; }

private:
    struct AmbientEmitter {
        uint64_t id;
        AmbientType type;
        glm::vec3 position;
        bool active;
        float lastPlayTime;
        float loopInterval;  // For periodic/looping sounds
    };

    struct AmbientSample {
        std::string path;
        std::vector<uint8_t> data;
        bool loaded = false;
    };

    struct WaterSoundBank {
        uint32_t soundEntryId = 0;
        uint32_t flags = 0;
        float volume = 0.5f;
        float minDistance = 8.0f;
        float maxDistance = 45.0f;
        AmbientSample sample{};
    };

    struct WaterTypeRule {
        uint8_t basicType = 0;
        uint8_t soundSubtype = 0;
        uint32_t soundEntryId = 0;
    };

    struct ActiveWaterLoop {
        uint32_t soundId = 0;
        uint32_t soundEntryId = 0;
        float startedVolumeScale = 0.0f;
    };

    // Sound libraries
    std::vector<AmbientSample> fireSoundsSmall_;
    std::vector<AmbientSample> fireSoundsLarge_;
    std::vector<AmbientSample> torchSounds_;
    std::vector<AmbientSample> waterfallSounds_;
    std::vector<AmbientSample> fountainSounds_;
    AmbientSample ambience_{};
    float ambienceEntryVolume_ = 1.0f;
    float ambienceDuration_ = 0.0f;
    float ambienceLoopTime_ = 0.0f;
    uint32_t ambienceSoundId_ = 0;
    pipeline::AssetManager* assets_ = nullptr;

    // Weather sound libraries
    std::vector<AmbientSample> rainLightSounds_;
    std::vector<AmbientSample> rainMediumSounds_;
    std::vector<AmbientSample> rainHeavySounds_;
    std::vector<AmbientSample> snowLightSounds_;
    std::vector<AmbientSample> snowMediumSounds_;
    std::vector<AmbientSample> snowHeavySounds_;
    std::vector<AmbientSample> stormLightSounds_;
    std::vector<AmbientSample> stormMediumSounds_;
    std::vector<AmbientSample> stormHeavySounds_;

    // Water & SoundWaterType.dbc proximity libraries
    std::vector<AmbientSample> underwaterSounds_;
    uint32_t underwaterSoundId_ = 0;
    float underwaterStartedScale_ = 0.0f;
    std::vector<WaterTypeRule> waterTypeRules_;
    std::unordered_map<uint32_t, WaterSoundBank> waterSoundBanks_;
    std::vector<LiquidProximityCandidate> liquidCandidates_;
    std::array<ActiveWaterLoop, 4> activeWaterLoops_{};

    // City bell sounds
    std::vector<AmbientSample> bellAllianceSounds_;
    std::vector<AmbientSample> bellHordeSounds_;
    std::vector<AmbientSample> bellNightElfSounds_;
    std::vector<AmbientSample> bellTribalSounds_;

    // Active emitters
    std::vector<AmbientEmitter> emitters_;
    uint64_t nextEmitterId_ = 1;

    // State tracking
    float volumeScale_ = 1.0f;
    float bellVolumeScale_ = 0.5f;
    float weatherLoopTime_ = 0.0f;
    float bellTollTime_ = 0.0f;
    bool wasSwimming_ = false;
    bool initialized_ = false;
    WeatherType currentWeather_ = WeatherType::NONE;
    CityType currentCity_ = CityType::NONE;

    // Helper methods
    void loadSoundWaterTypeTable(pipeline::AssetManager* assets);
    void stopAllWaterLoops();
    void updatePositionalEmitters(float deltaTime, const glm::vec3& cameraPos);
    void updateAmbience(float deltaTime);
    void updateWeatherAmbience(float deltaTime, bool isIndoor);
    void updateWaterAmbience(float deltaTime, bool isSwimming);
    void updateBellTolls(float deltaTime);
    bool loadSound(const std::string& path, AmbientSample& sample, pipeline::AssetManager* assets);
};

} // namespace audio
} // namespace wowee
