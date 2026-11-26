#pragma once

#include <glm/glm.hpp>
#include <atomic>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <cstdint>

// Forward declare miniaudio types to avoid exposing implementation in header
struct ma_engine;
struct ma_sound;
struct ma_device;

namespace wowee {
namespace pipeline { class AssetManager; }
namespace audio {

struct SoundProviderPrefRecord;
class EnvironmentReverbProcessor;

/// SESound 18-slot concurrency categories (Wow.exe 8606 table at 0x896dc0 / caps at 0x897018).
namespace SESoundCategory {
    inline constexpr uint32_t kSfx             = 0;
    inline constexpr uint32_t kMusic           = 1;
    inline constexpr uint32_t kAmbience        = 2;
    inline constexpr uint32_t kCinematic       = 3;
    inline constexpr uint32_t kScriptSound     = 4;
    inline constexpr uint32_t kScriptMusic     = 5;
    inline constexpr uint32_t kRacialCinematic = 6;   // cap = 1
    inline constexpr uint32_t kVocalUi         = 7;   // cap = 1
    inline constexpr uint32_t kItemPickup      = 8;   // cap = 2
    inline constexpr uint32_t kNpcGreeting     = 9;   // cap = 1
    inline constexpr uint32_t kWeaponSwing     = 10;  // cap = 2
    inline constexpr uint32_t kDeathThud       = 11;  // cap = 2
    inline constexpr uint32_t kVocalEmote      = 12;  // cap = 1
    inline constexpr uint32_t kFootstepOther   = 13;  // cap = 6
    inline constexpr uint32_t kCombatImpact    = 14;  // cap = 4
    inline constexpr uint32_t kSpellFizzle     = 15;  // cap = 1
    inline constexpr uint32_t kCombatVocal     = 16;  // cap = 2
    inline constexpr uint32_t kFootstepSelf    = 17;  // cap = 4
    inline constexpr uint32_t kCount           = 18;
}

enum class EnvironmentBusMode : uint8_t {
    Auto = 0,
    ForceDry = 1,
    ForceWet = 2,
};

struct SfxPlayOptions {
    uint32_t soundEntryId = 0;
    uint32_t soundEntryFlags = 0;
    uint32_t category = SESoundCategory::kSfx;
    bool loop = false;
    uint32_t eaxDef = 0;
    EnvironmentBusMode envBusMode = EnvironmentBusMode::Auto;
};

/**
 * AudioEngine: Singleton managing miniaudio device and playback.
 * Replaces process-spawning audio system with proper non-blocking library.
 */
class AudioEngine {
public:
    static AudioEngine& instance();

    /// Exact Wow.exe build 8606 SESound::ComputeAttenuation (0x756bd0):
    /// rolloff = 4.0 (0x892db0) with linear fade-to-zero over [0.9 * maxDist, maxDist] (0x8a3218).
    [[nodiscard]] static float computeDistanceAttenuation(float minDistance, float maxDistance, float distance);

    /// Exact Wow.exe build 8606 per-category concurrency cap table (0x897018).
    [[nodiscard]] static uint32_t maxConcurrentForCategory(uint32_t category);

    ~AudioEngine();

    // Initialization
    [[nodiscard]] bool initialize();
    /// Deterministic device-less initialization for automated tests: mixes on
    /// demand via renderMixFrames() without background device threads.
    [[nodiscard]] bool initializeHeadless(uint32_t sampleRate = 48000, uint32_t channels = 2);
    void shutdown();
    [[nodiscard]] bool isInitialized() const { return initialized_; }

    /// The playback device miniaudio actually opened, by name.
    [[nodiscard]] std::string getOutputDeviceName() const;

    // Master volume (0.0 = silent, 1.0 = full)
    void setMasterVolume(float volume);
    [[nodiscard]] float getMasterVolume() const { return masterVolume_; }

    /// Silence the output without forgetting the volume the player chose.
    void setSuspended(bool suspended);
    [[nodiscard]] bool isSuspended() const { return suspended_; }

    /// Window focus + Sound_EnableSoundWhenGameIsInBG gate (Wow.exe 0x4621ef -> 0x4664b0).
    void setWindowFocused(bool focused);
    [[nodiscard]] bool isWindowFocused() const { return windowFocused_; }
    void setBackgroundSoundEnabled(bool enabled);
    [[nodiscard]] bool isBackgroundSoundEnabled() const { return backgroundSoundEnabled_; }
    [[nodiscard]] bool isMutedInBackground() const {
        return suspended_ || (!windowFocused_ && !backgroundSoundEnabled_);
    }

    // Asset manager (enables sound loading by MPQ path)
    void setAssetManager(pipeline::AssetManager* am) { assetManager_ = am; }

    // 3D listener position (for positional audio)
    void setListenerPosition(const glm::vec3& position);
    void setListenerOrientation(const glm::vec3& forward, const glm::vec3& up);
    [[nodiscard]] const glm::vec3& getListenerPosition() const { return listenerPosition_; }

    // Simple 2D sound playback (non-blocking)
    bool playSound2D(const std::vector<uint8_t>& wavData, float volume = 1.0f, float pitch = 1.0f,
                     const std::string& debugPath = {}, uint32_t soundEntryId = 0);
    bool playSound2D(const std::vector<uint8_t>& wavData, float volume, float pitch,
                     const SfxPlayOptions& options, const std::string& debugPath = {});
    bool playSound2D(const std::string& mpqPath, float volume = 1.0f, float pitch = 1.0f);

    // Stoppable 2D sound - returns a non-zero handle, or 0 on failure
    uint32_t playSound2DStoppable(const std::vector<uint8_t>& wavData, float volume = 1.0f, float pitch = 1.0f,
                                  const std::string& debugPath = {}, uint32_t soundEntryId = 0);
    uint32_t playSound2DStoppable(const std::vector<uint8_t>& wavData, float volume, float pitch,
                                  const SfxPlayOptions& options, const std::string& debugPath = {});
    uint32_t playSound2DStoppable(const std::string& mpqPath, float volume = 1.0f, float pitch = 1.0f);
    // Stop a sound started with playSound2DStoppable / playSound3DStoppable (no-op if already finished)
    void stopSound(uint32_t id);
    void setSoundPosition(uint32_t id, const glm::vec3& position);
    [[nodiscard]] bool isSoundPlaying(uint32_t id);
    [[nodiscard]] bool isSoundLooping(uint32_t id) const;
    [[nodiscard]] bool isSoundEntryPlaying(uint32_t soundEntryId);
    [[nodiscard]] uint32_t getActiveCategoryCount(uint32_t category);
    [[nodiscard]] bool canStartCategory(uint32_t category);

    // 3D positional sound playback
    bool playSound3D(const std::vector<uint8_t>& wavData, const glm::vec3& position,
                     float volume = 1.0f, float pitch = 1.0f, float maxDistance = 100.0f,
                     float minDistance = 1.0f, const std::string& debugPath = {},
                     uint32_t soundEntryId = 0);
    bool playSound3D(const std::vector<uint8_t>& wavData, const glm::vec3& position,
                     float volume, float pitch, float maxDistance, float minDistance,
                     const SfxPlayOptions& options, const std::string& debugPath = {});
    bool playSound3D(const std::string& mpqPath, const glm::vec3& position,
                     float volume = 1.0f, float pitch = 1.0f, float maxDistance = 100.0f,
                     float minDistance = 1.0f);
    uint32_t playSound3DStoppable(const std::vector<uint8_t>& wavData, const glm::vec3& position,
                                  float volume = 1.0f, float pitch = 1.0f, float maxDistance = 100.0f,
                                  float minDistance = 1.0f, const std::string& debugPath = {},
                                  uint32_t soundEntryId = 0);
    uint32_t playSound3DStoppable(const std::vector<uint8_t>& wavData, const glm::vec3& position,
                                  float volume, float pitch, float maxDistance, float minDistance,
                                  const SfxPlayOptions& options, const std::string& debugPath = {});
    uint32_t playSound3DStoppable(const std::string& mpqPath, const glm::vec3& position,
                                  float volume = 1.0f, float pitch = 1.0f, float maxDistance = 100.0f,
                                  float minDistance = 1.0f);

    // Music streaming (for background music)
    bool playMusic(std::shared_ptr<const std::vector<uint8_t>> musicData,
                   float volume = 1.0f, bool loop = true,
                   const std::string& debugPath = {});
    void stopMusic();
    [[nodiscard]] bool isMusicPlaying() const;
    void setMusicVolume(float volume);

    // Update (call once per frame for cleanup/position sync)
    void update(float deltaTime);

    bool beginOutputCapture();
    void endOutputCapture();
    uint32_t readCapturedOutput(float* dst, uint32_t maxFrames);
    [[nodiscard]] uint32_t capturedOutputAvailable() const;
    [[nodiscard]] uint32_t getOutputChannels() const;
    [[nodiscard]] uint32_t getOutputSampleRate() const;
    [[nodiscard]] size_t getActiveSoundCount() const { return activeSounds_.size(); }
    void setMaxChannels(uint32_t maxChannels);
    [[nodiscard]] uint32_t getMaxChannels() const { return maxChannels_; }

    /// Synchronously render interleaved float32 PCM frames from the mixer
    /// (used in headless mode for deterministic audio verification).
    uint32_t renderMixFrames(float* dstInterleaved, uint32_t frameCount);

    struct PlaybackEvent {
        uint32_t soundEntryId = 0;
        std::string soundEntryName;
        std::string path;
        size_t byteSize = 0;
        uint64_t decodedFrames = 0;
        uint32_t sampleRate = 0;
        float volume = 0.0f;
        float pitch = 1.0f;
        bool spatial = false;
        bool music = false;
        glm::vec3 position{0.0f};
        float minDistance = 1.0f;
        float maxDistance = 100.0f;
        bool started = false;
    };
    using PlaybackObserver = std::function<void(const PlaybackEvent&)>;
    void setPlaybackObserver(PlaybackObserver observer) { playbackObserver_ = std::move(observer); }

    /// Structured JSONL trace output (auto-opened when WOWEE_AUDIO_TRACE is set).
    bool setAudioTracePath(const std::string& jsonlPath);
    void closeAudioTrace();

    /// Environmental reverb & underwater acoustic preset (SoundProviderPreferences.dbc).
    void setEnvironmentPreset(const SoundProviderPrefRecord* preset, bool enabled = true);
    void clearEnvironmentPreset();
    [[nodiscard]] uint32_t activeEnvironmentPresetId() const;
    [[nodiscard]] bool isEnvironmentReverbEnabled() const;

private:
    bool initEnvironmentGraph();
    void uninitEnvironmentGraph();
    [[nodiscard]] ma_sound* selectTargetGroup(uint32_t resolvedSoundId, bool spatial,
                                              const SfxPlayOptions& options) const;
    void reapFinishedSounds();
    void refreshEngineMasterGain();
    void refreshSpatialVolumes();
    bool exceedsPerEntryOverlapLimit(uint32_t soundEntryId, bool stoppable, bool loopEnabled,
                                     double monoNow) const;
    bool ensureChannelSlotAvailable(bool stoppable, bool loopEnabled);
    uint32_t startSound2DInternal(const std::vector<uint8_t>& wavData, float volume, float pitch,
                                  bool stoppable, const SfxPlayOptions& options,
                                  const std::string& debugPath);
    uint32_t startSound3DInternal(const std::vector<uint8_t>& wavData, const glm::vec3& position,
                                  float volume, float pitch, float maxDistance, float minDistance,
                                  bool stoppable, const SfxPlayOptions& options,
                                  const std::string& debugPath);
    void writeTraceLineUnlocked(const std::string& jsonLine);
    void emitInitTrace(const char* backendName);

    AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // Track active sounds for cleanup, looping, NO_DUPLICATES, concurrency caps, and 3D attenuation
    struct ActiveSound {
        ma_sound* sound = nullptr;
        void* buffer = nullptr;  // ma_audio_buffer* - Keep audio buffer alive
        std::shared_ptr<const std::vector<uint8_t>> pcmDataRef;  // Keep decoded PCM alive
        uint32_t id = 0;  // 0 = anonymous (not stoppable)
        uint32_t soundEntryId = 0;
        uint32_t category = SESoundCategory::kSfx;
        bool looping = false;
        bool spatial = false;
        float baseVolume = 1.0f;
        glm::vec3 position{0.0f};
        float minDistance = 1.0f;
        float maxDistance = 100.0f;
        uint64_t traceSeq = 0;
        std::string soundEntryName;
        std::string path;
        double startMonoSec = 0.0;
    };
    std::vector<ActiveSound> activeSounds_;
    uint32_t nextSoundId_ = 1;
    uint32_t maxChannels_ = 64;

    // Music track state
    ma_sound* musicSound_ = nullptr;
    void* musicDecoder_ = nullptr;  // ma_decoder* - Keep decoder alive for streaming
    std::shared_ptr<const std::vector<uint8_t>> musicData_;  // Keep encoded music data alive
    float musicVolume_ = 1.0f;
    uint64_t musicTraceSeq_ = 0;
    std::string musicTracePath_;
    double musicStartMonoSec_ = 0.0;

    bool initialized_ = false;
    float masterVolume_ = 1.0f;
    bool suspended_ = false;
    bool windowFocused_ = true;
    bool backgroundSoundEnabled_ = false;
    glm::vec3 listenerPosition_{0.0f, 0.0f, 0.0f};
    glm::vec3 listenerForward_{0.0f, 0.0f, -1.0f};
    glm::vec3 listenerUp_{0.0f, 1.0f, 0.0f};

    pipeline::AssetManager* assetManager_ = nullptr;

    // miniaudio engine (opaque pointer)
    ma_engine* engine_ = nullptr;
    std::unique_ptr<EnvironmentReverbProcessor> envProcessor_;
    void* envEffectNode_ = nullptr;
    ma_sound* envGroup_ = nullptr;

    static void deviceDataCallback(ma_device* device, void* out, const void* in, uint32_t frames);
    void* captureRing_ = nullptr;
    std::atomic<bool> capturing_{false};
    bool headless_ = false;
    PlaybackObserver playbackObserver_;
    mutable std::mutex traceMutex_;
    std::FILE* traceFile_ = nullptr;
    std::string traceFilePath_;
    uint64_t nextTraceSeq_ = 1;
};

} // namespace audio
} // namespace wowee
