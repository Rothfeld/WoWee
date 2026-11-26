#pragma once

#include <string>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
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
    /// One row of WorldStateZoneSounds.dbc (8 uint32 columns in TBC 2.4.3 / WotLK):
    /// worldStateId, worldStateValue, areaId, wmoAreaId, zoneIntroMusicId,
    /// zoneMusicId, soundAmbienceId, soundProviderPrefId.
    struct WorldStateZoneSoundOverride {
        uint32_t worldStateId = 0;
        uint32_t worldStateValue = 0;
        uint32_t areaId = 0;
        uint32_t wmoAreaId = 0;
        uint32_t zoneIntroMusicId = 0;
        uint32_t zoneMusicId = 0;
        uint32_t soundAmbienceId = 0;
        uint32_t soundProviderPrefId = 0;
    };

    /// WMOAreaTable's ZoneMusic, AmbienceID, IntroSound and SoundProviderPref
    /// for a WMO room (with active WorldStateZoneSounds overrides applied),
    /// falling back field by field to the WMO's root row; 0 where neither names one.
    struct WmoAreaSounds {
        uint32_t zoneMusicId = 0;
        uint32_t ambienceId = 0;
        uint32_t introSoundId = 0;
        uint32_t soundProviderPrefId = 0;
        uint32_t soundProviderPrefUnderwaterId = 0;
        uint32_t wmoAreaId = 0;
        uint32_t areaId = 0;
    };

    /// Full resolved environmental audio IDs at a player position, applying
    /// WorldStateZoneSounds overrides and priority:
    /// WMO group -> WMO root -> precise AreaTable (and ancestors) -> zone AreaTable.
    struct ResolvedZoneAudio {
        uint32_t zoneMusicId = 0;
        uint32_t soundAmbienceId = 0;
        uint32_t zoneIntroMusicId = 0;
        uint32_t soundProviderPrefId = 0;
        uint32_t soundProviderPrefUnderwaterId = 0;
    };

    struct ZoneMusicSet {
        std::vector<std::string> dayTracks;
        std::vector<std::string> nightTracks;
        float daySilenceMinSec = 0.0f;
        float daySilenceMaxSec = 0.0f;
        float nightSilenceMinSec = 0.0f;
        float nightSilenceMaxSec = 0.0f;
    };
    struct ZoneIntroMusic {
        uint32_t id = 0;
        std::vector<std::string> tracks;
        uint32_t priority = 1;
        float minDelaySeconds = 600.0f;
    };

    void initialize();

    // Supplement zone music paths using AreaTable -> ZoneMusic -> SoundEntries DBC chain
    // plus WorldStateZoneSounds.dbc and SoundProviderPreferences.dbc.
    // Safe to call after initialize(); preserves active server world-state values.
    void enrichFromDBC(pipeline::AssetManager* assets);

    /// Replace or update server world states (SMSG_INIT_WORLD_STATES /
    /// SMSG_UPDATE_WORLD_STATE). Returns true when any world state watched by
    /// WorldStateZoneSounds.dbc changed value.
    bool setWorldStates(const std::unordered_map<uint32_t, uint32_t>& states);
    bool setWorldState(uint32_t worldStateId, uint32_t value);
    void clearWorldStates();
    [[nodiscard]] uint32_t getWorldStateValue(uint32_t worldStateId) const;
    [[nodiscard]] const std::vector<WorldStateZoneSoundOverride>& getWorldStateZoneSounds() const {
        return worldStateZoneSounds_;
    }
    /// Add a WorldStateZoneSounds override rule directly (used by enrichFromDBC and tests).
    void addWorldStateZoneSoundOverride(const WorldStateZoneSoundOverride& rule);

    /// The zone of an ADT tile, from its own MCNK area ids through
    /// AreaTable parentage: the zone most of its chunks belong to. For the
    /// moments the chunk under the player carries area 0.
    [[nodiscard]] uint32_t dominantZoneId(const std::vector<uint32_t>& areaIds) const;
    [[nodiscard]] uint32_t resolveAreaZoneId(uint32_t areaId) const;
    [[nodiscard]] uint32_t getMapZoneId(uint32_t mapId) const;
    uint32_t getWmoAreaId(uint32_t root, uint32_t nameSet, uint32_t group) const;
    [[nodiscard]] uint32_t getWmoAreaTableId(uint32_t root, uint32_t nameSet, uint32_t group) const;

    [[nodiscard]] WmoAreaSounds getWmoAreaSounds(uint32_t root, uint32_t nameSet, uint32_t group) const;
    [[nodiscard]] ResolvedZoneAudio resolveZoneAudio(uint32_t wmoRoot, uint32_t wmoNameSet,
                                                     uint32_t wmoGroup, bool hasWmoArea,
                                                     uint32_t areaId, uint32_t zoneId) const;
    /// The tracks of a ZoneMusic set (day and night SoundEntries).
    [[nodiscard]] const std::vector<std::string>* getZoneMusicTracks(uint32_t zoneMusicId) const;
    [[nodiscard]] const ZoneMusicSet* getZoneMusicSet(uint32_t zoneMusicId) const;
    [[nodiscard]] bool zoneMusicSetContainsTrack(uint32_t zoneMusicId, const std::string& track) const;
    std::string pickZoneMusicTrack(uint32_t zoneId, uint32_t zoneMusicId, bool night, bool includeWoweeTracks);
    [[nodiscard]] std::pair<float, float> getZoneSilenceInterval(uint32_t zoneMusicId, bool night) const;
    /// The ZoneMusic set an area plays (with WorldStateZoneSounds override): its own, else nearest ancestor's.
    [[nodiscard]] uint32_t getAreaZoneMusicId(uint32_t areaId) const;
    /// The ZoneIntroMusicTable id an area plays (with WorldStateZoneSounds override): its own, else nearest ancestor's.
    [[nodiscard]] uint32_t getAreaIntroMusicId(uint32_t areaId) const;
    [[nodiscard]] const ZoneIntroMusic* getZoneIntroMusic(uint32_t introId) const;
    /// The SoundAmbience id an area plays (with WorldStateZoneSounds override): its own, else nearest ancestor's.
    [[nodiscard]] uint32_t getAreaAmbienceId(uint32_t areaId) const;
    /// The SoundProviderPreferences id an area uses (with WorldStateZoneSounds override): its own, else nearest ancestor's.
    [[nodiscard]] uint32_t getAreaSoundProviderPrefId(uint32_t areaId) const;
    /// The underwater SoundProviderPreferences id an area uses: its own, else nearest ancestor's.
    [[nodiscard]] uint32_t getAreaUnderwaterSoundProviderPrefId(uint32_t areaId) const;
    /// SoundAmbience's day or night sound file for an ambience id; empty
    /// where the row names none for that half of the day.
    [[nodiscard]] std::string getAmbienceSound(uint32_t ambienceId, bool night) const;
    [[nodiscard]] float getAmbienceVolume(uint32_t ambienceId, bool night) const;
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

    /// AreaTable AmbientMultiplier (field 34) of an area, or of its parent
    /// zone when the area's own is 0: Agama'gor (1700, 0) is lit as The
    /// Barrens (17, 0.3). 0 for unknown areas and for layouts without the
    /// column. See Renderer::updatePerFrameUBO for what it does to units.
    [[nodiscard]] float getAreaAmbientMultiplier(uint32_t areaId) const;
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
    struct WmoAreaRecord {
        uint32_t recordId = 0;
        uint32_t areaId = 0;
        uint32_t soundProviderPrefId = 0;
        uint32_t soundProviderPrefUnderwaterId = 0;
        uint32_t ambienceId = 0;
        uint32_t zoneMusicId = 0;
        uint32_t introSoundId = 0;
        std::string name;
    };

    [[nodiscard]] bool isValidZoneMusicId(uint32_t id) const;
    [[nodiscard]] bool isValidIntroMusicId(uint32_t id) const;
    [[nodiscard]] bool isValidAmbienceId(uint32_t id) const;
    [[nodiscard]] bool isValidSoundProviderPrefId(uint32_t id) const;
    [[nodiscard]] WorldStateZoneSoundOverride resolveAreaWorldStateOverride(uint32_t areaId) const;
    [[nodiscard]] WorldStateZoneSoundOverride resolveWmoWorldStateOverride(uint32_t wmoAreaId,
                                                                           uint32_t areaId) const;

    std::unordered_map<uint32_t, uint32_t> mapZones_;
    std::map<std::tuple<uint32_t, uint32_t, uint32_t>, WmoAreaRecord> wmoAreaRecords_;
    std::unordered_map<uint32_t, std::vector<std::string>> zoneMusicTracks_;
    std::unordered_map<uint32_t, ZoneMusicSet> zoneMusicSets_;
    std::unordered_map<uint32_t, ZoneIntroMusic> zoneIntroMusic_;
    std::unordered_map<uint32_t, uint32_t> ownAreaZoneMusic_;
    std::unordered_map<uint32_t, uint32_t> ownAreaIntroMusic_;
    std::unordered_map<uint32_t, uint32_t> ownAreaAmbience_;
    std::unordered_map<uint32_t, uint32_t> ownAreaSoundProviderPref_;
    std::unordered_map<uint32_t, uint32_t> ownAreaUnderwaterSoundProviderPref_;
    std::unordered_map<uint32_t, uint32_t> areaZoneMusic_;
    std::unordered_map<uint32_t, uint32_t> areaIntroMusic_;
    std::unordered_map<uint32_t, uint32_t> areaAmbience_;
    std::unordered_map<uint32_t, std::pair<std::string, std::string>> ambiencePaths_;
    std::unordered_map<uint32_t, std::pair<float, float>> ambienceVolumes_;
    std::unordered_set<uint32_t> soundProviderPrefs_;
    std::vector<WorldStateZoneSoundOverride> worldStateZoneSounds_;
    std::unordered_map<uint32_t, std::vector<size_t>> areaWorldStateOverrides_;
    std::unordered_map<uint32_t, std::vector<size_t>> wmoWorldStateOverrides_;
    std::unordered_set<uint32_t> watchedWorldStateIds_;
    std::unordered_map<uint32_t, uint32_t> worldStates_;
    std::unordered_map<uint32_t, ZoneInfo> zones;
    std::unordered_map<uint32_t, uint32_t> areaParents_;
    /// AreaTable's Flags column, by area id. Read in the same pass as the
    /// parents; the file is walked once either way.
    std::unordered_map<uint32_t, uint32_t> areaFlags_;
    std::unordered_map<uint32_t, float> areaAmbientMultipliers_;
    std::string lastPlayedMusic_;
    bool useOriginalSoundtrack_ = true;
};

} // namespace game
} // namespace wowee
