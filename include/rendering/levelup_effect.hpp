#pragma once

#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <vector>

namespace wowee {
namespace rendering {

class M2Renderer;

/// Manages spawning hardcoded unit level-up M2 effects at world positions:
/// - LevelUp: Spells/LevelUp/LevelUp.m2 (SpellVisualEffectName id=21)
/// - ReputationLevelUp: Spells/ReputationLevelUp.m2 (SpellVisualEffectName id=2922)
class LevelUpEffect {
public:
    enum class Kind : uint8_t {
        LevelUp = 0,
        ReputationLevelUp = 1,
    };

    LevelUpEffect();
    ~LevelUpEffect();

    /// Load the M2 model for the specified effect kind (call once after M2Renderer is ready)
    bool loadModel(Kind kind,
                   M2Renderer* m2Renderer,
                   const std::vector<uint8_t>& m2FileData,
                   const std::vector<uint8_t>& skinFileData);

    /// Load the default LevelUp.m2 model
    bool loadModel(M2Renderer* m2Renderer,
                   const std::vector<uint8_t>& m2FileData,
                   const std::vector<uint8_t>& skinFileData) {
        return loadModel(Kind::LevelUp, m2Renderer, m2FileData, skinFileData);
    }

    /// Trigger an effect at a world position (render coords)
    void trigger(Kind kind, const glm::vec3& position);

    /// Trigger the default character level-up effect at a world position (render coords)
    void trigger(const glm::vec3& position) {
        trigger(Kind::LevelUp, position);
    }

    /// Remove expired effect instances
    void update(float deltaTime);

    [[nodiscard]] bool isModelLoaded(Kind kind = Kind::LevelUp) const {
        return modelLoaded_[static_cast<size_t>(kind)];
    }

private:
    static constexpr float EFFECT_DURATION = 3.5f;
    static constexpr uint32_t MODEL_ID = 999900;             // Character level-up effect
    // 999901 is reserved by LootSparkles; QuestMarkerRenderer starts at 999910.
    static constexpr uint32_t REPUTATION_MODEL_ID = 999902;  // Reputation standing tier-up effect

    static constexpr uint32_t modelIdFor(Kind kind) {
        return kind == Kind::ReputationLevelUp ? REPUTATION_MODEL_ID : MODEL_ID;
    }

    struct ActiveEffect {
        uint32_t instanceId;
        float elapsed;
    };

    M2Renderer* m2Renderer_ = nullptr;
    std::array<bool, 2> modelLoaded_{false, false};
    std::vector<ActiveEffect> activeEffects_;
};

} // namespace rendering
} // namespace wowee
