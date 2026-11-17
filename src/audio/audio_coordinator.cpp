#include "audio/audio_coordinator.hpp"
#include <cmath>
#include "audio/audio_engine.hpp"
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
void AudioCoordinator::shutdown() {
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
        }
        ambientSoundManager_->setWeather(audioWeatherType);
        // Ambience by the same precedence as music: the WMO room's
        // WMOAreaTable AmbienceID (Tavern in an inn, BlackSmith in a smithy),
        // else the area's AreaTable AmbienceID or its nearest ancestor's
        // (ForestNormal in Elwynn, ForestScary in Duskwood), else the zone's.
        // Nothing is keyed on a WMO model or zone id.
        uint32_t ambienceId = ctx.wmoAmbienceId;
        if (ambienceId == 0 && zm) ambienceId = zm->getAreaAmbienceId(ctx.areaId);
        if (ambienceId == 0 && zm) ambienceId = zm->getAreaAmbienceId(zoneId);
        // SoundAmbience names a day and a night sound; the hours it switches
        // at are not in the data, so 06:00 and 20:00 here are WoWee's.
        const float hour = std::fmod(std::fmod(ctx.gameTimeHours, 24.0f) + 24.0f, 24.0f);
        const bool night = hour < 6.0f || hour >= 20.0f;
        if (ambienceId != currentAmbienceId_ || night != ambienceNight_) {
            currentAmbienceId_ = ambienceId;
            ambienceNight_ = night;
            LOG_INFO("Ambience: SoundAmbience ", ambienceId, night ? " (night)" : " (day)",
                     " area ", ctx.areaId, " zone ", zoneId);
        }
        ambientSoundManager_->setAmbience(
            zm && ambienceId ? zm->getAmbienceSound(ambienceId, night) : std::string());
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

    // Which ZoneMusic set plays: the WMO room's when it names one (an inn's
    // Zone-TavernAlliance), else the area's (its own or its nearest
    // ancestor's), else the zone's. One rule for taverns, cities and fields;
    // nothing is keyed on a WMO model id.
    uint32_t musicSet = ctx.wmoZoneMusicId;
    if (musicSet == 0) musicSet = zm->getAreaZoneMusicId(ctx.areaId);
    if (musicSet == 0) musicSet = zm->getAreaZoneMusicId(zoneId);
    const auto pickForSet = [&]() -> std::string {
        // The zone's pool carries WoWee's own tracks as well; a WMO room's
        // set is played as authored.
        if (ctx.wmoZoneMusicId == 0 && currentZoneId_ != 0 &&
            zm->getAreaZoneMusicId(currentZoneId_) == musicSet)
            return zm->getRandomMusic(currentZoneId_);
        const auto* tracks = zm->getZoneMusicTracks(musicSet);
        return tracks ? zm->pickTrack(*tracks) : std::string();
    };

    if (musicSet != currentMusicSetId_ && musicSet != 0 && musicSwitchCooldown_ <= 0.0f) {
        currentMusicSetId_ = musicSet;
        const std::string music = pickForSet();
        if (!music.empty()) {
            playZoneMusic(music);
            musicSwitchCooldown_ = 6.0f;
        }
    }

    musicManager_->update(deltaTime);

    // When a track finishes, pick another from the same set.
    if (!musicManager_->isPlaying() && currentMusicSetId_ != 0 && musicSwitchCooldown_ <= 0.0f) {
        const std::string music = pickForSet();
        if (!music.empty()) {
            playZoneMusic(music);
            musicSwitchCooldown_ = 2.0f;
        }
    }
}

} // namespace audio
} // namespace wowee
