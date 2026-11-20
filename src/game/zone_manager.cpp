#include "game/zone_manager.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"
#include "core/logger.hpp"
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <random>
#include <unordered_set>

namespace wowee {
namespace game {

// Resolve "assets/Original Music/<name>" to an absolute path, or return empty.
// fs::exists already resolves a relative path against the working directory, so
// there is only ever one place to look; the track is found when the process runs
// from the source tree and not otherwise.
static std::string resolveOriginalMusic(const char* filename) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path rel = fs::path("assets") / "Original Music" / filename;
    if (!fs::exists(rel, ec)) return "";
    fs::path abs = fs::canonical(rel, ec);
    return ec ? std::string() : abs.string();
}

// Helper: prefix with "file:" so the renderer knows to use playFilePath
static std::string filePrefix(const std::string& path) {
    if (path.empty()) return "";
    return "file:" + path;
}

std::string ZoneManager::resolveOriginalMusicFile(const char* filename) {
    return filePrefix(resolveOriginalMusic(filename));
}

namespace {

/// WoWee's own soundtrack ("assets/Original Music"), offered alongside a
/// zone's authored music while the "Enable WoWee Music" setting is on. Not
/// game data: these are this client's additions, by zone id. Everything else
/// a zone plays comes from AreaTable -> ZoneMusic -> SoundEntries.
struct WoweeTrack { uint32_t zoneId; const char* file; };
constexpr WoweeTrack kWoweeSoundtrack[] = {
    {12, "Wanderwewill.mp3"},
    {12, "You No Take Candle!.mp3"},
    {1519, "stormwindinst.mp3"},
    {1519, "stormwindguitar.mp3"},
    {1, "RunBackPolka.mp3"},
    {40, "You No Take Candle!.mp3"},
    {85, "Lanterns Over Lordaeron.mp3"},
    {1497, "Lanterns Over Lordaeron.mp3"},
    {17, "The Barrens Has No End.mp3"},
    {33, "Gold on the Tide in Booty Bay.mp3"},
    {46, "One More Pull.mp3"},
    {46, "WHO PULLED_.mp3"},
    {46, "Loot the Dogs.mp3"},
    {51, "WHO PULLED_.mp3"},
    {51, "One More Pull.mp3"},
    {51, "Loot the Dogs.mp3"},
    {1537, "RunBackPolka.mp3"},
    {1537, "Roll Need Greed.mp3"},
    {1537, "Ironforge Intro.mp3"},
    {38, "Roll Need Greed.mp3"},
    {1637, "WHO PULLED_.mp3"},
    {1637, "One More Pull.mp3"},
    {14, "The Barrens Has No End.mp3"},
    {215, "Wanderwewill.mp3"},
    {215, "The Barrens Has No End.mp3"},
    {1638, "Wanderwewill.mp3"},
    {148, "The Bone Collector.mp3"},
    {148, "Lanterns Over Lordaeron.mp3"},
    {141, "Wanderwewill.mp3"},
};

}  // namespace

void ZoneManager::initialize() {
    zones.clear();
    LOG_INFO("Zone manager initialized; zones, music and ambience come from AreaTable, "
             "WMOAreaTable, ZoneMusic, SoundAmbience and SoundEntries");
}

uint32_t ZoneManager::dominantZoneId(const std::vector<uint32_t>& areaIds) const {
    std::vector<uint32_t> zoneIds;
    zoneIds.reserve(areaIds.size());
    for (uint32_t areaId : areaIds) zoneIds.push_back(resolveAreaZoneId(areaId));
    return mostCommonZoneId(zoneIds);
}

bool ZoneManager::isOutdoorPvpArea(uint32_t areaId) const {
    if (areaId == 0) return false;
    auto it = areaFlags_.find(areaId);
    if (it == areaFlags_.end()) return false;
    // The eleven objective subzones - the Plaguelands towers, Halaa, the
    // Hellfire towers, Twin Spire Ruins, the Bone Wastes - and Wintergrasp,
    // whose eighteen areas are marked with a flag of their own rather than
    // this one.
    constexpr uint32_t kOutdoorPvp  = 0x00008000u;
    constexpr uint32_t kWintergrasp = 0x01000000u;
    return (it->second & (kOutdoorPvp | kWintergrasp)) != 0;
}

bool ZoneManager::isFlyableArea(uint32_t areaId) const {
    const auto it = areaFlags_.find(areaId);
    return it != areaFlags_.end() && (it->second & 0x400u) != 0;
}

float ZoneManager::getAreaAmbientMultiplier(uint32_t areaId) const {
    const auto own = areaAmbientMultipliers_.find(areaId);
    if (own == areaAmbientMultipliers_.end()) return 0.0f;
    if (own->second != 0.0f) return own->second;
    const auto parent = areaParents_.find(areaId);
    if (parent == areaParents_.end() || parent->second == 0 || parent->second == areaId) return 0.0f;
    const auto zone = areaAmbientMultipliers_.find(parent->second);
    return zone == areaAmbientMultipliers_.end() ? 0.0f : zone->second;
}

uint32_t ZoneManager::getMapZoneId(uint32_t mapId) const {
    const auto it = mapZones_.find(mapId);
    return it == mapZones_.end() ? 0 : resolveAreaZoneId(it->second);
}

uint32_t ZoneManager::resolveAreaZoneId(uint32_t areaId) const {
    uint32_t current = areaId;
    // AreaTable parent chains are shallow; cap traversal to tolerate malformed
    // or cyclic custom DBC records without hanging the render/audio update.
    for (int depth = 0; current != 0 && depth < 16; ++depth) {
        auto it = areaParents_.find(current);
        if (it == areaParents_.end() || it->second == 0 || it->second == current) {
            return current;
        }
        current = it->second;
    }
    return current != 0 ? current : areaId;
}

const ZoneInfo* ZoneManager::getZoneInfo(uint32_t zoneId) const {
    auto it = zones.find(zoneId);
    if (it != zones.end()) {
        return &it->second;
    }
    return nullptr;
}

std::string ZoneManager::getRandomMusic(uint32_t zoneId) {
    // An empty pool after the setting's filter is silence, never the
    // filtered tracks: the setting must stay off in zones that are all ours.
    auto it = zones.find(zoneId);
    if (it == zones.end()) return "";
    return pickTrack(it->second.musicPaths);
}

std::vector<std::string> ZoneManager::getAllMusicPaths() const {
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    for (const auto& [zoneId, zone] : zones) {
        (void)zoneId;
        for (const auto& path : zone.musicPaths) {
            if (path.empty()) continue;
            if (seen.insert(path).second) {
                out.push_back(path);
            }
        }
    }
    return out;
}

void ZoneManager::enrichFromDBC(pipeline::AssetManager* assets) {
    if (!assets) return;

    mapZones_.clear();
    const auto* layouts = pipeline::getActiveDBCLayout();
    const auto* mapLayout = layouts ? layouts->getLayout("Map") : nullptr;
    if (mapLayout && mapLayout->tryField("LinkedZone") != UINT32_MAX) {
        const auto maps = assets->loadDBC("Map.dbc");
        const auto zoneColumn = (*mapLayout)["LinkedZone"];
        if (!maps || !maps->isLoaded() || maps->getFieldCount() <= zoneColumn)
            throw std::runtime_error("Map.dbc missing or invalid linked-zone column");
        for (uint32_t row = 0; row < maps->getRecordCount(); ++row) {
            const auto zone = maps->getUInt32(row, zoneColumn);
            if (zone) mapZones_[maps->getUInt32(row, (*mapLayout)["ID"])] = zone;
        }
        LOG_INFO("Loaded map linked zones: ", mapZones_.size());
    }

    wmoAreaNames_.clear();
    wmoAreaIds_.clear();
    const auto wmoAreas = assets->loadDBC("WMOAreaTable.dbc");
    if (!wmoAreas || !wmoAreas->isLoaded() || wmoAreas->getFieldCount() < 12)
        throw std::runtime_error("WMOAreaTable.dbc missing or invalid");
    wmoAreaSounds_.clear();
    for (uint32_t row = 0; row < wmoAreas->getRecordCount(); ++row) {
        wmoAreaIds_[{wmoAreas->getUInt32(row, 1), wmoAreas->getUInt32(row, 2),
                     wmoAreas->getUInt32(row, 3)}] = wmoAreas->getUInt32(row, 10);
        // AmbienceID (6), ZoneMusic (7) and IntroSound (8): a tavern's room
        // names Zone-TavernAlliance and the Tavern ambience, a smithy BlackSmith.
        wmoAreaSounds_[{wmoAreas->getUInt32(row, 1), wmoAreas->getUInt32(row, 2),
                        wmoAreas->getUInt32(row, 3)}] =
            WmoAreaSounds{wmoAreas->getUInt32(row, 7), wmoAreas->getUInt32(row, 6),
                          wmoAreas->getUInt32(row, 8)};
        const auto name = wmoAreas->getString(row, 11);
        if (!name.empty()) wmoAreaNames_[{wmoAreas->getUInt32(row, 1),
            wmoAreas->getUInt32(row, 2), wmoAreas->getUInt32(row, 3)}] = name;
    }

    auto areaDbc = assets->loadDBC("AreaTable.dbc");
    auto zoneMusicDbc = assets->loadDBC("ZoneMusic.dbc");
    auto soundDbc = assets->loadDBC("SoundEntries.dbc");

    if (!areaDbc || !areaDbc->isLoaded()) {
        LOG_WARNING("ZoneManager::enrichFromDBC: AreaTable.dbc not available");
        return;
    }

    const uint32_t numAreas = areaDbc->getRecordCount();
    const uint32_t areaFields = areaDbc->getFieldCount();
    if (areaFields < 3) {
        LOG_WARNING("ZoneManager::enrichFromDBC: AreaTable.dbc has too few fields (", areaFields, ")");
        return;
    }

    // ID, ContinentID, ParentAreaNum are the first three fields in the client
    // layouts supported here. Preserve this relationship even when the music
    // DBCs are absent: renderer and ambience classification depend on it.
    areaParents_.clear();
    areaFlags_.clear();
    areaAmbientMultipliers_.clear();
    const bool haveFlags = areaFields > 4;
    // Field 34 is AmbientMultiplier in the 35/36-field TBC and Wrath
    // layouts (after the 17 name fields, faction group, four liquid types
    // and MinElevation, -500 on most rows). TBC values: Thousand Needles
    // 0.5, The Barrens 0.3, Outland 0.6, GM Island and Elwynn Forest 0.
    const bool haveAmbientMultiplier = areaFields >= 35;
    for (uint32_t i = 0; i < numAreas; ++i) {
        const uint32_t areaId = areaDbc->getUInt32(i, 0);
        if (areaId == 0) continue;
        areaParents_[areaId] = areaDbc->getUInt32(i, 2);
        // Field 4 is Flags. Not in dbc_layouts.json, so checked against known
        // rows rather than trusted from its position: area 4197 is Wintergrasp
        // and must carry 0x01000000, and 3703 is Shattrath and must carry the
        // sanctuary bit 0x800. A wrong column reads as zero forever.
        if (haveFlags) areaFlags_[areaId] = areaDbc->getUInt32(i, 4);
        if (haveAmbientMultiplier) areaAmbientMultipliers_[areaId] = areaDbc->getFloat(i, 34);
    }

    if (!zoneMusicDbc || !zoneMusicDbc->isLoaded()) {
        LOG_WARNING("ZoneManager::enrichFromDBC: ZoneMusic.dbc not available");
        return;
    }
    if (!soundDbc || !soundDbc->isLoaded()) {
        LOG_WARNING("ZoneManager::enrichFromDBC: SoundEntries.dbc not available");
        return;
    }

    // Build MPQ paths from a SoundEntries record.
    // Fields 3-12 = File[0..9], field 23 = DirectoryBase.
    auto getSoundPaths = [&](uint32_t soundId) -> std::vector<std::string> {
        if (soundId == 0) return {};
        int32_t idx = soundDbc->findRecordById(soundId);
        if (idx < 0) return {};
        uint32_t row = static_cast<uint32_t>(idx);
        if (soundDbc->getFieldCount() < 24) return {};
        std::string dir = soundDbc->getString(row, 23);
        std::vector<std::string> paths;
        for (uint32_t f = 3; f <= 12; ++f) {
            std::string name = soundDbc->getString(row, f);
            if (name.empty()) continue;
            paths.push_back(dir.empty() ? name : dir + "\\" + name);
        }
        return paths;
    };

    auto getSoundVolume = [&](uint32_t soundId) -> float {
        if (soundId == 0 || soundDbc->getFieldCount() <= 24) return 1.0f;
        int32_t idx = soundDbc->findRecordById(soundId);
        if (idx < 0) return 1.0f;
        const float v = soundDbc->getFloat(static_cast<uint32_t>(idx), 24);
        return (std::isfinite(v) && v > 0.0f) ? std::clamp(v, 0.0f, 1.0f) : 1.0f;
    };

    if (areaFields < 9) {
        LOG_WARNING("ZoneManager::enrichFromDBC: AreaTable.dbc has too few fields (", areaFields, ")");
        return;
    }

    // ZoneMusic.dbc: 0=ID, 1=SetName, 2=SilenceMinDay, 3=SilenceMinNight,
    // 4=SilenceMaxDay, 5=SilenceMaxNight, 6=DayMusic, 7=NightMusic.
    zoneMusicTracks_.clear();
    zoneMusicSets_.clear();
    if (zoneMusicDbc->getFieldCount() < 8)
        throw std::runtime_error("ZoneMusic.dbc has fewer than 8 fields");
    for (uint32_t row = 0; row < zoneMusicDbc->getRecordCount(); ++row) {
        const uint32_t setId = zoneMusicDbc->getUInt32(row, 0);
        auto& tracks = zoneMusicTracks_[setId];
        ZoneMusicSet& set = zoneMusicSets_[setId];
        set.daySilenceMinSec   = static_cast<float>(zoneMusicDbc->getUInt32(row, 2)) / 1000.0f;
        set.nightSilenceMinSec = static_cast<float>(zoneMusicDbc->getUInt32(row, 3)) / 1000.0f;
        set.daySilenceMaxSec   = std::max(set.daySilenceMinSec,
                                          static_cast<float>(zoneMusicDbc->getUInt32(row, 4)) / 1000.0f);
        set.nightSilenceMaxSec = std::max(set.nightSilenceMinSec,
                                          static_cast<float>(zoneMusicDbc->getUInt32(row, 5)) / 1000.0f);
        for (auto& path : getSoundPaths(zoneMusicDbc->getUInt32(row, 6))) {
            if (std::find(set.dayTracks.begin(), set.dayTracks.end(), path) == set.dayTracks.end())
                set.dayTracks.push_back(path);
            if (std::find(tracks.begin(), tracks.end(), path) == tracks.end())
                tracks.push_back(std::move(path));
        }
        for (auto& path : getSoundPaths(zoneMusicDbc->getUInt32(row, 7))) {
            if (std::find(set.nightTracks.begin(), set.nightTracks.end(), path) == set.nightTracks.end())
                set.nightTracks.push_back(path);
            if (std::find(tracks.begin(), tracks.end(), path) == tracks.end())
                tracks.push_back(std::move(path));
        }
    }

    // ZoneIntroMusicTable.dbc: 0=ID, 1=Name, 2=SoundID, 3=Priority, 4=MinDelayMinutes.
    zoneIntroMusic_.clear();
    if (auto introDbc = assets->loadDBC("ZoneIntroMusicTable.dbc");
        introDbc && introDbc->isLoaded() && introDbc->getFieldCount() >= 5) {
        for (uint32_t row = 0; row < introDbc->getRecordCount(); ++row) {
            const uint32_t introId = introDbc->getUInt32(row, 0);
            if (introId == 0) continue;
            ZoneIntroMusic intro;
            intro.id = introId;
            intro.tracks = getSoundPaths(introDbc->getUInt32(row, 2));
            intro.priority = introDbc->getUInt32(row, 3);
            const uint32_t minDelayMin = introDbc->getUInt32(row, 4);
            intro.minDelaySeconds = static_cast<float>(minDelayMin) * 60.0f;
            if (!intro.tracks.empty()) {
                zoneIntroMusic_[introId] = std::move(intro);
            }
        }
    }

    // SoundAmbience: day and night SoundEntries per ambience id. AreaTable
    // and WMOAreaTable name these, so a client without it cannot place any.
    ambiencePaths_.clear();
    ambienceVolumes_.clear();
    const auto ambience = assets->loadDBC("SoundAmbience.dbc");
    if (!ambience || !ambience->isLoaded() || ambience->getFieldCount() < 3)
        throw std::runtime_error("SoundAmbience.dbc missing or invalid");
    for (uint32_t row = 0; row < ambience->getRecordCount(); ++row) {
        const uint32_t ambId = ambience->getUInt32(row, 0);
        const uint32_t daySid = ambience->getUInt32(row, 1);
        const uint32_t nightSid = ambience->getUInt32(row, 2);
        auto day = getSoundPaths(daySid);
        auto night = getSoundPaths(nightSid);
        ambiencePaths_[ambId] = {
            day.empty() ? std::string() : day.front(),
            night.empty() ? std::string() : night.front()};
        ambienceVolumes_[ambId] = {getSoundVolume(daySid), getSoundVolume(nightSid)};
    }

    // Each area: its name, and the music (ZoneMusic, 8), intro (IntroSound, 9),
    // and ambience (AmbienceID, 7) of the nearest of itself and its ancestors
    // that names one.
    std::unordered_map<uint32_t, uint32_t> ownMusic;
    std::unordered_map<uint32_t, uint32_t> ownIntro;
    std::unordered_map<uint32_t, uint32_t> ownAmbience;
    std::unordered_map<uint32_t, std::string> names;
    for (uint32_t i = 0; i < numAreas; ++i) {
        const uint32_t areaId = areaDbc->getUInt32(i, 0);
        if (areaId == 0) continue;
        ownMusic[areaId] = areaDbc->getUInt32(i, 8);
        if (areaFields > 9) ownIntro[areaId] = areaDbc->getUInt32(i, 9);
        ownAmbience[areaId] = areaDbc->getUInt32(i, 7);
        if (areaFields > 11) names[areaId] = areaDbc->getString(i, 11);
    }
    const auto nearest = [&](const std::unordered_map<uint32_t, uint32_t>& own, uint32_t areaId) {
        uint32_t current = areaId;
        for (int depth = 0; current != 0 && depth < 16; ++depth) {
            const auto it = own.find(current);
            if (it != own.end() && it->second != 0) return it->second;
            const auto parent = areaParents_.find(current);
            if (parent == areaParents_.end() || parent->second == current) break;
            current = parent->second;
        }
        return 0u;
    };
    areaZoneMusic_.clear();
    areaIntroMusic_.clear();
    areaAmbience_.clear();
    for (const auto& [areaId, music] : ownMusic) {
        const uint32_t found = nearest(ownMusic, areaId);
        areaZoneMusic_[areaId] = found;
        areaIntroMusic_[areaId] = nearest(ownIntro, areaId);
        areaAmbience_[areaId] = nearest(ownAmbience, areaId);
        ZoneInfo& zone = zones[areaId];
        zone.id = areaId;
        zone.name = names[areaId];
        zone.musicPaths.clear();
        if (const auto t = zoneMusicTracks_.find(found); t != zoneMusicTracks_.end())
            zone.musicPaths = t->second;
    }
    for (const WoweeTrack& track : kWoweeSoundtrack) {
        const std::string path = filePrefix(resolveOriginalMusic(track.file));
        if (!path.empty()) zones[track.zoneId].musicPaths.push_back(path);
    }

    LOG_INFO("Zone music from DBC: ", zones.size(), " areas, ", zoneMusicTracks_.size(),
             " ZoneMusic sets, ", ambiencePaths_.size(), " ambiences, ",
             wmoAreaSounds_.size(), " WMO areas");

    // A track the data does not have is taken out of the pool rather than
    // picked and failed on: each one was a "Could not read" and a zone gone
    // quiet until the next pick. SoundEntries names files an extraction may
    // not hold, so this is checked against what is actually there. The
    // client's own tracks (file:) are not game data and are left.
    std::size_t dropped = 0;
    std::string firstDropped;
    std::vector<std::vector<std::string>*> pools;
    for (auto& [zoneId, zone] : zones) pools.push_back(&zone.musicPaths);
    for (auto& [setId, tracks] : zoneMusicTracks_) pools.push_back(&tracks);
    for (auto& [setId, set] : zoneMusicSets_) {
        pools.push_back(&set.dayTracks);
        pools.push_back(&set.nightTracks);
    }
    for (auto& [introId, intro] : zoneIntroMusic_) pools.push_back(&intro.tracks);
    for (auto* poolPtr : pools) {
        auto& paths = *poolPtr;
        const auto missing = [&](const std::string& path) {
            if (path.rfind("file:", 0) == 0 || assets->fileExists(path)) return false;
            if (firstDropped.empty()) firstDropped = path;
            ++dropped;
            return true;
        };
        paths.erase(std::remove_if(paths.begin(), paths.end(), missing), paths.end());
    }
    if (dropped > 0) {
        LOG_WARNING("Zone music: ", dropped, " track(s) named for zones are not in the data and "
                    "were left out, first ", firstDropped);
    }
}

ZoneManager::WmoAreaSounds ZoneManager::getWmoAreaSounds(uint32_t root, uint32_t nameSet,
                                                        uint32_t group) const {
    // The room's own row, else the WMO's root row; each field independently,
    // since a room may name only its ambience and take the building's music.
    WmoAreaSounds out;
    for (const uint32_t candidate : {group, UINT32_MAX}) {
        const auto it = wmoAreaSounds_.find({root, nameSet, candidate});
        if (it == wmoAreaSounds_.end()) continue;
        if (out.zoneMusicId == 0) out.zoneMusicId = it->second.zoneMusicId;
        if (out.ambienceId == 0) out.ambienceId = it->second.ambienceId;
        if (out.introSoundId == 0) out.introSoundId = it->second.introSoundId;
    }
    return out;
}

const std::vector<std::string>* ZoneManager::getZoneMusicTracks(uint32_t zoneMusicId) const {
    const auto it = zoneMusicTracks_.find(zoneMusicId);
    return it == zoneMusicTracks_.end() ? nullptr : &it->second;
}

const ZoneManager::ZoneMusicSet* ZoneManager::getZoneMusicSet(uint32_t zoneMusicId) const {
    const auto it = zoneMusicSets_.find(zoneMusicId);
    return it == zoneMusicSets_.end() ? nullptr : &it->second;
}

std::string ZoneManager::pickZoneMusicTrack(uint32_t zoneId, uint32_t zoneMusicId,
                                            bool night, bool includeWoweeTracks) {
    std::vector<std::string> combined;
    if (const auto* set = getZoneMusicSet(zoneMusicId)) {
        const auto& base = (night && !set->nightTracks.empty()) ? set->nightTracks : set->dayTracks;
        combined.insert(combined.end(), base.begin(), base.end());
    }
    if (includeWoweeTracks && useOriginalSoundtrack_ && zoneId != 0) {
        if (const auto zIt = zones.find(zoneId); zIt != zones.end()) {
            for (const auto& p : zIt->second.musicPaths) {
                if (p.rfind("file:", 0) == 0) combined.push_back(p);
            }
        }
    }
    return pickTrack(combined);
}

std::pair<float, float> ZoneManager::getZoneSilenceInterval(uint32_t zoneMusicId, bool night) const {
    const auto* set = getZoneMusicSet(zoneMusicId);
    if (!set) return {0.0f, 0.0f};
    if (night && (set->nightSilenceMaxSec > 0.0f || !set->nightTracks.empty())) {
        return {set->nightSilenceMinSec, set->nightSilenceMaxSec};
    }
    return {set->daySilenceMinSec, set->daySilenceMaxSec};
}

uint32_t ZoneManager::getAreaZoneMusicId(uint32_t areaId) const {
    const auto it = areaZoneMusic_.find(areaId);
    return it == areaZoneMusic_.end() ? 0 : it->second;
}

uint32_t ZoneManager::getAreaIntroMusicId(uint32_t areaId) const {
    const auto it = areaIntroMusic_.find(areaId);
    return it == areaIntroMusic_.end() ? 0 : it->second;
}

const ZoneManager::ZoneIntroMusic* ZoneManager::getZoneIntroMusic(uint32_t introId) const {
    const auto it = zoneIntroMusic_.find(introId);
    return it == zoneIntroMusic_.end() ? nullptr : &it->second;
}

uint32_t ZoneManager::getAreaAmbienceId(uint32_t areaId) const {
    const auto it = areaAmbience_.find(areaId);
    return it == areaAmbience_.end() ? 0 : it->second;
}

std::string ZoneManager::getAmbienceSound(uint32_t ambienceId, bool night) const {
    const auto it = ambiencePaths_.find(ambienceId);
    if (it == ambiencePaths_.end()) {
        LOG_ERROR("ZoneManager: SoundAmbience has no row ", ambienceId);
        return {};
    }
    return night ? it->second.second : it->second.first;
}

float ZoneManager::getAmbienceVolume(uint32_t ambienceId, bool night) const {
    const auto it = ambienceVolumes_.find(ambienceId);
    if (it == ambienceVolumes_.end()) return 1.0f;
    return night ? it->second.second : it->second.first;
}

std::string ZoneManager::pickTrack(const std::vector<std::string>& tracks) {
    std::vector<const std::string*> pool;
    for (const auto& p : tracks) {
        if (!useOriginalSoundtrack_ && p.rfind("file:", 0) == 0) continue;
        pool.push_back(&p);
    }
    if (pool.empty()) return "";
    const std::string* pick = pool[0];
    for (int attempts = 0; pool.size() > 1 && attempts < 5; ++attempts) {
        static std::mt19937 rng(std::random_device{}());
        pick = pool[std::uniform_int_distribution<size_t>(0, pool.size() - 1)(rng)];
        if (*pick != lastPlayedMusic_) break;
    }
    lastPlayedMusic_ = *pick;
    return lastPlayedMusic_;
}

uint32_t ZoneManager::getWmoAreaId(uint32_t root, uint32_t nameSet, uint32_t group) const {
    // A group without an AreaTable assignment inherits its WMO's root area.
    for (const uint32_t candidate : {group, UINT32_MAX}) {
        const auto entry = wmoAreaIds_.find({root, nameSet, candidate});
        if (entry != wmoAreaIds_.end() && entry->second != 0) return entry->second;
    }
    return 0;
}

std::string ZoneManager::getWmoAreaName(uint32_t root, uint32_t nameSet, uint32_t group) const {
    const auto exact = wmoAreaNames_.find({root, nameSet, group});
    if (exact != wmoAreaNames_.end()) return exact->second;
    return {};
}

} // namespace game
} // namespace wowee
