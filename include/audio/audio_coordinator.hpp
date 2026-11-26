#pragma once

#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <glm/vec3.hpp>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace game { class ZoneManager; }
namespace audio {

class MusicManager;
class FootstepManager;
class ActivitySoundManager;
class MountSoundManager;
class NpcVoiceManager;
class PlayerVoiceManager;
class AmbientSoundManager;
class UiSoundManager;
class CombatSoundManager;
class SpellSoundManager;
class MovementSoundManager;
class EnvironmentReverbTable;
struct EnvironmentQuery;

/// Flat context passed from Renderer into updateZoneAudio() each frame.
/// All values are pre-queried so AudioCoordinator needs no rendering pointers.
struct ZoneAudioContext {
    float deltaTime = 0.0f;
    glm::vec3 cameraPosition{0.0f};
    bool isSwimming = false;
    bool insideWmo = false;
    // WMOAreaTable sounds of the WMO room the player stands in (0 = none):
    // its ZoneMusic set, SoundAmbience id and IntroSound id.
    uint32_t wmoZoneMusicId = 0;
    uint32_t wmoAmbienceId = 0;
    uint32_t wmoIntroSoundId = 0;
    // The precise area at the player (WMOAreaTable's or the MCNK's), whose
    // ZoneMusic, or its nearest ancestor's, plays outside such rooms.
    uint32_t areaId = 0;
    // Visual weather state for ambient audio sync
    int weatherType = 0;      // 0=none, 1=rain, 2=snow, 3=storm
    float weatherIntensity = 0.0f;
    // Visible sky clock after zone ambience overrides (hours in [0, 24)).
    float gameTimeHours = 12.0f;
    // Zone of the terrain tile under the camera (its MCNK area ids through
    // AreaTable), for when the server has not said; 0 when unknown.
    uint32_t tileZoneId = 0;
    bool hasTile = false;
    // Server-authoritative zone (from SMSG_INIT_WORLD_STATES); 0 = offline
    uint32_t serverZoneId = 0;
    // Zone manager pointer (for zone info and music queries)
    game::ZoneManager* zoneManager = nullptr;
};

/// Coordinates all audio subsystems.
/// Extracted from Renderer to separate audio lifecycle from rendering.
/// Owned by Application; Renderer and UI components access through Application.
class AudioCoordinator {
public:
    static constexpr float kListenerCharacterZOffset = 2.0f; // Wow.exe 0x892b98
    static constexpr float kLocalPlayer2DGain = 0.65f;       // Wow.exe 0x89748c

    /// Loop Music, from the audio options. Takes effect on the next
    /// track; a track already running is not restarted to obey it.
    void setZoneMusicLooping(bool loop);

    AudioCoordinator();
    ~AudioCoordinator();

    /// Initialize the audio engine and all managers.
    /// @return true if audio is available (engine initialized successfully)
    [[nodiscard]] bool initialize();

    /// Bind AssetManager early at startup so glue screens (login, realm,
    /// character select/create) and early world-entry packets can play audio
    /// before terrain finishes loading.
    void initializeEarlyAssets(pipeline::AssetManager* assets);

    void setSfxVolumeScale(float scale) { sfxVolumeScale_ = scale; }
    [[nodiscard]] float getSfxVolumeScale() const { return sfxVolumeScale_; }

    void setEmoteSoundsEnabled(bool enabled) { emoteSoundsEnabled_ = enabled; }
    [[nodiscard]] bool areEmoteSoundsEnabled() const { return emoteSoundsEnabled_; }

    void setListenerAtCharacter(bool atChar) { listenerAtCharacter_ = atChar; }
    [[nodiscard]] bool isListenerAtCharacter() const { return listenerAtCharacter_; }

    void setBackgroundSoundEnabled(bool enabled);
    [[nodiscard]] bool isBackgroundSoundEnabled() const { return backgroundSoundEnabled_; }

    /// Compute 3D listener position per Sound_ListenerAtCharacter (Wow.exe 0x46657d: +2.0f Z).
    [[nodiscard]] glm::vec3 listenerPositionFor(const glm::vec3& characterBasePos,
                                                const glm::vec3& cameraPos,
                                                bool hasCharacter) const {
        return (listenerAtCharacter_ && hasCharacter)
            ? (characterBasePos + glm::vec3(0.0f, 0.0f, kListenerCharacterZOffset))
            : cameraPos;
    }

    /// Play a SoundEntries ID for a unit obeying Sound_ListenerAtCharacter & 0.65f self-2D rule.
    bool playUnitSound(pipeline::AssetManager* assets, uint32_t soundId,
                       const glm::vec3& unitBasePos, bool isLocalPlayer,
                       float volumeScale = 1.0f, float pitch = 1.0f) const;
    uint32_t playUnitSoundStoppable(pipeline::AssetManager* assets, uint32_t soundId,
                                    const glm::vec3& unitBasePos, bool isLocalPlayer,
                                    float volumeScale = 1.0f, float pitch = 1.0f) const;


    /// Shutdown all audio managers and engine.
    void shutdown();

    /// Per-frame zone detection, music transitions, and ambient weather sync.
    /// Called from Renderer::update() with a pre-filled context.
    void updateZoneAudio(const ZoneAudioContext& ctx);

    [[nodiscard]] const std::string& getCurrentZoneName() const { return currentZoneName_; }
    [[nodiscard]] uint32_t getCurrentZoneId() const { return currentZoneId_; }

    /// Called when the "Enable WoWee Music" setting is turned off. If one of
    /// the WoWee (file-based) tracks is currently playing, crossfade to a
    /// stock track so the disable takes effect immediately instead of waiting
    /// for the track to end.
    void onOriginalSoundtrackDisabled(game::ZoneManager* zm);

    // Accessors for all audio managers (same interface as Renderer had)
    MusicManager* getMusicManager() { return musicManager_.get(); }
    FootstepManager* getFootstepManager() { return footstepManager_.get(); }
    ActivitySoundManager* getActivitySoundManager() { return activitySoundManager_.get(); }
    MountSoundManager* getMountSoundManager() { return mountSoundManager_.get(); }
    NpcVoiceManager* getNpcVoiceManager() { return npcVoiceManager_.get(); }
    PlayerVoiceManager* getPlayerVoiceManager() { return playerVoiceManager_.get(); }
    AmbientSoundManager* getAmbientSoundManager() { return ambientSoundManager_.get(); }
    UiSoundManager* getUiSoundManager() { return uiSoundManager_.get(); }
    CombatSoundManager* getCombatSoundManager() { return combatSoundManager_.get(); }
    SpellSoundManager* getSpellSoundManager() { return spellSoundManager_.get(); }
    MovementSoundManager* getMovementSoundManager() { return movementSoundManager_.get(); }
    EnvironmentReverbTable* getEnvironmentReverbTable() { return environmentReverbTable_.get(); }
    const EnvironmentReverbTable* getEnvironmentReverbTable() const { return environmentReverbTable_.get(); }

    void setReverbEnabled(bool enabled);
    [[nodiscard]] bool isReverbEnabled() const { return reverbEnabled_; }
    void updateEnvironmentAcoustics(const EnvironmentQuery& query);
    void clearEnvironmentAcoustics();
    [[nodiscard]] uint32_t activeEnvironmentPresetId() const { return activeEnvPresetId_; }

private:
    void playZoneMusic(const std::string& music);

    std::unique_ptr<MusicManager> musicManager_;
    std::unique_ptr<FootstepManager> footstepManager_;
    std::unique_ptr<ActivitySoundManager> activitySoundManager_;
    std::unique_ptr<MountSoundManager> mountSoundManager_;
    std::unique_ptr<NpcVoiceManager> npcVoiceManager_;
    std::unique_ptr<PlayerVoiceManager> playerVoiceManager_;
    std::unique_ptr<AmbientSoundManager> ambientSoundManager_;
    std::unique_ptr<UiSoundManager> uiSoundManager_;
    std::unique_ptr<CombatSoundManager> combatSoundManager_;
    std::unique_ptr<SpellSoundManager> spellSoundManager_;
    std::unique_ptr<MovementSoundManager> movementSoundManager_;
    std::unique_ptr<EnvironmentReverbTable> environmentReverbTable_;

    bool audioAvailable_ = false;
    float sfxVolumeScale_ = 1.0f;
    bool emoteSoundsEnabled_ = true;
    bool listenerAtCharacter_ = true; // Wow.exe 0x467ec6 default "1"
    bool backgroundSoundEnabled_ = false; // Wow.exe Sound_EnableSoundWhenGameIsInBG default "0"
    bool reverbEnabled_ = true; // TBC Sound_EnableReverb default "1"
    uint32_t activeEnvPresetId_ = 0;
    bool envStateApplied_ = false;

    // Zone/music state - moved from Renderer
    uint32_t currentZoneId_ = 0;
    std::string currentZoneName_;
    /// The ZoneMusic set now playing (0 = none chosen yet).
    uint32_t currentMusicSetId_ = 0;
    bool currentMusicNight_ = false;
    uint32_t currentIntroId_ = 0;
    bool introPlaying_ = false;
    uint32_t currentAmbienceId_ = 0;
    bool ambienceNight_ = false;
    float musicSwitchCooldown_ = 0.0f;
    float audioClockSeconds_ = 0.0f;
    std::unordered_map<uint32_t, float> introReadyAt_;
    std::mt19937 rng_{std::random_device{}()};
};

} // namespace audio
} // namespace wowee
