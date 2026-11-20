#include "audio/ambient_sound_manager.hpp"
#include "audio/sample_load.hpp"
#include "audio/audio_engine.hpp"
#include "pipeline/asset_manager.hpp"
#include "core/logger.hpp"
#include <random>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>

namespace wowee {
namespace audio {

namespace {
    // Distance thresholds (in game units)
    constexpr float MAX_FIRE_DISTANCE = 20.0f;
    constexpr float MAX_WATER_DISTANCE = 35.0f;
    constexpr float MAX_AMBIENT_DISTANCE = 50.0f;

    // Volume settings
    constexpr float FIRE_VOLUME = 0.7f;
    constexpr float WATER_VOLUME = 0.5f;
    // WoWee's level for the area/room ambience bed. SoundEntries carries a
    // per-entry volume that is not read yet.
    constexpr float AMBIENCE_VOLUME = 0.4f;

    // Timing settings (seconds)
    constexpr float FIRE_LOOP_INTERVAL = 3.0f;  // Fire crackling loop length

    std::random_device rd;
    std::mt19937 gen(rd());

    float randomFloat(float min, float max) {
        std::uniform_real_distribution<float> dist(min, max);
        return dist(gen);
    }

    /// Playing time of a RIFF/WAVE file in seconds: the data chunk's size over
    /// the fmt chunk's byte rate. Null when the header is not one.
    std::optional<float> wavDurationSeconds(const std::vector<uint8_t>& wav) {
        const auto u32 = [&](size_t at) {
            return static_cast<uint32_t>(wav[at]) | (static_cast<uint32_t>(wav[at + 1]) << 8) |
                   (static_cast<uint32_t>(wav[at + 2]) << 16) | (static_cast<uint32_t>(wav[at + 3]) << 24);
        };
        if (wav.size() < 12 || std::memcmp(wav.data(), "RIFF", 4) != 0 ||
            std::memcmp(wav.data() + 8, "WAVE", 4) != 0)
            return std::nullopt;
        uint32_t byteRate = 0, dataSize = 0;
        for (size_t at = 12; at + 8 <= wav.size();) {
            const uint32_t size = u32(at + 4);
            if (std::memcmp(wav.data() + at, "fmt ", 4) == 0 && at + 8 + 12 <= wav.size())
                byteRate = u32(at + 8 + 8);
            else if (std::memcmp(wav.data() + at, "data", 4) == 0)
                dataSize = size;
            at += 8 + size + (size & 1u);
        }
        if (byteRate == 0 || dataSize == 0) return std::nullopt;
        return static_cast<float>(dataSize) / static_cast<float>(byteRate);
    }
}

bool AmbientSoundManager::initialize(pipeline::AssetManager* assets) {
    if (!assets) {
        LOG_ERROR("AmbientSoundManager: AssetManager is null");
        return false;
    }
    assets_ = assets;

    LOG_INFO("AmbientSoundManager: Initializing...");

    // Load fire sounds
    fireSoundsSmall_.resize(1);
    loadSound("Sound\\Doodad\\CampFireSmallLoop.wav", fireSoundsSmall_[0], assets);

    fireSoundsLarge_.resize(1);
    loadSound("Sound\\Doodad\\CampFireLargeLoop.wav", fireSoundsLarge_[0], assets);

    torchSounds_.resize(1);
    loadSound("Sound\\Doodad\\TorchFireLoop.wav", torchSounds_[0], assets);

    // Load water sounds
    waterSounds_.resize(1);
    loadSound("Sound\\Ambience\\Water\\River_LakeStillA.wav", waterSounds_[0], assets);

    riverSounds_.resize(1);
    loadSound("Sound\\Ambience\\Water\\RiverSlowA.wav", riverSounds_[0], assets);

    waterfallSounds_.resize(1);
    loadSound("Sound\\Doodad\\WaterFallSmall.wav", waterfallSounds_[0], assets);

    // Load fountain sounds
    fountainSounds_.resize(1);
    loadSound("Sound\\Doodad\\FountainSmallMediumLoop.wav", fountainSounds_[0], assets);

    // Load weather sounds (TBC 2.4.3 uses Rain*Loop.wav and SandStorm*.wav)
    rainLightSounds_.resize(1);
    bool rainLightLoaded = loadSound("Sound\\Ambience\\Weather\\RainLightLoop.wav", rainLightSounds_[0], assets) ||
                           loadSound("Sound\\Ambience\\Weather\\RainLight.wav", rainLightSounds_[0], assets);

    rainMediumSounds_.resize(1);
    bool rainMediumLoaded = loadSound("Sound\\Ambience\\Weather\\RainMediumLoop.wav", rainMediumSounds_[0], assets) ||
                            loadSound("Sound\\Ambience\\Weather\\RainMedium.wav", rainMediumSounds_[0], assets);

    rainHeavySounds_.resize(1);
    bool rainHeavyLoaded = loadSound("Sound\\Ambience\\Weather\\RainHeavyLoop.wav", rainHeavySounds_[0], assets) ||
                           loadSound("Sound\\Ambience\\Weather\\RainHeavy.wav", rainHeavySounds_[0], assets);

    snowLightSounds_.resize(1);
    bool snowLightLoaded = loadSound("Sound\\Ambience\\Weather\\SnowLight.wav", snowLightSounds_[0], assets);

    snowMediumSounds_.resize(1);
    bool snowMediumLoaded = loadSound("Sound\\Ambience\\Weather\\SnowMedium.wav", snowMediumSounds_[0], assets);

    snowHeavySounds_.resize(1);
    bool snowHeavyLoaded = loadSound("Sound\\Ambience\\Weather\\SnowHeavy.wav", snowHeavySounds_[0], assets);

    stormLightSounds_.resize(1);
    bool stormLightLoaded = loadSound("Sound\\Ambience\\Weather\\SandStormLight.wav", stormLightSounds_[0], assets);

    stormMediumSounds_.resize(1);
    bool stormMediumLoaded = loadSound("Sound\\Ambience\\Weather\\SandStormMedium.wav", stormMediumSounds_[0], assets);

    stormHeavySounds_.resize(1);
    bool stormHeavyLoaded = loadSound("Sound\\Ambience\\Weather\\SandStormHeavy.wav", stormHeavySounds_[0], assets);

    // Load water ambience sounds (TBC 2.4.3 uses Sound\Ambience\UndwaterLoop.wav)
    underwaterSounds_.resize(1);
    bool underwaterLoaded = loadSound("Sound\\Ambience\\UndwaterLoop.wav", underwaterSounds_[0], assets) ||
                            loadSound("Sound\\Ambience\\Water\\UnderwaterSwim.wav", underwaterSounds_[0], assets);

    // Load bell toll sounds
    bellAllianceSounds_.resize(1);
    bool bellAllianceLoaded = loadSound("Sound\\Doodad\\BellTollAlliance.wav", bellAllianceSounds_[0], assets);

    bellHordeSounds_.resize(1);
    bool bellHordeLoaded = loadSound("Sound\\Doodad\\BellTollHorde.wav", bellHordeSounds_[0], assets);

    bellNightElfSounds_.resize(1);
    bool bellNightElfLoaded = loadSound("Sound\\Doodad\\BellTollNightElf.wav", bellNightElfSounds_[0], assets);

    bellTribalSounds_.resize(1);
    bool bellTribalLoaded = loadSound("Sound\\Doodad\\BellTollTribal.wav", bellTribalSounds_[0], assets);

    LOG_INFO("AmbientSoundManager: Weather sounds - Rain: ", (rainLightLoaded && rainMediumLoaded && rainHeavyLoaded) ? "YES" : "NO",
             ", Snow: ", (snowLightLoaded && snowMediumLoaded && snowHeavyLoaded) ? "YES" : "NO",
             ", Storm: ", (stormLightLoaded && stormMediumLoaded && stormHeavyLoaded) ? "YES" : "NO");
    LOG_INFO("AmbientSoundManager: Water sounds - Underwater: ",
             underwaterLoaded ? "YES" : "NO");
    LOG_INFO("AmbientSoundManager: Bell tolls - Alliance: ", bellAllianceLoaded ? "YES" : "NO",
             ", Horde: ", bellHordeLoaded ? "YES" : "NO",
             ", NightElf: ", bellNightElfLoaded ? "YES" : "NO",
             ", Tribal: ", bellTribalLoaded ? "YES" : "NO");

    initialized_ = true;
    LOG_INFO("AmbientSoundManager: Initialization complete");
    return true;
}

void AmbientSoundManager::shutdown() {
    if (ambienceSoundId_ != 0) AudioEngine::instance().stopSound(ambienceSoundId_);
    ambienceSoundId_ = 0;
    ambience_ = AmbientSample{};
    emitters_.clear();
    activeSounds_.clear();
    initialized_ = false;
}

bool AmbientSoundManager::loadSound(const std::string& path, AmbientSample& sample, pipeline::AssetManager* assets) {
    return loadSampleFile(path, sample, assets, "AmbientSoundManager");
}

void AmbientSoundManager::setAmbience(const std::string& path, float entryVolume) {
    // AudioCoordinator asks every frame, from before the asset manager is
    // handed over (login, world entry); the request is taken once it is.
    if (!initialized_) return;
    const float clampedEntry = std::clamp(entryVolume, 0.0f, 1.0f);
    if (path == ambience_.path && std::abs(clampedEntry - ambienceEntryVolume_) < 1e-3f) return;
    if (ambienceSoundId_ != 0) AudioEngine::instance().stopSound(ambienceSoundId_);
    ambienceSoundId_ = 0;
    ambience_ = AmbientSample{};
    ambience_.path = path;
    ambienceEntryVolume_ = clampedEntry;
    ambienceDuration_ = 0.0f;
    ambienceLoopTime_ = 0.0f;
    if (path.empty()) {
        LOG_INFO("Ambient: no area ambience");
        return;
    }
    if (!loadSound(path, ambience_, assets_)) {
        LOG_ERROR("AmbientSoundManager: ambience ", path, " could not be loaded");
        return;
    }
    const auto duration = wavDurationSeconds(ambience_.data);
    if (!duration || *duration <= 0.0f) {
        LOG_ERROR("AmbientSoundManager: ambience ", path, " has no readable WAVE header");
        ambience_.loaded = false;
        return;
    }
    ambienceDuration_ = *duration;
    LOG_INFO("Ambient: ambience ", path, " (", ambienceDuration_, " s)");
}

void AmbientSoundManager::update(float deltaTime, const glm::vec3& cameraPos, bool isIndoor, bool isSwimming) {
    if (!initialized_) return;

    updatePositionalEmitters(deltaTime, cameraPos);
    updateAmbience(deltaTime);
    updateWeatherAmbience(deltaTime, isIndoor);
    updateWaterAmbience(deltaTime, isSwimming);
    updateBellTolls(deltaTime);
}

void AmbientSoundManager::updateAmbience(float deltaTime) {
    if (!ambience_.loaded) return;
    // Restarted as each pass ends, so the file plays back to back.
    ambienceLoopTime_ -= deltaTime;
    if (ambienceLoopTime_ > 0.0f) return;
    ambienceSoundId_ = AudioEngine::instance().playSound2DStoppable(
        ambience_.data, AMBIENCE_VOLUME * ambienceEntryVolume_ * volumeScale_);
    ambienceLoopTime_ = ambienceDuration_;
}

void AmbientSoundManager::updatePositionalEmitters(float deltaTime, const glm::vec3& cameraPos) {
    // First pass: mark emitters as active/inactive based on distance
    int activeFireCount = 0;
    int activeWaterCount = 0;
    const int MAX_ACTIVE_FIRE = 5;      // Max 5 fire sounds at once
    const int MAX_ACTIVE_WATER = 3;     // Max 3 water sounds at once

    for (auto& emitter : emitters_) {
        const glm::vec3 delta = emitter.position - cameraPos;
        const float distSq = glm::dot(delta, delta);

        // Determine max distance based on type
        float maxDist = MAX_AMBIENT_DISTANCE;
        bool isFire = false;
        bool isWater = false;

        if (emitter.type == AmbientType::FIREPLACE_SMALL ||
            emitter.type == AmbientType::FIREPLACE_LARGE ||
            emitter.type == AmbientType::TORCH) {
            maxDist = MAX_FIRE_DISTANCE;
            isFire = true;
        } else if (emitter.type == AmbientType::WATER_SURFACE ||
                   emitter.type == AmbientType::RIVER ||
                   emitter.type == AmbientType::WATERFALL ||
                   emitter.type == AmbientType::FOUNTAIN) {
            maxDist = MAX_WATER_DISTANCE;
            isWater = true;
        }

        // Update active state based on distance AND limits
        const float maxDistSq = maxDist * maxDist;
        const bool withinRange = (distSq < maxDistSq);

        if (isFire && withinRange && activeFireCount < MAX_ACTIVE_FIRE) {
            emitter.active = true;
            activeFireCount++;
        } else if (isWater && withinRange && activeWaterCount < MAX_ACTIVE_WATER) {
            emitter.active = true;
            activeWaterCount++;
        } else if (!isFire && !isWater && withinRange) {
            emitter.active = true;  // Other types (fountain, etc)
        } else {
            emitter.active = false;
        }

        if (!emitter.active) continue;

        // Update play timer
        emitter.lastPlayTime += deltaTime;

        // We only need the true distance for volume attenuation once the emitter is active.
        const float distance = std::sqrt(distSq);

        // Handle different emitter types
        switch (emitter.type) {
            case AmbientType::FIREPLACE_SMALL:
                if (emitter.lastPlayTime >= FIRE_LOOP_INTERVAL && !fireSoundsSmall_.empty() && fireSoundsSmall_[0].loaded) {
                    float volume = FIRE_VOLUME * volumeScale_ * (1.0f - (distance / maxDist));
                    AudioEngine::instance().playSound3D(fireSoundsSmall_[0].data, emitter.position, volume);
                    emitter.lastPlayTime = 0.0f;
                }
                break;

            case AmbientType::FIREPLACE_LARGE:
                if (emitter.lastPlayTime >= FIRE_LOOP_INTERVAL && !fireSoundsLarge_.empty() && fireSoundsLarge_[0].loaded) {
                    float volume = FIRE_VOLUME * volumeScale_ * (1.0f - (distance / maxDist));
                    AudioEngine::instance().playSound3D(fireSoundsLarge_[0].data, emitter.position, volume);
                    emitter.lastPlayTime = 0.0f;
                }
                break;

            case AmbientType::TORCH:
                if (emitter.lastPlayTime >= FIRE_LOOP_INTERVAL && !torchSounds_.empty() && torchSounds_[0].loaded) {
                    float volume = FIRE_VOLUME * 0.7f * volumeScale_ * (1.0f - (distance / maxDist));
                    AudioEngine::instance().playSound3D(torchSounds_[0].data, emitter.position, volume);
                    emitter.lastPlayTime = 0.0f;
                }
                break;

            case AmbientType::WATER_SURFACE:
                if (emitter.lastPlayTime >= 5.0f && !waterSounds_.empty() && waterSounds_[0].loaded) {
                    float volume = WATER_VOLUME * volumeScale_ * (1.0f - (distance / maxDist));
                    AudioEngine::instance().playSound3D(waterSounds_[0].data, emitter.position, volume);
                    emitter.lastPlayTime = 0.0f;
                }
                break;

            case AmbientType::RIVER:
                if (emitter.lastPlayTime >= 5.0f && !riverSounds_.empty() && riverSounds_[0].loaded) {
                    float volume = WATER_VOLUME * volumeScale_ * (1.0f - (distance / maxDist));
                    AudioEngine::instance().playSound3D(riverSounds_[0].data, emitter.position, volume);
                    emitter.lastPlayTime = 0.0f;
                }
                break;

            case AmbientType::WATERFALL:
                if (emitter.lastPlayTime >= 4.0f && !waterfallSounds_.empty() && waterfallSounds_[0].loaded) {
                    float volume = WATER_VOLUME * 1.2f * volumeScale_ * (1.0f - (distance / maxDist));
                    AudioEngine::instance().playSound3D(waterfallSounds_[0].data, emitter.position, volume);
                    emitter.lastPlayTime = 0.0f;
                }
                break;

            case AmbientType::FOUNTAIN:
                if (emitter.lastPlayTime >= 6.0f && !fountainSounds_.empty() && fountainSounds_[0].loaded) {
                    float volume = WATER_VOLUME * 0.8f * volumeScale_ * (1.0f - (distance / maxDist));
                    AudioEngine::instance().playSound3D(fountainSounds_[0].data, emitter.position, volume);
                    emitter.lastPlayTime = 0.0f;
                }
                break;

            default:
                break;
        }
    }
}

uint64_t AmbientSoundManager::addEmitter(const glm::vec3& position, AmbientType type) {
    AmbientEmitter emitter;
    emitter.id = nextEmitterId_++;
    emitter.type = type;
    emitter.position = position;
    emitter.active = false;
    emitter.lastPlayTime = randomFloat(0.0f, 2.0f);  // Random initial offset
    emitter.loopInterval = FIRE_LOOP_INTERVAL;

    emitters_.push_back(emitter);
    return emitter.id;
}
void AmbientSoundManager::setVolumeScale(float scale) {
    const float clamped = std::max(0.0f, std::min(1.0f, scale));
    if (clamped == volumeScale_) return;
    volumeScale_ = clamped;
    // A pass runs up to ~90 s at the volume it started with; restart the
    // ambience so the slider (or sound_enableambience off) takes now.
    if (ambienceSoundId_ != 0) AudioEngine::instance().stopSound(ambienceSoundId_);
    ambienceSoundId_ = 0;
    ambienceLoopTime_ = 0.0f;
}

void AmbientSoundManager::setWeather(WeatherType type) {
    if (currentWeather_ != type) {
        LOG_INFO("AmbientSoundManager: Weather changed from ", static_cast<int>(currentWeather_),
                 " to ", static_cast<int>(type));
        currentWeather_ = type;
        weatherLoopTime_ = (type != WeatherType::NONE) ? 100.0f : 0.0f;  // Play immediately on onset
    }
}

void AmbientSoundManager::setZoneId(uint32_t zoneId) {
    // Only the bell tolls still key on this: which capital, by AreaTable id.
    CityType city = CityType::NONE;
    switch (zoneId) {
        case 1519: city = CityType::STORMWIND;    break;
        case 1537: city = CityType::IRONFORGE;    break;
        case 1657: city = CityType::DARNASSUS;    break;
        case 1637: city = CityType::ORGRIMMAR;    break;
        case 1497: city = CityType::UNDERCITY;    break;
        case 1638: city = CityType::THUNDERBLUFF; break;
        default: break;
    }
    setCityType(city);
}

void AmbientSoundManager::setCityType(CityType type) {
    if (currentCity_ != type) {
        LOG_INFO("AmbientSoundManager: City changed from ", static_cast<int>(currentCity_),
                 " to ", static_cast<int>(type));
        currentCity_ = type;
        bellTollTime_ = randomFloat(60.0f, 90.0f);  // First bell after 1-1.5 minutes
    }
}

void AmbientSoundManager::updateWeatherAmbience(float deltaTime, bool isIndoor) {
    // Don't play weather sounds when indoors
    if (isIndoor || currentWeather_ == WeatherType::NONE) return;

    weatherLoopTime_ += deltaTime;

    // Select appropriate sound library based on weather type
    const std::vector<AmbientSample>* weatherLibrary = nullptr;
    float loopInterval = 20.0f;  // Default 20 second loop for weather

    switch (currentWeather_) {
        case WeatherType::RAIN_LIGHT:
            weatherLibrary = &rainLightSounds_;
            loopInterval = 25.0f;
            break;
        case WeatherType::RAIN_MEDIUM:
            weatherLibrary = &rainMediumSounds_;
            loopInterval = 20.0f;
            break;
        case WeatherType::RAIN_HEAVY:
            weatherLibrary = &rainHeavySounds_;
            loopInterval = 18.0f;
            break;
        case WeatherType::SNOW_LIGHT:
            weatherLibrary = &snowLightSounds_;
            loopInterval = 30.0f;
            break;
        case WeatherType::SNOW_MEDIUM:
            weatherLibrary = &snowMediumSounds_;
            loopInterval = 25.0f;
            break;
        case WeatherType::SNOW_HEAVY:
            weatherLibrary = &snowHeavySounds_;
            loopInterval = 22.0f;
            break;
        case WeatherType::STORM_LIGHT:
            weatherLibrary = &stormLightSounds_;
            loopInterval = 25.0f;
            break;
        case WeatherType::STORM_MEDIUM:
            weatherLibrary = &stormMediumSounds_;
            loopInterval = 20.0f;
            break;
        case WeatherType::STORM_HEAVY:
            weatherLibrary = &stormHeavySounds_;
            loopInterval = 18.0f;
            break;
        default:
            return;
    }

    // Play weather sound if library is loaded and timer expired
    if (weatherLibrary && !weatherLibrary->empty() && (*weatherLibrary)[0].loaded) {
        if (weatherLoopTime_ >= loopInterval) {
            float volume = 0.4f * volumeScale_;  // Weather ambience at moderate volume
            AudioEngine::instance().playSound2D((*weatherLibrary)[0].data, volume, 1.0f);
            LOG_INFO("Playing weather ambience: type ", static_cast<int>(currentWeather_));
            weatherLoopTime_ = 0.0f;
        }
    }
}

void AmbientSoundManager::updateWaterAmbience(float deltaTime, bool isSwimming) {
    bool stateChanged = (wasSwimming_ != isSwimming);

    if (stateChanged) {
        LOG_INFO("Ambient: ", isSwimming ? "ENTERED WATER" : "EXITED WATER");
        oceanLoopTime_ = isSwimming ? 100.0f : 0.0f;  // Play immediately when submerging
    }

    wasSwimming_ = isSwimming;

    // Play underwater sounds when swimming
    if (isSwimming) {
        if (!underwaterSounds_.empty() && underwaterSounds_[0].loaded) {
            oceanLoopTime_ += deltaTime;
            // Play every 18 seconds for underwater ambience
            if (oceanLoopTime_ >= 18.0f) {
                float volume = 0.5f * volumeScale_;
                AudioEngine::instance().playSound2D(underwaterSounds_[0].data, volume, 1.0f);
                LOG_INFO("Playing underwater ambience");
                oceanLoopTime_ = 0.0f;
            }
        }
    }
    // Ocean ambience for standing beside water, rather than in it, is not
    // played and its sample is no longer loaded for it.
    //
    // It wants a distance to the nearest water surface, which nothing here
    // measures: isSwimming says in or out and nothing about how far away it is
    // when out. Being near the sea is already covered approximately by the
    // BEACH zone ambience above, which is why this was never missed. Whoever
    // adds the proximity query can load Sound\\Ambience\\Water\\OceanDeepDay.wav
    // here again - keeping it in memory until then bought nothing.
}

void AmbientSoundManager::updateBellTolls(float deltaTime) {
    // Only play bells when in a city
    if (currentCity_ == CityType::NONE) return;

    bellTollTime_ += deltaTime;

    // Select appropriate bell sound based on city faction
    const std::vector<AmbientSample>* bellLibrary = nullptr;

    switch (currentCity_) {
        case CityType::STORMWIND:
        case CityType::IRONFORGE:
            bellLibrary = &bellAllianceSounds_;
            break;
        case CityType::DARNASSUS:
            bellLibrary = &bellNightElfSounds_;
            break;
        case CityType::ORGRIMMAR:
        case CityType::UNDERCITY:
            bellLibrary = &bellHordeSounds_;
            break;
        case CityType::THUNDERBLUFF:
            bellLibrary = &bellTribalSounds_;
            break;
        default:
            return;
    }

    // The authored sample already contains its complete bell phrase. Play it
    // once at a sparse ambient interval instead of replaying the whole sample
    // once per clock hour count.
    const float bellInterval = randomFloat(120.0f, 180.0f);
    if (bellLibrary && !bellLibrary->empty() && (*bellLibrary)[0].loaded) {
        if (bellTollTime_ >= bellInterval) {
            // Bells have their own "Capital City Bells" slider (bellVolumeScale_)
            // and must be independent of the ambient slider (volumeScale_) - the
            // engine still applies master volume. Coupling to volumeScale_ made the
            // bell control appear dead whenever ambient was turned down.
            float volume = 0.5f * bellVolumeScale_;
            AudioEngine::instance().playSound2D((*bellLibrary)[0].data, volume, 1.0f);
            LOG_INFO("Bell toll ringing in city: type ", static_cast<int>(currentCity_));
            bellTollTime_ = 0.0f;
        }
    }
}

} // namespace audio
} // namespace wowee
