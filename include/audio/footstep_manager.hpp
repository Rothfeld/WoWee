#pragma once

#include "pipeline/m2_loader.hpp"

#include <glm/glm.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace wowee {
namespace pipeline { class AssetManager; }

namespace audio {

class CreatureSoundIndex;

/// TBC 2.4.3 unit & M2 animation-event audio dispatcher:
///   - $FSD footsteps (local 2D 0.65x / camera-3D +2.0z, remote/NPC 3D, bare-foot fallback)
///   - $DTH corpse impact thuds (DeathThudLookups.dbc: SizeClass x TerrainType x inWater)
///   - $TRD chest/leg armor foley (Material.dbc col 2 + Self/Others CVars)
///   - $CSD, $SCD, $SMG, $SMD creature/character M2 sound events
///   - $SND, $DSO, $DSL, $GO0..5, $GC0..3 spell/doodad/GameObject M2 sound events
///   - PlayerDrowningLoop (3353) + Drowning (5736) / Injury ticks
///   - CreatureSoundData.LoopSoundID (col 24: elementals, Infernal, slimes, mechanicals)
class FootstepManager {
public:
    static constexpr uint32_t kPlayerDrowningLoopSoundId = 3353;
    static constexpr uint32_t kDrowningBubbleSoundId = 5736;
    static constexpr float kLocalSelf2DGain = 0.65f;
    static constexpr float kLocalListenerZOffset = 2.0f;

    FootstepManager();
    ~FootstepManager();

    bool initialize(pipeline::AssetManager* assets);
    void shutdown();

    void update(float deltaTime);

    /// Legacy 2D step entry point (local player when ListenerAtCharacter is true).
    void playFootstep(uint32_t displayId, uint32_t terrainTypeId, bool wading);

    /// Spatialized $FSD step (local self = 0.65f 2D when listenerAtCharacter, else 3D).
    bool playFootstepAt(uint32_t displayId, uint32_t terrainTypeId, bool wading, bool bareFeet,
                        const glm::vec3& position, bool isLocalPlayer, bool listenerAtCharacter);

    /// $DTH corpse impact thud via DeathThudLookups.dbc.
    bool playDeathThud(uint32_t displayId, uint32_t terrainTypeId, bool inWater,
                       const glm::vec3& position, bool isLocalPlayer, bool listenerAtCharacter);

    /// $TRD chest/leg armor foley via Material.dbc col 2, gated by Self/Others CVars.
    bool playArmorFoley(uint32_t materialId, const glm::vec3& position,
                        bool isLocalPlayer, bool listenerAtCharacter);
    /// CreatureSoundData WingFlap / WingGlide for flying shapeshifts (e.g. Druid Flight Form).
    bool playCreatureWingSound(uint32_t displayId, bool glide, const glm::vec3& position,
                               bool isLocalPlayer, bool listenerAtCharacter);

    /// Dispatch a unit/character M2 audio event ($FSD, $DTH, $TRD, $CSD, $SCD, $SMG, $SMD, $SND, $DSO).
    bool playUnitM2Event(pipeline::M2AudioEventType type, uint32_t eventData,
                         uint32_t displayId, uint32_t terrainTypeId, bool wadingOrInWater,
                         bool bareFeet, uint32_t armorMaterialId, const glm::vec3& position,
                         bool isLocalPlayer, bool listenerAtCharacter, bool skipFootstep = false);

    /// Step M2Renderer instance audio events ($SND, $DSO, $DSL, $GO0..5, $GC0..3) across [prevTime, animTimeMs].
    uint32_t stepModelAudioEvents(
        uint32_t instanceId,
        const std::vector<pipeline::M2Model::AudioEvent>& events,
        int sequenceIndex,
        float animTimeMs,
        float animDurationMs,
        bool heldAtEnd,
        const std::function<glm::vec3(uint16_t bone, const glm::vec3& localOffset)>& boneWorldPos,
        const std::function<uint32_t(uint32_t goSlotIndex)>& goSlotSoundResolver = {});
    void stopModelAudioInstance(uint32_t instanceId);
    void clearAllModelAudioInstances();
    [[nodiscard]] bool hasActiveModelLoop(uint32_t instanceId) const;

    struct CharacterAudioStepContext {
        uint32_t displayId = 0;
        uint32_t terrainTypeId = 0;
        bool hasTerrainType = false;
        bool wading = false;
        bool inWater = false;
        bool bareFeet = false;
        uint32_t armorMaterialId = 0;
        bool isLocalPlayer = false;
        bool listenerAtCharacter = true;
        bool skipFootstep = false;
        bool muteEvents = false; // e.g. beyond max audible range: advance cursor without playing
        /// Optional bridge callback for combat & emote M2 keyframe events
        /// ($CSS, $CAH, $AH0..3, $BWP, $BWR, $SHL, $SHR, $ESD).
        std::function<bool(pipeline::M2AudioEventType type, uint32_t eventData,
                           uint32_t animId, const glm::vec3& worldPos)> onCustomUnitEvent;
    };

    /// Step CharacterRenderer instance audio events (base sequence + optional upper-body sequence).
    uint32_t stepCharacterAudioEvents(
        uint32_t instanceId,
        const pipeline::M2Model& model,
        int baseSeqIndex,
        float baseTimeMs,
        bool playReversed,
        int upperSeqIndex,
        float upperTimeMs,
        const CharacterAudioStepContext& ctx,
        const std::function<glm::vec3(uint16_t bone, const glm::vec3& localOffset)>& boneWorldPos);
    void removeCharacterAudioInstance(uint32_t instanceId);
    void clearAllCharacterAudioInstances();

    /// Local player drowning loop (3353 PlayerDrowningLoop) + DAMAGE_DROWNING tick (5736 + Injury).
    void updateDrowningState(bool isDrowning);
    void onDrowningDamageTick(uint32_t playerDisplayId, const glm::vec3& position, bool listenerAtCharacter);
    void stopDrowningLoop();
    [[nodiscard]] bool hasActiveDrowningLoop() const noexcept { return drowningLoopHandle_ != 0; }

    /// CreatureSoundData.LoopSoundID (col 24: elementals, Infernal, slimes, mechanicals) lifecycle.
    void syncCreatureLoopSound(uint64_t guid, uint32_t displayId, const glm::vec3& position, bool alive);
    void stopCreatureLoopSound(uint64_t guid);
    void clearAllCreatureLoops();
    [[nodiscard]] bool hasActiveCreatureLoop(uint64_t guid) const;
    [[nodiscard]] size_t activeCreatureLoopCount() const noexcept { return creatureLoops_.size(); }

    void setArmorFoleySelfEnabled(bool enabled) noexcept { armorFoleySelfEnabled_ = enabled; }
    [[nodiscard]] bool isArmorFoleySelfEnabled() const noexcept { return armorFoleySelfEnabled_; }
    void setArmorFoleyOthersEnabled(bool enabled) noexcept { armorFoleyOthersEnabled_ = enabled; }
    [[nodiscard]] bool isArmorFoleyOthersEnabled() const noexcept { return armorFoleyOthersEnabled_; }

    void setLocalPlayerBarefoot(bool barefoot) noexcept { localPlayerBarefoot_ = barefoot; }
    [[nodiscard]] bool isLocalPlayerBarefoot() const noexcept { return localPlayerBarefoot_; }

    void setVolumeScale(float scale) { volumeScale = scale; }
    [[nodiscard]] float getVolumeScale() const { return volumeScale; }

    [[nodiscard]] bool isInitialized() const { return assetManager != nullptr; }
    [[nodiscard]] pipeline::AssetManager* assets() const noexcept { return assetManager; }
    [[nodiscard]] const CreatureSoundIndex* sounds() const { return sounds_.get(); }
    [[nodiscard]] CreatureSoundIndex* mutableSounds() { return sounds_.get(); }

private:
    bool playSoundSpatialOrSelf(uint32_t soundId, const glm::vec3& position,
                                bool isLocalPlayer, bool listenerAtCharacter);

    struct ModelAudioCursor {
        int prevSequence = -1;
        float prevTimeMs = 0.0f;
        uint32_t activeLoopHandle = 0;
        uint32_t activeLoopSoundId = 0;
        int activeLoopSequence = -1;
    };

    struct CharacterAudioCursor {
        int prevBaseSeq = -1;
        float prevBaseTimeMs = 0.0f;
        int prevUpperSeq = -1;
        float prevUpperTimeMs = 0.0f;
    };

    struct CreatureLoopState {
        uint32_t soundId = 0;
        uint32_t handle = 0;
    };

    pipeline::AssetManager* assetManager = nullptr;
    std::unique_ptr<CreatureSoundIndex> sounds_;
    std::mt19937 rng;
    float volumeScale = 1.0f;
    bool armorFoleySelfEnabled_ = true;
    bool armorFoleyOthersEnabled_ = true;
    bool localPlayerBarefoot_ = false;
    uint32_t drowningLoopHandle_ = 0;
    std::unordered_map<uint32_t, ModelAudioCursor> modelAudioCursors_;
    std::unordered_map<uint32_t, CharacterAudioCursor> charAudioCursors_;
    std::unordered_map<uint64_t, CreatureLoopState> creatureLoops_;
};

} // namespace audio
} // namespace wowee
