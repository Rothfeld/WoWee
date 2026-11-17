#pragma once

#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace wowee {
namespace pipeline { class AssetManager; class DBCFile; }
namespace audio {

/// CreatureSoundData.dbc columns (TBC 2.4.3, 37 fields). Named from the
/// SoundEntries each column points at across all 799 rows: column 1 is the
/// "...Attack" exertion of 540 rows, 3 the "...Wound" of 568, 9 a
/// FootstepTerrainLookup id, 11 "...WingFlap", 26/27 "...JumpStart/End".
enum class CreatureSoundColumn : uint32_t {
    Exertion = 1,
    ExertionCritical = 2,
    Injury = 3,
    InjuryCritical = 4,
    Death = 6,
    Stun = 7,
    Stand = 8,
    Footstep = 9,
    Aggro = 10,
    WingFlap = 11,
    WingGlide = 12,
    Alert = 13,
    Fidget0 = 14,  // 14..18
    JumpStart = 26,
    JumpEnd = 27,
};

/// One SoundEntries.dbc row: files with their weights, and the playback volume.
struct SoundEntry {
    std::vector<std::string> files;   // directory joined
    std::vector<uint32_t> weights;    // Freq column per file
    float volume = 1.0f;
};

/// SoundEntries row `id` (fields 3-12 files, 13-22 weights, 23 directory,
/// 24 volume). nullopt when the row does not exist or names no file.
std::optional<SoundEntry> readSoundEntry(const pipeline::DBCFile& soundEntries, uint32_t id);

/// A file of the entry, drawn by its Freq weights (all equal when all zero).
const std::string& pickSoundFile(const SoundEntry& entry, std::mt19937& rng);

/// Display -> CreatureSoundData row, and the footstep sound of a unit on a
/// terrain type, all from DBC rows:
///   CreatureDisplayInfo.SoundID (col 2), else CreatureModelData.SoundID (col 13)
///   CreatureSoundData.Footstep (col 9) + TerrainType.SoundID (col 4)
///   -> FootstepTerrainLookup (col 1 footstep, 2 terrain sound, 3 sound, 4 splash)
class CreatureSoundIndex {
public:
    bool load(pipeline::AssetManager& assets);
    /// Same, from DBC files already open (tests).
    void load(const pipeline::DBCFile& displayInfo, const pipeline::DBCFile& modelData,
              const pipeline::DBCFile& soundData, const pipeline::DBCFile& terrainLookup,
              const pipeline::DBCFile& terrainType);

    [[nodiscard]] uint32_t soundDataForDisplay(uint32_t displayId) const;
    /// SoundEntries id in a column of a CreatureSoundData row; 0 = none.
    [[nodiscard]] uint32_t sound(uint32_t soundDataId, CreatureSoundColumn column) const;
    /// SoundEntries id of a footstep on TerrainType `terrainTypeId`; splash
    /// when the foot lands in shallow water. 0 = no authored sound.
    [[nodiscard]] uint32_t footstepSound(uint32_t displayId, uint32_t terrainTypeId, bool splash) const;

private:
    std::unordered_map<uint32_t, uint32_t> displaySound_;      // display -> sound data
    std::unordered_map<uint32_t, std::vector<uint32_t>> soundData_;
    std::unordered_map<uint32_t, uint32_t> terrainSound_;      // TerrainType id -> SoundID
    std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> footsteps_;  // (footstep, terrain sound)
};

} // namespace audio
} // namespace wowee
