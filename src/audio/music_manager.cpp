#include "audio/music_manager.hpp"

#include <chrono>
#include "audio/audio_engine.hpp"
#include "audio/sound_entries.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "core/thread_pool.hpp"
#include "core/logger.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

namespace wowee {
namespace audio {

MusicManager::MusicManager() = default;

MusicManager::~MusicManager() {
    shutdown();
}

void MusicManager::loadMusicDbcTables(pipeline::AssetManager* assets) {
    zoneMusicSoundIds_.clear();
    zoneIntroSoundIds_.clear();
    if (!assets || !assets->isInitialized()) return;

    SoundEntryTable::instance().ensureLoaded(assets);

    // ZoneMusic.dbc: 0=ID, 1=SetName, 2..5=Silence intervals, 6=DayMusic, 7=NightMusic.
    if (auto zmDbc = assets->loadDBC("ZoneMusic.dbc");
        zmDbc && zmDbc->isLoaded() && zmDbc->getFieldCount() >= 8) {
        for (uint32_t row = 0; row < zmDbc->getRecordCount(); ++row) {
            const uint32_t setId = zmDbc->getUInt32(row, 0);
            if (setId == 0) continue;
            zoneMusicSoundIds_[setId] = {zmDbc->getUInt32(row, 6), zmDbc->getUInt32(row, 7)};
        }
    }

    // ZoneIntroMusicTable.dbc: 0=ID, 1=Name, 2=SoundID, 3=Priority, 4=MinDelayMinutes.
    if (auto introDbc = assets->loadDBC("ZoneIntroMusicTable.dbc");
        introDbc && introDbc->isLoaded() && introDbc->getFieldCount() >= 3) {
        for (uint32_t row = 0; row < introDbc->getRecordCount(); ++row) {
            const uint32_t introId = introDbc->getUInt32(row, 0);
            if (introId == 0) continue;
            zoneIntroSoundIds_[introId] = introDbc->getUInt32(row, 2);
        }
    }
}

bool MusicManager::initialize(pipeline::AssetManager* assets) {
    assetManager = assets;
    loadMusicDbcTables(assets);
    LOG_INFO("Music manager initialized");
    return true;
}

uint32_t MusicManager::resolveZoneMusicSoundEntryId(uint32_t zoneMusicId, bool night) const {
    const auto it = zoneMusicSoundIds_.find(zoneMusicId);
    if (it == zoneMusicSoundIds_.end()) return 0;
    if (night && it->second.second != 0) {
        return it->second.second;
    }
    return it->second.first;
}

uint32_t MusicManager::resolveZoneIntroSoundEntryId(uint32_t introId) const {
    const auto it = zoneIntroSoundIds_.find(introId);
    return (it != zoneIntroSoundIds_.end()) ? it->second : 0u;
}

float MusicManager::resolveSoundEntryVolume(uint32_t soundEntryId) const {
    if (soundEntryId == 0) return 1.0f;
    const SoundEntryRecord* rec = SoundEntryTable::instance().findById(assetManager, soundEntryId);
    if (!rec) {
        rec = SoundEntryTable::instance().findLoadedById(soundEntryId);
    }
    if (!rec || !std::isfinite(rec->volume) || rec->volume <= 0.0f) {
        return 1.0f;
    }
    return std::clamp(rec->volume, 0.0f, 1.0f);
}

float MusicManager::effectiveMusicVolumeForEntry(float entryVolume) const {
    if (!isEnabled()) return 0.0f;
    float vol = (static_cast<float>(volumePercent) / 100.0f) *
                kNativeMusicBusGain *
                std::clamp(entryVolume, 0.0f, 1.0f);
    if (underwaterMode) {
        vol *= 0.3f;
    }
    return std::clamp(vol, 0.0f, 1.0f);
}

float MusicManager::effectiveMusicVolume() const {
    return effectiveMusicVolumeForEntry(currentEntryVolume_);
}

void MusicManager::shutdown() {
    cancelPendingFileLoad();
    AudioEngine::instance().stopMusic();
    playing = false;
    fadingIn = false;
    fadingOut = false;
    crossfading = false;
    fadeInTimer = 0.0f;
    fadeInDuration = 0.0f;
    fadeInTargetVolume = 0.0f;
    appliedVolume_ = 0.0f;
    currentEntryVolume_ = 1.0f;
    currentTrack.clear();
    currentTrackIsFile = false;
    pendingTrack.clear();
    pendingIsFile = false;
    musicDataCache_.clear();
    zoneMusicSoundIds_.clear();
    zoneIntroSoundIds_.clear();
    assetManager = nullptr;
}

void MusicManager::preloadMusic(const std::string& mpqPath) {
    if (!assetManager || mpqPath.empty() || !isEnabled()) return;
    if (musicDataCache_.find(mpqPath) != musicDataCache_.end()) return;

    auto data = assetManager->readFile(mpqPath);
    if (!data.empty()) {
        musicDataCache_[mpqPath] =
            std::make_shared<const std::vector<uint8_t>>(std::move(data));
    }
}

void MusicManager::playMusic(const std::string& mpqPath, bool loop, float fadeInMs,
                             uint32_t soundEntryId) {
    if (!assetManager || mpqPath.empty() || !isEnabled()) return;
    if (mpqPath == currentTrack && playing && !fadingOut) return;

    if (!AudioEngine::instance().isInitialized()) {
        LOG_WARNING("Music: AudioEngine not initialized");
        return;
    }

    cancelPendingFileLoad();
    pendingTrack.clear();
    pendingIsFile = false;

    auto cacheIt = musicDataCache_.find(mpqPath);
    if (cacheIt == musicDataCache_.end()) {
        preloadMusic(mpqPath);
        cacheIt = musicDataCache_.find(mpqPath);
    }
    if (cacheIt == musicDataCache_.end() || !cacheIt->second || cacheIt->second->empty()) {
        LOG_WARNING("Music: Could not read: ", mpqPath);
        return;
    }

    // Direct Lua PlayMusic(path) passes soundEntryId == 0 -> entryVolume = 1.0f
    // (0.6081 * slider, verified on Native TBC 2.4.3). ZoneMusic / Intro / SMSG_PLAY_MUSIC
    // pass the authoritative SoundEntries.dbc ID -> rec->volume * 0.6081 * slider.
    const float nextEntryVol = resolveSoundEntryVolume(soundEntryId);
    const float targetVolume = effectiveMusicVolumeForEntry(nextEntryVol);
    if (targetVolume <= 0.0f) return;

    const float startVolume = (fadeInMs > 0.0f) ? 0.0f : targetVolume;
    if (AudioEngine::instance().playMusic(cacheIt->second, startVolume, loop, mpqPath,
                                          /*keepOutgoingForCrossfade=*/false, soundEntryId)) {
        currentEntryVolume_ = nextEntryVol;
        playing = true;
        crossfading = false;
        fadingOut = false;
        fadingIn = false;
        appliedVolume_ = startVolume;
        if (fadeInMs > 0.0f) {
            fadingIn = true;
            fadeInTimer = 0.0f;
            fadeInDuration = std::max(0.05f, fadeInMs / 1000.0f);
            fadeInTargetVolume = targetVolume;
            AudioEngine::instance().setMusicVolume(0.0f);
        }
        currentTrack = mpqPath;
        currentTrackIsFile = false;
        LOG_INFO("Music: Playing ", mpqPath, " (entryVol=", currentEntryVolume_,
                 ", targetVol=", targetVolume, ")");
    } else {
        LOG_ERROR("Music: Failed to play music via AudioEngine");
    }
}

void MusicManager::playFilePath(const std::string& filePath, bool loop, float fadeInMs) {
    if (filePath.empty() || !isEnabled()) return;
    if (filePath == currentTrack && playing && !fadingOut) return;
    if (!std::filesystem::exists(filePath)) {
        LOG_WARNING("Music: file not found: ", filePath);
        return;
    }

    if (!AudioEngine::instance().isInitialized()) {
        LOG_WARNING("Music: AudioEngine not initialized");
        return;
    }

    if (pendingFileLoad_ && pendingFileLoad_->path == filePath) {
        pendingFileLoad_->loop = loop;
        pendingFileLoad_->fadeInMs = fadeInMs;
        pendingFileLoad_->crossfadeOutMs = 0.0f;
        return;
    }

    pendingFileLoad_ = PendingFileLoad{
        core::ThreadPool::ioWorkers().submit([filePath]()
                -> std::shared_ptr<const std::vector<uint8_t>> {
            std::ifstream file(filePath, std::ios::binary | std::ios::ate);
            if (!file) return nullptr;
            std::streamsize size = file.tellg();
            if (size <= 0) return nullptr;
            file.seekg(0, std::ios::beg);
            std::vector<uint8_t> bytes(static_cast<size_t>(size));
            if (!file.read(reinterpret_cast<char*>(bytes.data()), size)) return nullptr;
            return std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
        }),
        filePath, loop, fadeInMs, 0.0f
    };
}

void MusicManager::cancelPendingFileLoad() {
    pendingFileLoad_.reset();
}

void MusicManager::pollPendingFileLoad() {
    if (!pendingFileLoad_) return;
    if (!isEnabled()) {
        cancelPendingFileLoad();
        return;
    }
    auto& pending = *pendingFileLoad_;
    if (!pending.future.valid()) { pendingFileLoad_.reset(); return; }
    if (pending.future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;

    auto data = pending.future.get();
    const std::string filePath = pending.path;
    const bool loop = pending.loop;
    const float fadeInMs = pending.fadeInMs;
    const float crossfadeOutMs = pending.crossfadeOutMs;
    pendingFileLoad_.reset();
    pendingTrack.clear();
    pendingIsFile = false;

    if (!data || data->empty()) {
        LOG_ERROR("Music: Could not read file: ", filePath);
        return;
    }

    const float nextEntryVol = 1.0f;
    const float targetVolume = effectiveMusicVolumeForEntry(nextEntryVol);
    if (targetVolume <= 0.0f) return;

    const bool doOverlap = (crossfadeOutMs > 0.0f && playing && appliedVolume_ > 0.0f);
    const float outgoingStartVol = appliedVolume_;
    const float effectiveFadeInMs = doOverlap ? crossfadeOutMs : fadeInMs;
    const float startVolume = (effectiveFadeInMs > 0.0f) ? 0.0f : targetVolume;

    if (AudioEngine::instance().playMusic(std::move(data), startVolume, loop, filePath,
                                          /*keepOutgoingForCrossfade=*/doOverlap, 0u)) {
        currentEntryVolume_ = nextEntryVol;
        playing = true;
        fadingOut = false;
        fadingIn = false;
        appliedVolume_ = startVolume;
        if (doOverlap) {
            crossfading = true;
            fadeTimer = 0.0f;
            fadeDuration = std::max(0.05f, crossfadeOutMs / 1000.0f);
            crossfadeStartVolume_ = outgoingStartVol;
        } else {
            crossfading = false;
        }
        if (effectiveFadeInMs > 0.0f) {
            fadingIn = true;
            fadeInTimer = 0.0f;
            fadeInDuration = std::max(0.05f, effectiveFadeInMs / 1000.0f);
            fadeInTargetVolume = targetVolume;
            AudioEngine::instance().setMusicVolume(0.0f);
        }
        currentTrack = filePath;
        currentTrackIsFile = true;
        LOG_INFO("Music: Playing file ", filePath);
    } else {
        LOG_ERROR("Music: Failed to play music via AudioEngine");
    }
}

void MusicManager::stopMusic(float fadeMs) {
    cancelPendingFileLoad();
    fadingIn = false;
    pendingTrack.clear();
    pendingIsFile = false;
    if (!playing && !crossfading) return;

    if (fadeMs > 0.0f && playing && appliedVolume_ > 0.0f) {
        crossfading = false;
        AudioEngine::instance().stopCrossfadeOut();
        fadingOut = true;
        fadeOutTimer = 0.0f;
        fadeOutDuration = fadeMs / 1000.0f;
        fadeOutStartVolume = appliedVolume_;
    } else {
        AudioEngine::instance().stopMusic();
        playing = false;
        crossfading = false;
        fadingOut = false;
        appliedVolume_ = 0.0f;
        currentTrack.clear();
        currentTrackIsFile = false;
    }
}

void MusicManager::setEnabled(bool enabled) {
    if (enabled_ == enabled) return;
    enabled_ = enabled;
    if (!isEnabled()) {
        stopMusic(0.0f);
        return;
    }
    const float vol = effectiveMusicVolume();
    appliedVolume_ = vol;
    AudioEngine::instance().setMusicVolume(vol);
}

void MusicManager::setVolume(int volume) {
    volume = std::clamp(volume, 0, 100);
    if (volumePercent == volume) return;
    const float prevTarget = effectiveMusicVolume();
    volumePercent = volume;

    if (!isEnabled()) {
        // Native TBC 2.4.3 stops music decoding immediately when Sound_EnableMusic=0 or MusicVolume=0.
        stopMusic(0.0f);
        return;
    }

    const float vol = effectiveMusicVolume();
    if (crossfading && prevTarget > 1e-6f) {
        crossfadeStartVolume_ *= (vol / prevTarget);
        const float outT = std::clamp(1.0f - fadeTimer / std::max(fadeDuration, 0.001f), 0.0f, 1.0f);
        AudioEngine::instance().setCrossfadeOutVolume(crossfadeStartVolume_ * outT);
    }
    if (fadingIn) {
        fadeInTargetVolume = vol;
        const float t = std::clamp(fadeInTimer / std::max(fadeInDuration, 0.001f), 0.0f, 1.0f);
        appliedVolume_ = fadeInTargetVolume * t;
        AudioEngine::instance().setMusicVolume(appliedVolume_);
    } else if (!fadingOut) {
        appliedVolume_ = vol;
        AudioEngine::instance().setMusicVolume(vol);
    }
}

void MusicManager::setUnderwaterMode(bool underwater) {
    if (underwaterMode == underwater) return;
    const float prevTarget = effectiveMusicVolume();
    underwaterMode = underwater;
    if (!isEnabled()) return;

    const float vol = effectiveMusicVolume();
    if (crossfading && prevTarget > 1e-6f) {
        crossfadeStartVolume_ *= (vol / prevTarget);
        const float outT = std::clamp(1.0f - fadeTimer / std::max(fadeDuration, 0.001f), 0.0f, 1.0f);
        AudioEngine::instance().setCrossfadeOutVolume(crossfadeStartVolume_ * outT);
    }
    if (fadingIn) {
        fadeInTargetVolume = vol;
        const float t = std::clamp(fadeInTimer / std::max(fadeInDuration, 0.001f), 0.0f, 1.0f);
        appliedVolume_ = fadeInTargetVolume * t;
        AudioEngine::instance().setMusicVolume(appliedVolume_);
    } else if (!fadingOut) {
        appliedVolume_ = vol;
        AudioEngine::instance().setMusicVolume(vol);
    }
}

void MusicManager::crossfadeTo(const std::string& mpqPath, float fadeMs, uint32_t soundEntryId) {
    if (!assetManager || mpqPath.empty() || !isEnabled()) return;
    if (mpqPath == currentTrack && playing && !fadingOut) return;

    if (!AudioEngine::instance().isInitialized()) {
        LOG_WARNING("Music: AudioEngine not initialized");
        return;
    }

    uint32_t resolvedId = soundEntryId;
    if (resolvedId == 0) {
        if (const SoundEntryRecord* rec = SoundEntryTable::instance().findByPath(assetManager, mpqPath)) {
            resolvedId = rec->id;
        }
    }

    // If nothing is audibly playing yet, start immediately (with fade-in if requested when entering world).
    if (fadeMs <= 0.0f || !playing || appliedVolume_ <= 0.0f) {
        fadingOut = false;
        playMusic(mpqPath, loopTracks_, /*fadeInMs=*/(fadeMs > 0.0f ? fadeMs : 0.0f), resolvedId);
        return;
    }

    cancelPendingFileLoad();
    pendingTrack.clear();
    pendingIsFile = false;

    auto cacheIt = musicDataCache_.find(mpqPath);
    if (cacheIt == musicDataCache_.end()) {
        preloadMusic(mpqPath);
        cacheIt = musicDataCache_.find(mpqPath);
    }
    if (cacheIt == musicDataCache_.end() || !cacheIt->second || cacheIt->second->empty()) {
        LOG_WARNING("Music: Could not read for crossfade: ", mpqPath);
        return;
    }

    const float nextEntryVol = resolveSoundEntryVolume(resolvedId);
    const float targetVolume = effectiveMusicVolumeForEntry(nextEntryVol);
    if (targetVolume <= 0.0f) return;

    const float outgoingStartVol = appliedVolume_;
    const float fadeSec = std::max(0.05f, fadeMs / 1000.0f);

    // Simultaneous dual-decoder linear crossfade (Native TBC 2.4.3 parity):
    // outgoing stream ramps outgoingStartVol -> 0 over fadeSec while incoming
    // stream simultaneously ramps 0 -> targetVolume over fadeSec.
    if (AudioEngine::instance().playMusic(cacheIt->second, /*volume=*/0.0f, loopTracks_, mpqPath,
                                          /*keepOutgoingForCrossfade=*/true, resolvedId)) {
        currentEntryVolume_ = nextEntryVol;
        playing = true;
        fadingOut = false;
        crossfading = AudioEngine::instance().isCrossfadeOutPlaying();
        fadeTimer = 0.0f;
        fadeDuration = fadeSec;
        crossfadeStartVolume_ = outgoingStartVol;
        AudioEngine::instance().setCrossfadeOutVolume(outgoingStartVol);

        fadingIn = true;
        fadeInTimer = 0.0f;
        fadeInDuration = fadeSec;
        fadeInTargetVolume = targetVolume;
        appliedVolume_ = 0.0f;
        AudioEngine::instance().setMusicVolume(0.0f);

        currentTrack = mpqPath;
        currentTrackIsFile = false;
        LOG_INFO("Music: Crossfading to ", mpqPath, " over ", fadeSec,
                 "s (entryVol=", currentEntryVolume_, ", targetVol=", targetVolume, ")");
    } else {
        LOG_ERROR("Music: Failed to start crossfade track via AudioEngine");
    }
}

void MusicManager::crossfadeToFile(const std::string& filePath, float fadeMs) {
    if (filePath.empty() || !isEnabled()) return;
    if (filePath == currentTrack && playing && !fadingOut) return;
    if (pendingFileLoad_ && pendingFileLoad_->path == filePath) return;

    if (!std::filesystem::exists(filePath)) {
        LOG_WARNING("Music: file not found: ", filePath);
        return;
    }
    if (!AudioEngine::instance().isInitialized()) {
        LOG_WARNING("Music: AudioEngine not initialized");
        return;
    }

    if (fadeMs <= 0.0f || !playing || appliedVolume_ <= 0.0f) {
        fadingOut = false;
        playFilePath(filePath, loopTracks_, fadeMs > 0.0f ? fadeMs : 0.0f);
        return;
    }

    pendingTrack = filePath;
    pendingIsFile = true;
    pendingFileLoad_ = PendingFileLoad{
        core::ThreadPool::ioWorkers().submit([filePath]()
                -> std::shared_ptr<const std::vector<uint8_t>> {
            std::ifstream file(filePath, std::ios::binary | std::ios::ate);
            if (!file) return nullptr;
            std::streamsize size = file.tellg();
            if (size <= 0) return nullptr;
            file.seekg(0, std::ios::beg);
            std::vector<uint8_t> bytes(static_cast<size_t>(size));
            if (!file.read(reinterpret_cast<char*>(bytes.data()), size)) return nullptr;
            return std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
        }),
        filePath, loopTracks_, fadeMs, fadeMs
    };
}

void MusicManager::update(float deltaTime) {
    const float dt = std::max(0.0f, deltaTime);

    // Hand off any background-loaded track before touching playback state.
    pollPendingFileLoad();

    if (!isEnabled()) {
        if (playing || crossfading) {
            stopMusic(0.0f);
        }
        return;
    }

    // Check if primary music stream finished naturally
    if (playing && !AudioEngine::instance().isMusicPlaying()) {
        playing = false;
        fadingIn = false;
    }

    // Advance simultaneous outgoing crossfade decoder
    if (crossfading) {
        fadeTimer += dt;
        if (fadeTimer >= fadeDuration || !AudioEngine::instance().isCrossfadeOutPlaying()) {
            crossfading = false;
            AudioEngine::instance().stopCrossfadeOut();
        } else {
            const float outT = std::clamp(1.0f - fadeTimer / std::max(fadeDuration, 0.001f), 0.0f, 1.0f);
            AudioEngine::instance().setCrossfadeOutVolume(crossfadeStartVolume_ * outT);
        }
    }

    if (fadingOut) {
        fadeOutTimer += dt;
        const float t = std::clamp(1.0f - fadeOutTimer / std::max(fadeOutDuration, 0.001f), 0.0f, 1.0f);
        appliedVolume_ = fadeOutStartVolume * t;
        AudioEngine::instance().setMusicVolume(appliedVolume_);
        if (t <= 0.0f) {
            fadingOut = false;
            AudioEngine::instance().stopMusic();
            appliedVolume_ = 0.0f;
            playing = false;
            currentTrack.clear();
            currentTrackIsFile = false;
        }
        return;
    }

    if (fadingIn) {
        fadeInTimer += dt;
        const float t = std::clamp(fadeInTimer / std::max(fadeInDuration, 0.001f), 0.0f, 1.0f);
        appliedVolume_ = fadeInTargetVolume * t;
        AudioEngine::instance().setMusicVolume(appliedVolume_);
        if (t >= 1.0f) {
            fadingIn = false;
        }
    }
}

} // namespace audio
} // namespace wowee
