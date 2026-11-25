#pragma once

#include "audio/footstep_manager.hpp"
#include "platform/process.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <glm/glm.hpp>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace audio {

struct CreatureVocalProfile {
    uint32_t displayId = 0;
    uint32_t soundDataId = 0;
    uint32_t exertionId = 0;          // CreatureSoundData.dbc col 1
    uint32_t exertionCritId = 0;      // CreatureSoundData.dbc col 2
    uint32_t injuryId = 0;            // CreatureSoundData.dbc col 3
    uint32_t injuryCritId = 0;        // CreatureSoundData.dbc col 4
    uint32_t injuryCrushingId = 0;    // CreatureSoundData.dbc col 5
    uint32_t deathId = 0;             // CreatureSoundData.dbc col 6
    uint32_t jumpStartId = 0;         // CreatureSoundData.dbc col 26 (0 on all 20 player races)
    uint32_t jumpEndId = 0;           // CreatureSoundData.dbc col 27 (0 on all 20 player races)
};

class ActivitySoundManager {
public:
    ActivitySoundManager();
    ~ActivitySoundManager();

    bool initialize(pipeline::AssetManager* assets);
    void shutdown();
    void update(float deltaTime);
    [[nodiscard]] bool isInitialized() const { return initialized; }

    void playJump();
    /// The character's own landing exertion on a hard landing. The contact
    /// sound of the feet is a footstep (FootstepDriver::playLandingStep).
    void playLanding(bool hardLanding);
    void setSwimmingState(bool swimming, bool moving);
    void setCharacterVoiceProfile(const std::string& modelName);
    void setCharacterVoiceProfile(const std::string& raceFolder, const std::string& raceBase, bool male);
    void setCharacterRaceGender(uint8_t raceId, uint8_t gender);
    void setActiveDisplayId(uint32_t displayId);
    void setShapeshiftForm(uint8_t formId);

    [[nodiscard]] CreatureVocalProfile getActiveVocalProfile();
    [[nodiscard]] CreatureVocalProfile resolveVocalProfileForDisplay(uint32_t displayId);
    [[nodiscard]] CreatureVocalProfile resolveVocalProfileForRace(uint8_t raceId, uint8_t gender);
    [[nodiscard]] CreatureVocalProfile resolveVocalProfileForShapeshift(uint8_t formId, uint8_t raceId = 0);

    void playWaterEnter();
    void playWaterExit();
    void playMeleeSwing();
    void playAttackGrunt();
    bool playExertion(bool isCrit = false);
    void playWound(bool isCrit = false);
    bool playCrushingWound();
    void playDeath();

    // 3D positional vocals for remote players, shapeshifted units, and creatures.
    bool playUnitExertion3D(uint32_t displayId, bool isCrit, const glm::vec3& position);
    bool playUnitWound3D(uint32_t displayId, bool isCrit, bool isCrushing, const glm::vec3& position);
    bool playUnitDeath3D(uint32_t displayId, const glm::vec3& position);
    bool playPlayerExertion3D(uint8_t raceId, uint8_t gender, uint32_t activeDisplayId,
                              bool isCrit, const glm::vec3& position);
    bool playPlayerWound3D(uint8_t raceId, uint8_t gender, uint32_t activeDisplayId,
                           bool isCrit, bool isCrushing, const glm::vec3& position);
    bool playPlayerDeath3D(uint8_t raceId, uint8_t gender, uint32_t activeDisplayId,
                           const glm::vec3& position);

    void setVolumeScale(float scale) { volumeScale = scale; }
    [[nodiscard]] float getVolumeScale() const { return volumeScale; }

private:
    struct Sample {
        std::string path;
        std::vector<uint8_t> data;
    };

    struct ShapeshiftDisplayEntry {
        std::array<uint32_t, 4> displayIds{};
    };

    bool initialized = false;
    bool dbcLoaded_ = false;
    pipeline::AssetManager* assetManager = nullptr;

    std::vector<Sample> splashEnterClips;
    std::vector<Sample> splashExitClips;
    std::vector<Sample> swimLoopClips;
    std::vector<Sample> hardLandClips;
    std::vector<Sample> meleeSwingClips;

    // DBC-backed CreatureSoundData tables
    // ChrRaces: (raceId << 1 | gender) -> base CreatureDisplayInfo ID
    std::unordered_map<uint16_t, uint32_t> raceDisplayIds_;
    // CreatureDisplayInfo ID -> CreatureSoundData ID
    std::unordered_map<uint32_t, uint32_t> displayToSoundData_;
    // Normalized model stem (e.g. "character\\human\\male\\humanmale") -> CreatureDisplayInfo / CreatureSoundData
    std::unordered_map<std::string, uint32_t> modelStemToSoundData_;
    // CreatureSoundData ID -> vocal columns
    std::unordered_map<uint32_t, CreatureVocalProfile> soundDataProfiles_;
    // SpellShapeshiftForm ID -> CreatureDisplayID[0..3]
    std::unordered_map<uint8_t, ShapeshiftDisplayEntry> shapeshiftDisplays_;

    uint8_t baseRaceId_ = 0;
    uint8_t baseGender_ = 0;
    uint32_t baseDisplayId_ = 0;
    uint32_t baseSoundDataId_ = 0;
    uint32_t activeDisplayId_ = 0;
    uint8_t shapeshiftFormId_ = 0;

    bool swimmingActive = false;
    bool swimMoving = false;
    ProcessHandle swimLoopPid = INVALID_PROCESS;
    std::string loopTempPath = platform::getTempFilePath("wowee_swim_loop.wav");
    std::mt19937 rng;

    std::chrono::steady_clock::time_point lastJumpAt{};
    std::chrono::steady_clock::time_point lastLandAt{};
    std::chrono::steady_clock::time_point lastSplashAt{};
    std::chrono::steady_clock::time_point lastMeleeSwingAt{};
    std::chrono::steady_clock::time_point lastAttackGruntAt{};
    std::chrono::steady_clock::time_point lastSwimStrokeAt{};
    bool meleeSwingWarned = false;
    std::string voiceProfileKey;
    float volumeScale = 1.0f;

    void ensureDbcLoaded();
    void applyRaceBaseStrings(const std::string& raceFolder, const std::string& raceBase, bool male);
    void preloadCandidates(std::vector<Sample>& out, const std::vector<std::string>& candidates);
    void rebuildSwimLoopClipsForProfile(const std::string& raceFolder, const std::string& raceBase, bool male);
    void rebuildHardLandClipsForProfile(const std::string& raceFolder, const std::string& raceBase, bool male);
    bool playSplash(const std::vector<Sample>& clips);
    void startSwimLoop();
    void stopSwimLoop();
    void stopOneShot();
    void reapProcesses();
};

} // namespace audio
} // namespace wowee
