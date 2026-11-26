#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace wowee {
namespace pipeline {
class AssetManager;
}

namespace audio {

/// 24-column row of TBC 2.4.3 SoundProviderPreferences.dbc (I3DL2 / EAX 3.0 / FMOD SFXReverb preset).
struct SoundProviderPrefRecord {
    uint32_t id = 0;                        // col 0: ID (e.g. 11 'Underwater', 73 'PRESET_AUDITORIUM', 75 'PRESET_CAVE')
    std::string description;                // col 1: Description
    uint32_t flags = 0;                     // col 2: EAX environment scale flags (0x03f, 0x01f, 0x004, 0x33f)
    int32_t eaxEnvironmentSelection = -1;   // col 3: EAXEnvironmentSelection (-1 = OFF, 0..25)
    float decayTime = 1.0f;                 // col 4: DecayTime in seconds (0.1 .. 20.0)
    float environmentSize = 7.5f;           // col 5: EnvironmentSize in meters
    float environmentDiffusion = 1.0f;      // col 6: EnvironmentDiffusion (0.0 .. 1.0)
    int32_t eaxRoom = -10000;               // col 7: Room gain in millibels (-10000 .. 0)
    int32_t eaxRoomHf = 0;                  // col 8: RoomHF gain at HFReference in millibels (-10000 .. 0)
    float eaxDecayHfRatio = 1.0f;           // col 9: DecayHFRatio (HF T60 / LF T60)
    int32_t eaxReflections = -10000;        // col 10: Reflections gain relative to Room in millibels
    float eaxReflectionsDelay = 0.007f;     // col 11: ReflectionsDelay in seconds
    int32_t eaxReverb = -10000;             // col 12: Late reverb gain relative to Room in millibels
    float eaxReverbDelay = 0.011f;          // col 13: ReverbDelay relative to Reflections in seconds
    float eaxRoomRolloffFactor = 0.0f;      // col 14: RoomRolloffFactor
    float eaxAirAbsorptionHf = -5.0f;       // col 15: AirAbsorptionHF in mB/m
    int32_t eaxRoomLf = 0;                  // col 16: RoomLF in millibels
    float eaxDecayLfRatio = 1.0f;           // col 17: DecayLFRatio
    float eaxEchoTime = 0.25f;              // col 18: EchoTime in seconds
    float eaxEchoDepth = 0.0f;              // col 19: EchoDepth (0.0 .. 1.0)
    float eaxModulationTime = 0.25f;        // col 20: ModulationTime in seconds
    float eaxModulationDepth = 0.0f;        // col 21: ModulationDepth (0.0 .. 1.0)
    float eaxHfReference = 5000.0f;         // col 22: HFReference in Hz
    float eaxLfReference = 250.0f;          // col 23: LFReference in Hz
};

/// 17-column row of TBC 2.4.3 SoundSamplePreferences.dbc (EAX 3.0 per-buffer send/occlusion profile).
struct SoundSamplePrefRecord {
    uint32_t id = 0;                        // col 0: ID (1 or 2, referenced by SoundEntries.dbc col 28 EAXDef)
    int32_t direct = 0;                     // col 1: Direct in mB
    int32_t directHf = 0;                   // col 2: DirectHF in mB
    int32_t room = 0;                       // col 3: Room in mB
    int32_t roomHf = 0;                     // col 4: RoomHF in mB
    int32_t obstruction = 0;                // col 5: Obstruction in mB
    float obstructionLfRatio = 0.0f;        // col 6: ObstructionLFRatio
    int32_t occlusion = 0;                  // col 7: Occlusion in mB
    float occlusionLfRatio = 0.25f;         // col 8: OcclusionLFRatio
    float occlusionRoomRatio = 1.5f;        // col 9: OcclusionRoomRatio
    float occlusionDirectRatio = 0.0f;      // col 10: OcclusionDirectRatio
    float exclusionLfRatio = 1.0f;          // col 11: ExclusionLFRatio
    int32_t outsideVolumeHf = 0;            // col 12: OutsideVolumeHF in mB
    float roomRolloffFactor = 1.0f;         // col 13: RoomRolloffFactor / DopplerFactor
    float exclusion = 0.0f;                 // col 14: Exclusion (-10000.0f on ID 1 suppresses wet room send; 0.0f on ID 2)
    float airAbsorptionFactor = 1.0f;       // col 15: AirAbsorptionFactor
    uint32_t flags = 0;                     // col 16: Flags
};

/// Listener spatial/acoustic query resolved once per frame into an active SoundProviderPreferences preset.
struct EnvironmentQuery {
    bool hasWmoLocation = false;
    bool insideInteriorWmo = false;
    uint32_t wmoRootId = 0;
    uint32_t wmoNameSet = 0;
    uint32_t wmoGroupId = 0;
    uint32_t areaId = 0;
    uint32_t zoneId = 0;
    bool submerged = false;
    bool reverbEnabled = true;
};

/// Loads SoundProviderPreferences.dbc, SoundSamplePreferences.dbc, WMOAreaTable.dbc (cols 4-5),
/// and AreaTable.dbc (cols 5-6), and resolves the active environmental acoustic preset.
class EnvironmentReverbTable {
public:
    static constexpr uint32_t kDefaultUnderwaterPresetId = 11u; // SoundProviderPreferences ID 11 ('Underwater')

    bool initialize(pipeline::AssetManager* assets);
    void reset();

    [[nodiscard]] bool isLoaded() const { return loaded_; }
    [[nodiscard]] size_t providerPrefCount() const { return providerPrefs_.size(); }
    [[nodiscard]] size_t samplePrefCount() const { return samplePrefs_.size(); }
    [[nodiscard]] size_t wmoPrefCount() const { return wmoPrefs_.size(); }
    [[nodiscard]] size_t areaPrefCount() const { return areaPrefs_.size(); }

    [[nodiscard]] const SoundProviderPrefRecord* findProviderPref(uint32_t id) const;
    [[nodiscard]] const SoundSamplePrefRecord* findSamplePref(uint32_t id) const;

    /// True when a SoundProviderPreferences row produces audible wet environmental reflections/reverb
    /// (excludes nullptr, ID 66 PRESET_OFF where env=-1 / room=-10000, and unflagged TESTVOLUME* debug rows).
    [[nodiscard]] static bool isActivePreset(const SoundProviderPrefRecord* rec);

    /// True when a SoundSamplePreferences row suppresses room reverb (ID 1: exclusion <= -9000).
    [[nodiscard]] bool sampleSuppressesRoom(uint32_t eaxDef) const;

    /// Raw WMOAreaTable SoundProviderPref (underwater=false) or SoundProviderPrefUnderwater (underwater=true),
    /// walking (root, nameSet, group) -> (root, nameSet, -1) -> (root, 0, group) -> (root, 0, -1).
    [[nodiscard]] int32_t lookupWmoProviderPref(uint32_t rootId, uint32_t nameSet, uint32_t groupId,
                                                bool underwater) const;

    /// Raw AreaTable SoundProviderPref (underwater=false) or SoundProviderPrefUnderwater (underwater=true),
    /// walking AreaTable ParentAreaNum inheritance up to 16 levels.
    [[nodiscard]] int32_t lookupAreaProviderPref(uint32_t areaId, bool underwater) const;

    /// Resolve active SoundProviderPreferences ID for the listener state (0 when dry or reverb disabled).
    [[nodiscard]] uint32_t resolvePresetId(const EnvironmentQuery& query) const;
    [[nodiscard]] const SoundProviderPrefRecord* resolvePreset(const EnvironmentQuery& query) const;

private:
    struct WmoKey {
        uint32_t rootId = 0;
        uint32_t nameSet = 0;
        int32_t groupId = -1;

        bool operator==(const WmoKey& o) const noexcept {
            return rootId == o.rootId && nameSet == o.nameSet && groupId == o.groupId;
        }
    };

    struct WmoKeyHash {
        size_t operator()(const WmoKey& k) const noexcept {
            size_t h = std::hash<uint32_t>{}(k.rootId);
            h ^= std::hash<uint32_t>{}(k.nameSet) + 0x9e3779b9u + (h << 6) + (h >> 2);
            h ^= std::hash<int32_t>{}(k.groupId) + 0x9e3779b9u + (h << 6) + (h >> 2);
            return h;
        }
    };

    struct PrefPair {
        int32_t pref = 0;
        int32_t underwaterPref = 0;
    };

    bool loaded_ = false;
    std::unordered_map<uint32_t, SoundProviderPrefRecord> providerPrefs_;
    std::unordered_map<uint32_t, SoundSamplePrefRecord> samplePrefs_;
    std::unordered_map<WmoKey, PrefPair, WmoKeyHash> wmoPrefs_;
    std::unordered_map<uint32_t, PrefPair> areaPrefs_;
    std::unordered_map<uint32_t, uint32_t> areaParents_;
};

/// Real-time click-free stereo I3DL2 environmental reverb + underwater HF-damping & chorus DSP.
/// Runs inside AudioEngine's miniaudio node graph (both live ALSA device and headless renderMixFrames).
class EnvironmentReverbProcessor {
public:
    EnvironmentReverbProcessor();

    void configure(uint32_t sampleRate, uint32_t channels);
    void reset();

    void setTargetPreset(const SoundProviderPrefRecord* preset, bool enabled);
    void clearTargetPreset();

    [[nodiscard]] uint32_t activePresetId() const;
    [[nodiscard]] bool isEnabled() const;

    /// Process `frameCount` interleaved frames from `framesIn` (may be nullptr when no sound is
    /// currently playing on the environment group while the reverb tail rings out) into `framesOut`.
    void processFrames(const float* framesIn, float* framesOut, uint32_t frameCount);

private:
    static constexpr size_t kCombsPerChannel = 4;
    static constexpr size_t kTotalCombs = kCombsPerChannel * 2;
    static constexpr size_t kAllpassesPerChannel = 2;

    struct TargetState {
        uint32_t presetId = 0;
        bool enabled = true;
        bool active = false;
        float inputSend = 0.0f;
        float outputEnable = 1.0f;
        float lpB = 1.0f;
        float reflGain = 0.0f;
        float revGain = 0.0f;
        uint32_t reflDelaySamples = 336;
        uint32_t tankDelaySamples = 960;
        float damp = 0.0f;
        std::array<float, kTotalCombs> combFeedback{};
        std::array<float, kTotalCombs> combInputNorm{};
        float allpassFeedback = 0.5f;
        float lfoIncRad = 0.0f;
        float modExcursionSamples = 0.0f;
    };

    struct CombLine {
        std::vector<float> buffer;
        uint32_t nominalDelay = 1024;
        uint32_t writePos = 0;
        float filterState = 0.0f;
        float lfoPhase = 0.0f;
    };

    struct AllpassLine {
        std::vector<float> buffer;
        uint32_t writePos = 0;
    };

    mutable std::mutex targetMutex_;
    TargetState target_;

    uint32_t sampleRate_ = 48000;
    uint32_t channels_ = 2;
    float smoothAlpha_ = 0.0005f;

    // Smoothed audio-thread parameters
    float curInputSend_ = 0.0f;
    float curOutputEnable_ = 1.0f;
    float curLpB_ = 1.0f;
    float curReflGain_ = 0.0f;
    float curRevGain_ = 0.0f;
    float curReflDelay_ = 336.0f;
    float curTankDelay_ = 960.0f;
    float curDamp_ = 0.0f;
    float curAllpassFb_ = 0.5f;
    float curModExcursion_ = 0.0f;
    std::array<float, kTotalCombs> curCombFeedback_{};
    std::array<float, kTotalCombs> curCombNorm_{};

    // Stereo 2-stage RoomHF input low-pass state
    float lp1L_ = 0.0f;
    float lp2L_ = 0.0f;
    float lp1R_ = 0.0f;
    float lp2R_ = 0.0f;

    // Stereo pre-delay ring buffer for early reflections and late reverb pre-delay
    std::vector<float> predelayL_;
    std::vector<float> predelayR_;
    uint32_t predelayWrite_ = 0;

    std::array<CombLine, kTotalCombs> combs_{};
    std::array<std::array<AllpassLine, kAllpassesPerChannel>, 2> allpasses_{};
    float runningTankEnergy_ = 0.0f;
};

} // namespace audio
} // namespace wowee
