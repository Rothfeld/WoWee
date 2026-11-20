#pragma once

#include <vector>
#include <memory>
#include <string>
#include <cstdint>
#include <glm/vec3.hpp>

namespace wowee {
namespace pipeline {
class AssetManager;
}

namespace audio {

class AmbientSoundManager {
public:
    AmbientSoundManager() = default;
    ~AmbientSoundManager() = default;

    // Initialization
    bool initialize(pipeline::AssetManager* assets);
    void shutdown();

    // Main update loop - called from renderer
    void update(float deltaTime, const glm::vec3& cameraPos, bool isIndoor, bool isSwimming = false);
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

    // Sound libraries
    std::vector<AmbientSample> fireSoundsSmall_;
    std::vector<AmbientSample> fireSoundsLarge_;
    std::vector<AmbientSample> torchSounds_;
    std::vector<AmbientSample> waterSounds_;
    std::vector<AmbientSample> riverSounds_;
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

    // Water ambience libraries
    std::vector<AmbientSample> underwaterSounds_;

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
    float oceanLoopTime_ = 0.0f;
    float bellTollTime_ = 0.0f;
    bool wasSwimming_ = false;
    bool initialized_ = false;
    WeatherType currentWeather_ = WeatherType::NONE;
    CityType currentCity_ = CityType::NONE;

    // Active audio tracking
    struct ActiveSound {
        uint64_t emitterId;
        float startTime;
    };
    std::vector<ActiveSound> activeSounds_;

    // Helper methods
    void updatePositionalEmitters(float deltaTime, const glm::vec3& cameraPos);
    void updateAmbience(float deltaTime);
    void updateWeatherAmbience(float deltaTime, bool isIndoor);
    void updateWaterAmbience(float deltaTime, bool isSwimming);
    void updateBellTolls(float deltaTime);
    bool loadSound(const std::string& path, AmbientSample& sample, pipeline::AssetManager* assets);

};

} // namespace audio
} // namespace wowee
