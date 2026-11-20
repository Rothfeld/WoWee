#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace wowee {
namespace pipeline {
class AssetManager;
}

namespace audio {

struct SoundVariant {
    std::string path;
    uint32_t weight = 1;
};

struct SoundEntryRecord {
    uint32_t id = 0;
    uint32_t soundType = 0;
    std::string name;
    std::vector<SoundVariant> variants;
    float volume = 1.0f;
    uint32_t flags = 0;
    float minDistance = 8.0f;
    float maxDistance = 45.0f;
};

/// Shared TBC 2.4.3 / Classic / WotLK SoundEntries.dbc + UISoundLookups.dbc resolver.
class SoundEntryTable {
public:
    static SoundEntryTable& instance();

    bool ensureLoaded(pipeline::AssetManager* assets);
    void reset();

    [[nodiscard]] const SoundEntryRecord* findById(pipeline::AssetManager* assets, uint32_t soundId);
    [[nodiscard]] const SoundEntryRecord* findByName(pipeline::AssetManager* assets, const std::string& name);

    /// Choose one existing archive path from a SoundEntryRecord using authored weights.
    std::string pickExistingPath(pipeline::AssetManager* assets, const SoundEntryRecord& entry);

    bool play2D(pipeline::AssetManager* assets, uint32_t soundId,
                float volumeScale = 1.0f, float pitch = 1.0f);
    uint32_t play2DStoppable(pipeline::AssetManager* assets, uint32_t soundId,
                             float volumeScale = 1.0f, float pitch = 1.0f);
    bool play3D(pipeline::AssetManager* assets, uint32_t soundId, const glm::vec3& position,
                float volumeScale = 1.0f, float pitch = 1.0f);
    uint32_t play3DStoppable(pipeline::AssetManager* assets, uint32_t soundId, const glm::vec3& position,
                             float volumeScale = 1.0f, float pitch = 1.0f);

    [[nodiscard]] size_t entryCount() const { return byId_.size(); }
    [[nodiscard]] size_t namedLookupCount() const { return byName_.size(); }

private:
    SoundEntryTable();

    pipeline::AssetManager* loadedAssets_ = nullptr;
    bool loaded_ = false;
    std::unordered_map<uint32_t, SoundEntryRecord> byId_;
    std::unordered_map<std::string, uint32_t> byName_;
    std::mt19937 rng_;
};

} // namespace audio
} // namespace wowee
