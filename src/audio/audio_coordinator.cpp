#include "audio/audio_coordinator.hpp"
#include <cmath>
#include "audio/audio_engine.hpp"
#include "audio/sound_entries.hpp"
#include "audio/music_manager.hpp"
#include "audio/footstep_manager.hpp"
#include "audio/activity_sound_manager.hpp"
#include "audio/mount_sound_manager.hpp"
#include "audio/npc_voice_manager.hpp"
#include "audio/player_voice_manager.hpp"
#include "audio/ambient_sound_manager.hpp"
#include "audio/ui_sound_manager.hpp"
#include "audio/combat_sound_manager.hpp"
#include "audio/spell_sound_manager.hpp"
#include "audio/movement_sound_manager.hpp"
#include "pipeline/asset_manager.hpp"
#include "game/zone_manager.hpp"
#include "core/logger.hpp"

namespace wowee {
namespace audio {

AudioCoordinator::AudioCoordinator() = default;

AudioCoordinator::~AudioCoordinator() {
    shutdown();
}

bool AudioCoordinator::initialize() {
    // Initialize AudioEngine (singleton)
    if (!AudioEngine::instance().initialize()) {
        LOG_WARNING("Failed to initialize AudioEngine - audio will be disabled");
        audioAvailable_ = false;
        return false;
    }
    audioAvailable_ = true;

    // Create all audio managers (initialized later with asset manager)
    musicManager_ = std::make_unique<MusicManager>();
    footstepManager_ = std::make_unique<FootstepManager>();
    activitySoundManager_ = std::make_unique<ActivitySoundManager>();
    mountSoundManager_ = std::make_unique<MountSoundManager>();
    npcVoiceManager_ = std::make_unique<NpcVoiceManager>();
    playerVoiceManager_ = std::make_unique<PlayerVoiceManager>();
    ambientSoundManager_ = std::make_unique<AmbientSoundManager>();
    uiSoundManager_ = std::make_unique<UiSoundManager>();
    combatSoundManager_ = std::make_unique<CombatSoundManager>();
    spellSoundManager_ = std::make_unique<SpellSoundManager>();
    movementSoundManager_ = std::make_unique<MovementSoundManager>();

    LOG_INFO("AudioCoordinator initialized with ", 11, " audio managers");
    return true;
}

void AudioCoordinator::initializeEarlyAssets(pipeline::AssetManager* assets) {
    if (!assets || !audioAvailable_) return;
    AudioEngine::instance().setAssetManager(assets);
    SoundEntryTable::instance().ensureLoaded(assets);
    if (musicManager_ && !musicManager_->isInitialized()) {
        musicManager_->initialize(assets);
    }
    if (uiSoundManager_ && !uiSoundManager_->isInitialized()) {
        uiSoundManager_->initialize(assets);
    }
    if (playerVoiceManager_) {
        playerVoiceManager_->initialize(assets);
    }
}

void AudioCoordinator::shutdown() {
    SoundEntryTable::instance().reset();
    // Reset all managers first (they may reference AudioEngine)
    movementSoundManager_.reset();
    spellSoundManager_.reset();
    combatSoundManager_.reset();
    uiSoundManager_.reset();
    ambientSoundManager_.reset();
    playerVoiceManager_.reset();
    npcVoiceManager_.reset();
    mountSoundManager_.reset();
    activitySoundManager_.reset();
    footstepManager_.reset();
    musicManager_.reset();

    // Shutdown audio engine last
    if (audioAvailable_) {
        AudioEngine::instance().shutdown();
        audioAvailable_ = false;
    }

    LOG_INFO("AudioCoordinator shutdown complete");
}

void AudioCoordinator::playZoneMusic(const std::string& music) {
    if (music.empty() || !musicManager_) return;
    if (music.rfind("file:", 0) == 0) {
        musicManager_->crossfadeToFile(music.substr(5));
    } else {
        musicManager_->crossfadeTo(music);
    }
}

void AudioCoordinator::setZoneMusicLooping(bool loop) {
    if (musicManager_) musicManager_->setLooping(loop);
}

void AudioCoordinator::onOriginalSoundtrackDisabled(game::ZoneManager* zm) {
    if (!zm || !musicManager_) return;
    if (!musicManager_->isCurrentTrackFile()) return;
    if (!musicManager_->isPlaying() && !musicManager_->isLoading()) return;
    // Only act in-world with a known zone; the login screen intentionally
    // plays a file track and is not part of the zone rotation this setting
    // controls.
    if (currentZoneId_ == 0) return;

    std::string music = zm->getRandomMusic(currentZoneId_);
    if (!music.empty() && music.rfind("file:", 0) != 0) {
        playZoneMusic(music);
        musicSwitchCooldown_ = 6.0f;
    } else {
        musicManager_->stopMusic();
    }
}

void AudioCoordinator::updateZoneAudio(const ZoneAudioContext& ctx) {
    float deltaTime = ctx.deltaTime;
    audioClockSeconds_ += std::max(0.0f, deltaTime);
    if (musicSwitchCooldown_ > 0.0f) {
        musicSwitchCooldown_ = std::max(0.0f, musicSwitchCooldown_ - deltaTime);
    }

    // Resolve the spatial zone before updating ambience. Zone ambience used to
    // run first, leaving it one zone behind and permanently on its noon default.
    auto* zm = ctx.zoneManager;
    const uint32_t tileZoneId = ctx.hasTile ? ctx.tileZoneId : 0;
    const uint32_t serverZoneId = (zm && ctx.serverZoneId != 0)
        ? zm->resolveAreaZoneId(ctx.serverZoneId)
        : ctx.serverZoneId;
    uint32_t zoneId = serverZoneId != 0 ? serverZoneId : tileZoneId;
    const float hour = std::fmod(std::fmod(ctx.gameTimeHours, 24.0f) + 24.0f, 24.0f);
    const bool night = hour < 6.0f || hour >= 20.0f;

    // ── Ambient weather audio sync ──
    if (ambientSoundManager_) {
        if (zoneId != 0) {
            ambientSoundManager_->setZoneId(zoneId);
        }
        // Map visual weather type to ambient sound weather type
        AmbientSoundManager::WeatherType audioWeatherType = AmbientSoundManager::WeatherType::NONE;
        if (ctx.weatherType == 1) { // RAIN
            if (ctx.weatherIntensity < 0.33f)      audioWeatherType = AmbientSoundManager::WeatherType::RAIN_LIGHT;
            else if (ctx.weatherIntensity < 0.66f)  audioWeatherType = AmbientSoundManager::WeatherType::RAIN_MEDIUM;
            else                                     audioWeatherType = AmbientSoundManager::WeatherType::RAIN_HEAVY;
        } else if (ctx.weatherType == 2) { // SNOW
            if (ctx.weatherIntensity < 0.33f)      audioWeatherType = AmbientSoundManager::WeatherType::SNOW_LIGHT;
            else if (ctx.weatherIntensity < 0.66f)  audioWeatherType = AmbientSoundManager::WeatherType::SNOW_MEDIUM;
            else                                     audioWeatherType = AmbientSoundManager::WeatherType::SNOW_HEAVY;
        } else if (ctx.weatherType == 3) { // STORM / SANDSTORM
            if (ctx.weatherIntensity < 0.33f)      audioWeatherType = AmbientSoundManager::WeatherType::STORM_LIGHT;
            else if (ctx.weatherIntensity < 0.66f)  audioWeatherType = AmbientSoundManager::WeatherType::STORM_MEDIUM;
            else                                     audioWeatherType = AmbientSoundManager::WeatherType::STORM_HEAVY;
        }
        ambientSoundManager_->setWeather(audioWeatherType);
        uint32_t ambienceId = ctx.wmoAmbienceId;
        if (ambienceId == 0 && zm) ambienceId = zm->getAreaAmbienceId(ctx.areaId);
        if (ambienceId == 0 && zm) ambienceId = zm->getAreaAmbienceId(zoneId);
        if (ambienceId != currentAmbienceId_ || night != ambienceNight_) {
            currentAmbienceId_ = ambienceId;
            ambienceNight_ = night;
            LOG_INFO("Ambience: SoundAmbience ", ambienceId, night ? " (night)" : " (day)",
                     " area ", ctx.areaId, " zone ", zoneId);
        }
        ambientSoundManager_->setAmbience(
            zm && ambienceId ? zm->getAmbienceSound(ambienceId, night) : std::string(),
            zm && ambienceId ? zm->getAmbienceVolume(ambienceId, night) : 1.0f);
        ambientSoundManager_->update(deltaTime, ctx.cameraPosition, ctx.insideWmo, ctx.isSwimming);
    }

    // ── Music ──
    if (!zm || !musicManager_ || !ctx.hasTile) return;

    if (zoneId != 0 && zoneId != currentZoneId_) {
        currentZoneId_ = zoneId;
        if (const auto* info = zm->getZoneInfo(zoneId)) {
            currentZoneName_ = info->name;
            LOG_INFO("Entered zone: ", info->name);
        }
    }

    // ZoneIntroMusicTable: play area/WMO intro fanfare (Stormwind Valley of
    // Heroes, Orgrimmar Intro, Undercity Intro, Silvermoon Intro) once per
    // MinDelayMinutes cooldown before settling into regular ZoneMusic.
    uint32_t introId = ctx.wmoIntroSoundId;
    if (introId == 0) introId = zm->getAreaIntroMusicId(ctx.areaId);
    if (introId == 0) introId = zm->getAreaIntroMusicId(zoneId);
    if (introId != currentIntroId_) {
        currentIntroId_ = introId;
        if (introPlaying_) {
            introPlaying_ = false;
            musicSwitchCooldown_ = 0.0f;
        }
        if (introId != 0) {
            const auto* intro = zm->getZoneIntroMusic(introId);
            const auto readyIt = introReadyAt_.find(introId);
            if (intro && !intro->tracks.empty() &&
                (readyIt == introReadyAt_.end() || audioClockSeconds_ >= readyIt->second)) {
                const std::string track = zm->pickTrack(intro->tracks);
                if (!track.empty()) {
                    playZoneMusic(track);
                    introReadyAt_[introId] = audioClockSeconds_ + intro->minDelaySeconds;
                    introPlaying_ = true;
                    musicSwitchCooldown_ = 2.0f;
                    LOG_INFO("Zone intro music: id=", introId, " track=", track);
                }
            }
        }
    }

    uint32_t musicSet = ctx.wmoZoneMusicId;
    if (musicSet == 0) musicSet = zm->getAreaZoneMusicId(ctx.areaId);
    if (musicSet == 0) musicSet = zm->getAreaZoneMusicId(zoneId);
    const auto pickForSet = [&]() -> std::string {
        const bool includeWowee = (ctx.wmoZoneMusicId == 0 && currentZoneId_ != 0 &&
                                   zm->getAreaZoneMusicId(currentZoneId_) == musicSet);
        return zm->pickZoneMusicTrack(currentZoneId_, musicSet, night, includeWowee);
    };

    const bool setChanged = (musicSet != currentMusicSetId_);
    const bool dayNightChanged = (night != currentMusicNight_);
    if ((setChanged || dayNightChanged) && musicSet != 0 && !introPlaying_ && musicSwitchCooldown_ <= 0.0f) {
        currentMusicSetId_ = musicSet;
        currentMusicNight_ = night;
        const std::string music = pickForSet();
        if (!music.empty()) {
            playZoneMusic(music);
            musicSwitchCooldown_ = 4.0f;
        }
    } else if (setChanged) {
        currentMusicSetId_ = musicSet;
        currentMusicNight_ = night;
    }

    musicManager_->update(deltaTime);

    // When a track finishes, clear any intro state and either rotate
    // immediately (Sound_ZoneMusicNoDelay / Loop Music) or wait the ZoneMusic
    // silence interval [SilenceMin, SilenceMax].
    if (!musicManager_->isPlaying() && !musicManager_->isLoading() && currentMusicSetId_ != 0) {
        if (introPlaying_) {
            introPlaying_ = false;
            musicSwitchCooldown_ = 1.0f;
        }
        if (musicSwitchCooldown_ <= 0.0f) {
            const std::string music = pickForSet();
            if (!music.empty()) {
                playZoneMusic(music);
                if (musicManager_->isLooping()) {
                    musicSwitchCooldown_ = 0.5f;
                } else {
                    auto [minSil, maxSil] = zm->getZoneSilenceInterval(currentMusicSetId_, night);
                    if (maxSil > 0.0f) {
                        std::uniform_real_distribution<float> dist(minSil, std::max(minSil, maxSil));
                        musicSwitchCooldown_ = dist(rng_);
                    } else {
                        musicSwitchCooldown_ = 2.0f;
                    }
                }
            }
        }
    }
}

} // namespace audio
} // namespace wowee
