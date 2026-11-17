#pragma once

#include <cstdint>
#include <random>
#include <string>
#include <vector>
#include <chrono>
#include <memory>

namespace wowee {
namespace pipeline { class AssetManager; }

namespace audio {

class CreatureSoundIndex;

/// Footsteps as the game data authors them: the unit's CreatureSoundData
/// footstep id and the TerrainType under it pick a FootstepTerrainLookup row,
/// whose SoundEntries row (or its splash row when wading) is played.
class FootstepManager {
public:
    FootstepManager();
    ~FootstepManager();

    bool initialize(pipeline::AssetManager* assets);
    void shutdown();

    void update(float deltaTime);
    /// One step of a unit drawn with CreatureDisplayInfo `displayId` onto
    /// TerrainType `terrainTypeId`.
    void playFootstep(uint32_t displayId, uint32_t terrainTypeId, bool wading);
    void setVolumeScale(float scale) { volumeScale = scale; }
    [[nodiscard]] float getVolumeScale() const { return volumeScale; }

    [[nodiscard]] bool isInitialized() const { return assetManager != nullptr; }
    [[nodiscard]] const CreatureSoundIndex* sounds() const { return sounds_.get(); }

private:
    pipeline::AssetManager* assetManager = nullptr;
    std::unique_ptr<CreatureSoundIndex> sounds_;
    std::mt19937 rng;
    float volumeScale = 1.0f;
};

} // namespace audio
} // namespace wowee
