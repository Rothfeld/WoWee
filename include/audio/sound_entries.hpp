#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace wowee {
namespace pipeline {
class AssetManager;
}

namespace audio {

/// SoundEntries.dbc Flags bitmask (col 25, Wow.exe build 8606 SESound::PlaySound 0x462060..0x462620).
namespace SoundEntryFlags {
    inline constexpr uint32_t kNoDuplicates = 0x0020u; // 0x46224c: reject if same SoundEntries ID active
    inline constexpr uint32_t kLooping      = 0x0200u; // 0x462334: loop stoppable SFX handle until stopped
    inline constexpr uint32_t kVaryPitch    = 0x0400u; // 0x46255d: uniform pitch multiplier in [0.85, 1.15]
    inline constexpr uint32_t kVaryVolume   = 0x0800u; // 0x462515: uniform gain offset in [-0.15, +0.15]
}

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
    static constexpr uint32_t kAutoCategory = std::numeric_limits<uint32_t>::max();
    static constexpr float kListenerCharacterZOffset = 2.0f; // Wow.exe 0x892b98
    static constexpr float kLocalPlayer2DGain = 0.65f;       // Wow.exe 0x89748c

    static SoundEntryTable& instance();

    /// Map SoundEntries.dbc SoundType (col 1) to Wow.exe 0x897018 SESoundCategory (0..17).
    [[nodiscard]] static uint32_t categoryForSoundType(uint32_t dbcSoundType);

    bool ensureLoaded(pipeline::AssetManager* assets);
    void reset();
    void seedRngForTest(uint32_t seed) { rng_.seed(seed); }
    void upsertRecordForTest(SoundEntryRecord record) { byId_[record.id] = std::move(record); }

    [[nodiscard]] const SoundEntryRecord* findById(pipeline::AssetManager* assets, uint32_t soundId);
    [[nodiscard]] const SoundEntryRecord* findByName(pipeline::AssetManager* assets, const std::string& name);

    /// Choose one existing archive path from a SoundEntryRecord using authored weights.
    std::string pickExistingPath(pipeline::AssetManager* assets, const SoundEntryRecord& entry);

    bool play2D(pipeline::AssetManager* assets, uint32_t soundId,
                float volumeScale = 1.0f, float pitch = 1.0f,
                uint32_t categoryOverride = kAutoCategory);
    uint32_t play2DStoppable(pipeline::AssetManager* assets, uint32_t soundId,
                             float volumeScale = 1.0f, float pitch = 1.0f,
                             uint32_t categoryOverride = kAutoCategory);
    bool play3D(pipeline::AssetManager* assets, uint32_t soundId, const glm::vec3& position,
                float volumeScale = 1.0f, float pitch = 1.0f,
                uint32_t categoryOverride = kAutoCategory);
    uint32_t play3DStoppable(pipeline::AssetManager* assets, uint32_t soundId, const glm::vec3& position,
                             float volumeScale = 1.0f, float pitch = 1.0f,
                             uint32_t categoryOverride = kAutoCategory);

    /// Local-player vs world spatial routing (Wow.exe 0x466562..0x4665f6, 0x627796..0x627818):
    /// - When isLocalPlayer && listenerAtCharacter: plays 2D scaled by 0.65f (0x89748c).
    /// - Otherwise: plays 3D at unitBasePos + (0, 0, 2.0f) (0x892b98).
    bool playUnit(pipeline::AssetManager* assets, uint32_t soundId, const glm::vec3& unitBasePos,
                  bool isLocalPlayer, bool listenerAtCharacter,
                  float volumeScale = 1.0f, float pitch = 1.0f,
                  uint32_t categoryOverride = kAutoCategory);
    uint32_t playUnitStoppable(pipeline::AssetManager* assets, uint32_t soundId, const glm::vec3& unitBasePos,
                               bool isLocalPlayer, bool listenerAtCharacter,
                               float volumeScale = 1.0f, float pitch = 1.0f,
                               uint32_t categoryOverride = kAutoCategory);

    [[nodiscard]] size_t entryCount() const { return byId_.size(); }
    [[nodiscard]] size_t namedLookupCount() const { return byName_.size(); }

private:
    SoundEntryTable();

    bool prepareEntryPlayback(pipeline::AssetManager* assets, uint32_t soundId,
                              float volumeScale, float pitch, bool stoppable,
                              uint32_t categoryOverride,
                              const SoundEntryRecord*& outRec, std::string& outPath,
                              float& outVolume, float& outPitch, uint32_t& outCategory,
                              bool& outLoop);

    pipeline::AssetManager* loadedAssets_ = nullptr;
    bool loaded_ = false;
    std::unordered_map<uint32_t, SoundEntryRecord> byId_;
    std::unordered_map<std::string, uint32_t> byName_;
    std::mt19937 rng_;
};

} // namespace audio
} // namespace wowee
