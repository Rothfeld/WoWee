#pragma once

#include <string>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>
#include <map>
#include <tuple>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace game {

/// The zone most of these zone ids name; 0 when none is non-zero. Ties go
/// to the smaller id so the answer does not depend on input order.
[[nodiscard]] inline uint32_t mostCommonZoneId(const std::vector<uint32_t>& zoneIds) {
    std::map<uint32_t, int> counts;
    for (uint32_t z : zoneIds) if (z != 0) ++counts[z];
    uint32_t best = 0;
    int bestCount = 0;
    // std::map walks ids in order, so a tie goes to the smaller id.
    for (const auto& [z, n] : counts) {
        if (n > bestCount) { best = z; bestCount = n; }
    }
    return best;
}

struct ZoneInfo {
    uint32_t id;
    std::string name;
    std::vector<std::string> musicPaths;  // MPQ paths to music files
};

class ZoneManager {
public:
    void initialize();

    // Supplement zone music paths using AreaTable → ZoneMusic → SoundEntries DBC chain.
    // Safe to call after initialize(); idempotent and additive (does not remove existing paths).
    void enrichFromDBC(pipeline::AssetManager* assets);

    /// The zone of an ADT tile, from its own MCNK area ids through
    /// AreaTable parentage: the zone most of its chunks belong to. For the
    /// moments the chunk under the player carries area 0.
    [[nodiscard]] uint32_t dominantZoneId(const std::vector<uint32_t>& areaIds) const;
    [[nodiscard]] uint32_t resolveAreaZoneId(uint32_t areaId) const;
    [[nodiscard]] uint32_t getMapZoneId(uint32_t mapId) const;
    uint32_t getWmoAreaId(uint32_t root, uint32_t nameSet, uint32_t group) const;

    /// WMOAreaTable's ZoneMusic and AmbienceID for a WMO room, falling back
    /// field by field to the WMO's root row; 0 where neither names one.
    struct WmoAreaSounds { uint32_t zoneMusicId = 0; uint32_t ambienceId = 0; };
    [[nodiscard]] WmoAreaSounds getWmoAreaSounds(uint32_t root, uint32_t nameSet, uint32_t group) const;
    /// The tracks of a ZoneMusic set (day and night SoundEntries).
    [[nodiscard]] const std::vector<std::string>* getZoneMusicTracks(uint32_t zoneMusicId) const;
    /// The ZoneMusic set an area plays: its own, else its nearest ancestor's.
    [[nodiscard]] uint32_t getAreaZoneMusicId(uint32_t areaId) const;
    /// The SoundAmbience id an area plays: its own, else its nearest ancestor's.
    [[nodiscard]] uint32_t getAreaAmbienceId(uint32_t areaId) const;
    /// SoundAmbience's day or night sound file for an ambience id; empty
    /// where the row names none for that half of the day.
    [[nodiscard]] std::string getAmbienceSound(uint32_t ambienceId, bool night) const;
    /// A random track of a list, honouring the WoWee soundtrack setting and
    /// avoiding an immediate repeat.
    std::string pickTrack(const std::vector<std::string>& tracks);
    std::string getWmoAreaName(uint32_t root, uint32_t nameSet, uint32_t group) const;

    /// Whether an area is a world PvP objective - one of the eleven the
    /// AREA_FLAG_OUTDOOR_PVP bit marks, or anywhere in Wintergrasp, which
    /// carries a flag of its own instead.
    ///
    /// Field 4 of AreaTable.dbc, verified against two rows rather than taken
    /// from the position: Wintergrasp's own area carries 0x01000000 there and
    /// Shattrath carries the sanctuary bit, and neither would hold if the
    /// column were something else.
    [[nodiscard]] bool isOutdoorPvpArea(uint32_t areaId) const;

    /// Whether IsFlyableArea answers 1 for an area: AreaTable Flags bit
    /// 0x400. Measured on 2.4.3: Nagrand, Shattrath's Terrace of Light,
    /// Hellfire Citadel and Circle of Blood Arena (0x4d0, which lacks the
    /// 0x4000 bit the other Outland areas also carry) answer 1; Isle of
    /// Quel'Danas, Eversong Woods, Silvermoon City and Goldshire answer nil.
    [[nodiscard]] bool isFlyableArea(uint32_t areaId) const;
    [[nodiscard]] const ZoneInfo* getZoneInfo(uint32_t zoneId) const;
    std::string getRandomMusic(uint32_t zoneId);
    [[nodiscard]] std::vector<std::string> getAllMusicPaths() const;

    /// Resolve "assets/Original Music/<filename>" to a "file:<absolute path>"
    /// track the music manager plays off disk, or "" when the file is absent.
    /// Shared with the tavern rotation, which draws from the same directory.
    [[nodiscard]] static std::string resolveOriginalMusicFile(const char* filename);

    // When false, file: (original soundtrack) tracks are excluded from the pool
    void setUseOriginalSoundtrack(bool use) { useOriginalSoundtrack_ = use; }
    [[nodiscard]] bool getUseOriginalSoundtrack() const { return useOriginalSoundtrack_; }

private:
    std::unordered_map<uint32_t, uint32_t> mapZones_;
    std::map<std::tuple<uint32_t, uint32_t, uint32_t>, uint32_t> wmoAreaIds_;
    std::map<std::tuple<uint32_t, uint32_t, uint32_t>, std::string> wmoAreaNames_;
    std::map<std::tuple<uint32_t, uint32_t, uint32_t>, WmoAreaSounds> wmoAreaSounds_;
    std::unordered_map<uint32_t, std::vector<std::string>> zoneMusicTracks_;
    std::unordered_map<uint32_t, uint32_t> areaZoneMusic_;
    std::unordered_map<uint32_t, uint32_t> areaAmbience_;
    std::unordered_map<uint32_t, std::pair<std::string, std::string>> ambiencePaths_;
    std::unordered_map<uint32_t, ZoneInfo> zones;
    std::unordered_map<uint32_t, uint32_t> areaParents_;
    /// AreaTable's Flags column, by area id. Read in the same pass as the
    /// parents; the file is walked once either way.
    std::unordered_map<uint32_t, uint32_t> areaFlags_;
    std::string lastPlayedMusic_;
    bool useOriginalSoundtrack_ = true;
};

} // namespace game
} // namespace wowee
