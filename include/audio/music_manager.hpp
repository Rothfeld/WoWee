#pragma once

#include <string>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wowee {
namespace pipeline { class AssetManager; }

namespace audio {

class MusicManager {
public:
    /// Native TBC 2.4.3 music bus master scalar (-4.32 dB, verified corr=1.0000
    /// across PlayMusic, ZoneMusic, and Sound_MusicVolume=0.5/1.0 ALSA captures).
    static constexpr float kNativeMusicBusGain = 0.6081f;
    static constexpr float kDefaultCrossfadeMs = 4000.0f;

    MusicManager();
    ~MusicManager();

    bool initialize(pipeline::AssetManager* assets);
    void shutdown();

    void playMusic(const std::string& mpqPath, bool loop = true, float fadeInMs = 0.0f,
                   uint32_t soundEntryId = 0);
    void playFilePath(const std::string& filePath, bool loop = true, float fadeInMs = 0.0f);
    void stopMusic(float fadeMs = 2000.0f);
    /// Loop Music, from the audio options. Takes effect on the next track.
    void setLooping(bool loop) { loopTracks_ = loop; }
    [[nodiscard]] bool isLooping() const { return loopTracks_; }

    void crossfadeTo(const std::string& mpqPath, float fadeMs = kDefaultCrossfadeMs,
                     uint32_t soundEntryId = 0);
    void crossfadeToFile(const std::string& filePath, float fadeMs = kDefaultCrossfadeMs);
    void update(float deltaTime);
    void setVolume(int volume);
    [[nodiscard]] int getVolume() const { return volumePercent; }
    void setEnabled(bool enabled);
    [[nodiscard]] bool isEnabled() const { return enabled_ && volumePercent > 0; }
    void setUnderwaterMode(bool underwater);
    void preloadMusic(const std::string& mpqPath);

    /// Resolve ZoneMusic.dbc / ZoneIntroMusicTable.dbc -> SoundEntries.dbc ID & col 24 volume.
    [[nodiscard]] uint32_t resolveZoneMusicSoundEntryId(uint32_t zoneMusicId, bool night) const;
    [[nodiscard]] uint32_t resolveZoneIntroSoundEntryId(uint32_t introId) const;
    [[nodiscard]] float resolveSoundEntryVolume(uint32_t soundEntryId) const;
    [[nodiscard]] float currentEntryVolume() const { return currentEntryVolume_; }

    [[nodiscard]] bool isPlaying() const { return playing || crossfading; }
    [[nodiscard]] bool isCrossfading() const { return crossfading; }
    /// True while a track is being read on the worker but has not started yet.
    /// Callers must treat this as "in progress", not as failure.
    [[nodiscard]] bool isLoading() const { return pendingFileLoad_.has_value(); }
    [[nodiscard]] bool isInitialized() const { return assetManager != nullptr; }
    [[nodiscard]] const std::string& getCurrentTrack() const { return currentTrack; }
    /// Track currently playing or queued for crossfade/async load.
    [[nodiscard]] const std::string& getActiveOrPendingTrack() const {
        if (!pendingTrack.empty()) return pendingTrack;
        if (pendingFileLoad_) return pendingFileLoad_->path;
        return currentTrack;
    }
    /// True when the current track came from the filesystem (WoWee original
    /// music) rather than a game archive.
    [[nodiscard]] bool isCurrentTrackFile() const { return currentTrackIsFile; }

private:
    [[nodiscard]] float effectiveMusicVolume() const;
    [[nodiscard]] float effectiveMusicVolumeForEntry(float entryVolume) const;
    void loadMusicDbcTables(pipeline::AssetManager* assets);

    // Tracks run to several MB, so reading one on the calling thread stalls whatever
    // frame asked for it - starting the login music was costing ~200ms of render time.
    // The read happens on a worker; update() hands the bytes to the AudioEngine once
    // they land, keeping every miniaudio call on the main thread.
    struct PendingFileLoad {
        std::future<std::shared_ptr<const std::vector<uint8_t>>> future;
        std::string path;
        bool loop = true;
        float fadeInMs = 0.0f;
        float crossfadeOutMs = 0.0f;
    };
    std::optional<PendingFileLoad> pendingFileLoad_;
    void cancelPendingFileLoad();
    void pollPendingFileLoad();

    pipeline::AssetManager* assetManager = nullptr;
    std::string currentTrack;
    bool currentTrackIsFile = false;
    bool playing = false;
    bool enabled_ = true;
    int volumePercent = 30;
    float currentEntryVolume_ = 1.0f;
    bool underwaterMode = false;

    // Crossfade state
    /// Loop Music: whether a zone track runs on instead of stopping at its
    /// end. Every place a track is started reads this, including the one that
    /// starts the pending track after a crossfade - a flag applied at three of
    /// four sites is worse than none, because the exception is the one nobody
    /// remembers.
    bool loopTracks_ = false;
    bool crossfading = false;
    std::string pendingTrack;
    bool pendingIsFile = false;
    float fadeTimer = 0.0f;
    float fadeDuration = 0.0f;
    float crossfadeStartVolume_ = 0.0f;
    float appliedVolume_ = 0.0f;
    bool fadingIn = false;
    float fadeInTimer = 0.0f;
    float fadeInDuration = 0.0f;
    float fadeInTargetVolume = 0.0f;
    // Fade-out state (for stopMusic with fadeMs > 0)
    bool fadingOut = false;
    float fadeOutTimer = 0.0f;
    float fadeOutDuration = 0.0f;
    float fadeOutStartVolume = 0.0f;

    std::unordered_map<std::string, std::shared_ptr<const std::vector<uint8_t>>> musicDataCache_;
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> zoneMusicSoundIds_;
    std::unordered_map<uint32_t, uint32_t> zoneIntroSoundIds_;
};

} // namespace audio
} // namespace wowee
