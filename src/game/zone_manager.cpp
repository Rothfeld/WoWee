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

void ZoneManager::initialize() {
    // Resolve original music paths at startup
    auto om = [](const char* name) -> std::string {
        std::string path = resolveOriginalMusic(name);
        return path.empty() ? "" : filePrefix(path);
    };

    std::string omWanderwewill   = om("Wanderwewill.mp3");
    std::string omYouNoTake      = om("You No Take Candle!.mp3");
    std::string omGoldBooty      = om("Gold on the Tide in Booty Bay.mp3");
    std::string omLanterns       = om("Lanterns Over Lordaeron.mp3");
    std::string omBarrens        = om("The Barrens Has No End.mp3");
    std::string omBoneCollector  = om("The Bone Collector.mp3");
    std::string omLootTheDogs    = om("Loot the Dogs.mp3");
    std::string omOneMorePull    = om("One More Pull.mp3");
    std::string omRollNeedGreed  = om("Roll Need Greed.mp3");
    std::string omRunBackPolka   = om("RunBackPolka.mp3");
    std::string omWhoPulled      = om("WHO PULLED_.mp3");
    std::string omStormwindInst  = om("stormwindinst.mp3");
    std::string omStormwindGtr   = om("stormwindguitar.mp3");
    std::string omIronforgeIntro = om("Ironforge Intro.mp3");

    // Elwynn Forest (zone 12)
    ZoneInfo elwynn;
    elwynn.id = 12;
    elwynn.name = "Elwynn Forest";
    elwynn.musicPaths = {
        "Sound\\Music\\ZoneMusic\\Forest\\DayForest01.mp3",
        "Sound\\Music\\ZoneMusic\\Forest\\DayForest02.mp3",
        "Sound\\Music\\ZoneMusic\\Forest\\DayForest03.mp3",
    };
    if (!omWanderwewill.empty()) elwynn.musicPaths.push_back(omWanderwewill);
    if (!omYouNoTake.empty()) elwynn.musicPaths.push_back(omYouNoTake);
    zones[12] = elwynn;

    // Stormwind City (zone 1519)
    ZoneInfo stormwind;
    stormwind.id = 1519;
    stormwind.name = "Stormwind City";
    stormwind.musicPaths = {
        "Sound\\Music\\CityMusic\\Stormwind\\stormwind04-zone.mp3",
        "Sound\\Music\\CityMusic\\Stormwind\\stormwind05-zone.mp3",
        "Sound\\Music\\CityMusic\\Stormwind\\stormwind06-zone.mp3",
        "Sound\\Music\\CityMusic\\Stormwind\\stormwind07-zone.mp3",
        "Sound\\Music\\CityMusic\\Stormwind\\stormwind08-zone.mp3",
    };
    if (!omStormwindInst.empty()) stormwind.musicPaths.push_back(omStormwindInst);
    if (!omStormwindGtr.empty()) stormwind.musicPaths.push_back(omStormwindGtr);
    zones[1519] = stormwind;

    // Dun Morogh (zone 1) - neighboring zone
    ZoneInfo dunmorogh;
    dunmorogh.id = 1;
    dunmorogh.name = "Dun Morogh";
    dunmorogh.musicPaths = {
        "Sound\\Music\\ZoneMusic\\Mountain\\DayMountain01.mp3",
        "Sound\\Music\\ZoneMusic\\Mountain\\DayMountain02.mp3",
        "Sound\\Music\\ZoneMusic\\Mountain\\DayMountain03.mp3",
    };
    if (!omRunBackPolka.empty()) dunmorogh.musicPaths.push_back(omRunBackPolka);
    zones[1] = dunmorogh;

    // Westfall (zone 40)
    ZoneInfo westfall;
    westfall.id = 40;
    westfall.name = "Westfall";
    westfall.musicPaths = {
        "Sound\\Music\\ZoneMusic\\Plains\\DayPlains01.mp3",
        "Sound\\Music\\ZoneMusic\\Plains\\DayPlains02.mp3",
    };
    if (!omYouNoTake.empty()) westfall.musicPaths.push_back(omYouNoTake);
    zones[40] = westfall;

    // Tirisfal Glades (zone 85)
    ZoneInfo tirisfal;
    tirisfal.id = 85;
    tirisfal.name = "Tirisfal Glades";
    tirisfal.musicPaths = {
        // EvilForest, as AreaTable -> ZoneMusic -> SoundEntries names it. There
        // was never an UndeadForest folder, so all three of these failed to
        // read and Tirisfal was silent but for the client's own tracks.
        "Sound\\Music\\ZoneMusic\\EvilForest\\DayEvilForest01.mp3",
        "Sound\\Music\\ZoneMusic\\EvilForest\\DayEvilForest02.mp3",
        "Sound\\Music\\ZoneMusic\\EvilForest\\DayEvilForest03.mp3",
        "Sound\\Music\\ZoneMusic\\EvilForest\\NightEvilForest01.mp3",
        "Sound\\Music\\ZoneMusic\\EvilForest\\NightEvilForest02.mp3",
        "Sound\\Music\\ZoneMusic\\EvilForest\\NightEvilForest03.mp3",
    };
    if (!omLanterns.empty()) tirisfal.musicPaths.push_back(omLanterns);
    zones[85] = tirisfal;

    // Undercity (zone 1497)
    ZoneInfo undercity;
    undercity.id = 1497;
    undercity.name = "Undercity";
    undercity.musicPaths = {
        "Sound\\Music\\CityMusic\\Undercity\\Undercity01-zone.mp3",
        "Sound\\Music\\CityMusic\\Undercity\\Undercity02-zone.mp3",
        "Sound\\Music\\CityMusic\\Undercity\\Undercity03-zone.mp3",
    };
    if (!omLanterns.empty()) undercity.musicPaths.push_back(omLanterns);
    zones[1497] = undercity;

    // The Barrens (zone 17)
    ZoneInfo barrens;
    barrens.id = 17;
    barrens.name = "The Barrens";
    barrens.musicPaths = {
        "Sound\\Music\\ZoneMusic\\Desert\\DayDesert01.mp3",
        "Sound\\Music\\ZoneMusic\\Desert\\DayDesert02.mp3",
        "Sound\\Music\\ZoneMusic\\Desert\\DayDesert03.mp3",
    };
    if (!omBarrens.empty()) barrens.musicPaths.push_back(omBarrens);
    zones[17] = barrens;

    // Stranglethorn Vale (zone 33)
    ZoneInfo stranglethorn;
    stranglethorn.id = 33;
    stranglethorn.name = "Stranglethorn Vale";
    stranglethorn.musicPaths = {
        "Sound\\Music\\ZoneMusic\\Jungle\\DayJungle01.mp3",
        "Sound\\Music\\ZoneMusic\\Jungle\\DayJungle02.mp3",
        "Sound\\Music\\ZoneMusic\\Jungle\\DayJungle03.mp3",
    };
    if (!omGoldBooty.empty()) stranglethorn.musicPaths.push_back(omGoldBooty);
    zones[33] = stranglethorn;

    // Duskwood (zone 10)
    ZoneInfo duskwood;
    duskwood.id = 10;
    duskwood.name = "Duskwood";
    duskwood.musicPaths = {
        "Sound\\Music\\ZoneMusic\\EvilForest\\DayEvilForest01.mp3",
        "Sound\\Music\\ZoneMusic\\EvilForest\\DayEvilForest02.mp3",
        "Sound\\Music\\ZoneMusic\\EvilForest\\DayEvilForest03.mp3",
        "Sound\\Music\\ZoneMusic\\EvilForest\\NightEvilForest01.mp3",
        "Sound\\Music\\ZoneMusic\\EvilForest\\NightEvilForest02.mp3",
        "Sound\\Music\\ZoneMusic\\EvilForest\\NightEvilForest03.mp3",
    };
    // Keep Duskwood on its original haunted-forest score; custom tracks are
    // too upbeat for the zone's persistent dark ambience.
    zones[10] = duskwood;

    // Burning Steppes (zone 46)
    ZoneInfo burningSteppes;
    burningSteppes.id = 46;
    burningSteppes.name = "Burning Steppes";
    burningSteppes.musicPaths = {
        "Sound\\Music\\ZoneMusic\\BarrenDry\\DayBarrenDry01.mp3",
        "Sound\\Music\\ZoneMusic\\BarrenDry\\DayBarrenDry02.mp3",
    };
    if (!omOneMorePull.empty()) burningSteppes.musicPaths.push_back(omOneMorePull);
    if (!omWhoPulled.empty()) burningSteppes.musicPaths.push_back(omWhoPulled);
    if (!omLootTheDogs.empty()) burningSteppes.musicPaths.push_back(omLootTheDogs);
    zones[46] = burningSteppes;

    // Searing Gorge (zone 51)
    ZoneInfo searingGorge;
    searingGorge.id = 51;
    searingGorge.name = "Searing Gorge";
    searingGorge.musicPaths = {
        "Sound\\Music\\ZoneMusic\\BarrenDry\\DayBarrenDry01.mp3",
        "Sound\\Music\\ZoneMusic\\BarrenDry\\DayBarrenDry02.mp3",
    };
    if (!omWhoPulled.empty()) searingGorge.musicPaths.push_back(omWhoPulled);
    if (!omOneMorePull.empty()) searingGorge.musicPaths.push_back(omOneMorePull);
    if (!omLootTheDogs.empty()) searingGorge.musicPaths.push_back(omLootTheDogs);
    zones[51] = searingGorge;

    // Ironforge (zone 1537)
    ZoneInfo ironforge;
    ironforge.id = 1537;
    ironforge.name = "Ironforge";
    ironforge.musicPaths = {
        "Sound\\Music\\CityMusic\\Ironforge\\IronForge Walking 01.mp3",
        "Sound\\Music\\CityMusic\\Ironforge\\Ironforge Walking 03 (Glenn).mp3",
        "Sound\\Music\\CityMusic\\Ironforge\\Ironforge Walking 04.mp3",
    };
    if (!omRunBackPolka.empty()) ironforge.musicPaths.push_back(omRunBackPolka);
    if (!omRollNeedGreed.empty()) ironforge.musicPaths.push_back(omRollNeedGreed);
    if (!omIronforgeIntro.empty()) ironforge.musicPaths.push_back(omIronforgeIntro);
    zones[1537] = ironforge;

    // Loch Modan (zone 38)
    ZoneInfo lochModan;
    lochModan.id = 38;
    lochModan.name = "Loch Modan";
    lochModan.musicPaths = {
        "Sound\\Music\\ZoneMusic\\Mountain\\DayMountain01.mp3",
        "Sound\\Music\\ZoneMusic\\Mountain\\DayMountain02.mp3",
        "Sound\\Music\\ZoneMusic\\Mountain\\DayMountain03.mp3",
    };
    if (!omRollNeedGreed.empty()) lochModan.musicPaths.push_back(omRollNeedGreed);
    zones[38] = lochModan;

    // --- Kalimdor zones ---

    // Orgrimmar (zone 1637)
    ZoneInfo orgrimmar;
    orgrimmar.id = 1637;
    orgrimmar.name = "Orgrimmar";
    orgrimmar.musicPaths = {
        "Sound\\Music\\CityMusic\\Orgrimmar\\orgrimmar01-zone.mp3",
        "Sound\\Music\\CityMusic\\Orgrimmar\\orgrimmar02-zone.mp3",
    };
    if (!omWhoPulled.empty()) orgrimmar.musicPaths.push_back(omWhoPulled);
    if (!omOneMorePull.empty()) orgrimmar.musicPaths.push_back(omOneMorePull);
    zones[1637] = orgrimmar;

    // Durotar (zone 14)
    ZoneInfo durotar;
    durotar.id = 14;
    durotar.name = "Durotar";
    durotar.musicPaths = {
        "Sound\\Music\\ZoneMusic\\Desert\\DayDesert01.mp3",
        "Sound\\Music\\ZoneMusic\\Desert\\DayDesert02.mp3",
        "Sound\\Music\\ZoneMusic\\Desert\\DayDesert03.mp3",
    };
    if (!omBarrens.empty()) durotar.musicPaths.push_back(omBarrens);
    zones[14] = durotar;

    // Mulgore (zone 215)
    ZoneInfo mulgore;
    mulgore.id = 215;
    mulgore.name = "Mulgore";
    mulgore.musicPaths = {
        "Sound\\Music\\ZoneMusic\\Plains\\DayPlains01.mp3",
        "Sound\\Music\\ZoneMusic\\Plains\\DayPlains02.mp3",
    };
    if (!omWanderwewill.empty()) mulgore.musicPaths.push_back(omWanderwewill);
    if (!omBarrens.empty()) mulgore.musicPaths.push_back(omBarrens);
    zones[215] = mulgore;

    // Thunder Bluff (zone 1638)
    ZoneInfo thunderBluff;
    thunderBluff.id = 1638;
    thunderBluff.name = "Thunder Bluff";
    thunderBluff.musicPaths = {
        "Sound\\Music\\CityMusic\\Thunderbluff\\Thunderbluff Walking 01.mp3",
        "Sound\\Music\\CityMusic\\Thunderbluff\\Thunderbluff Walking 02.mp3",
        "Sound\\Music\\CityMusic\\Thunderbluff\\Thunderbluff Walking 03.mp3",
    };
    if (!omWanderwewill.empty()) thunderBluff.musicPaths.push_back(omWanderwewill);
    zones[1638] = thunderBluff;

    // Darkshore (zone 148)
    ZoneInfo darkshore;
    darkshore.id = 148;
    darkshore.name = "Darkshore";
    darkshore.musicPaths = {
        "Sound\\Music\\ZoneMusic\\Forest\\NightForest01.mp3",
        "Sound\\Music\\ZoneMusic\\Forest\\NightForest02.mp3",
        "Sound\\Music\\ZoneMusic\\Forest\\NightForest03.mp3",
        "Sound\\Music\\ZoneMusic\\Forest\\NightForest04.mp3",
    };
    if (!omBoneCollector.empty()) darkshore.musicPaths.push_back(omBoneCollector);
    if (!omLanterns.empty()) darkshore.musicPaths.push_back(omLanterns);
    zones[148] = darkshore;

    // Teldrassil (zone 141)
    ZoneInfo teldrassil;
    teldrassil.id = 141;
    teldrassil.name = "Teldrassil";
    teldrassil.musicPaths = {
        "Sound\\Music\\ZoneMusic\\EnchantedForest\\EnchantedForest01.mp3",
        "Sound\\Music\\ZoneMusic\\EnchantedForest\\EnchantedForest02.mp3",
        "Sound\\Music\\ZoneMusic\\EnchantedForest\\EnchantedForest03.mp3",
        "Sound\\Music\\ZoneMusic\\EnchantedForest\\EnchantedForest04.mp3",
        "Sound\\Music\\ZoneMusic\\EnchantedForest\\EnchantedForest05.mp3",
    };
    if (!omWanderwewill.empty()) teldrassil.musicPaths.push_back(omWanderwewill);
    zones[141] = teldrassil;

    // Darnassus (zone 1657)
    ZoneInfo darnassus;
    darnassus.id = 1657;
    darnassus.name = "Darnassus";
    darnassus.musicPaths = {
        "Sound\\Music\\CityMusic\\Darnassus\\Darnassus Walking 1.mp3",
        "Sound\\Music\\CityMusic\\Darnassus\\Darnassus Walking 2.mp3",
        "Sound\\Music\\CityMusic\\Darnassus\\Darnassus Walking 3.mp3",
    };
    zones[1657] = darnassus;

    // Seed removed - music shuffle now uses a local mt19937 (see pickMusicTrack).

    LOG_INFO("Zone manager initialized: ", zones.size(), " zones");
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
    auto it = zones.find(zoneId);
    if (it == zones.end() || it->second.musicPaths.empty()) {
        return "";
    }

    // Build filtered pool: exclude file: (original soundtrack) tracks if disabled
    const auto& all = it->second.musicPaths;
    std::vector<const std::string*> pool;
    pool.reserve(all.size());
    for (const auto& p : all) {
        if (!useOriginalSoundtrack_ && p.rfind("file:", 0) == 0) continue;
        pool.push_back(&p);
    }
    // No fallback to the full list. The only thing filtered out here is the
    // client's own soundtrack, and only when the player has turned it off - so
    // a pool that came back empty means every track this zone has is one they
    // asked not to hear, and the answer is silence rather than all of them.
    // Putting them back was the whole of "the setting does not stay off": it
    // held in any zone with a mixed pool and did nothing in the zones that are
    // entirely ours, which are the ones a player notices.
    if (pool.empty()) return "";

    if (pool.size() == 1) {
        lastPlayedMusic_ = *pool[0];
        return lastPlayedMusic_;
    }

    // Avoid playing the same track back-to-back
    const std::string* pick = pool[0];
    for (int attempts = 0; attempts < 5; ++attempts) {
        static std::mt19937 musicRng(std::random_device{}());
        pick = pool[std::uniform_int_distribution<size_t>(0, pool.size() - 1)(musicRng)];
        if (*pick != lastPlayedMusic_) break;
    }
    lastPlayedMusic_ = *pick;
    return lastPlayedMusic_;
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
    for (uint32_t row = 0; row < wmoAreas->getRecordCount(); ++row) {
        wmoAreaIds_[{wmoAreas->getUInt32(row, 1), wmoAreas->getUInt32(row, 2),
                     wmoAreas->getUInt32(row, 3)}] = wmoAreas->getUInt32(row, 10);
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
    const bool haveFlags = areaFields > 4;
    for (uint32_t i = 0; i < numAreas; ++i) {
        const uint32_t areaId = areaDbc->getUInt32(i, 0);
        if (areaId == 0) continue;
        areaParents_[areaId] = areaDbc->getUInt32(i, 2);
        // Field 4 is Flags. Not in dbc_layouts.json, so checked against known
        // rows rather than trusted from its position: area 4197 is Wintergrasp
        // and must carry 0x01000000, and 3703 is Shattrath and must carry the
        // sanctuary bit 0x800. A wrong column reads as zero forever.
        if (haveFlags) areaFlags_[areaId] = areaDbc->getUInt32(i, 4);
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

    if (areaFields < 9) {
        LOG_WARNING("ZoneManager::enrichFromDBC: AreaTable.dbc has too few fields (", areaFields, ")");
        return;
    }

    uint32_t zonesEnriched = 0;
    for (uint32_t i = 0; i < numAreas; ++i) {
        uint32_t zoneId      = areaDbc->getUInt32(i, 0);
        uint32_t zoneMusicId = areaDbc->getUInt32(i, 8);
        if (zoneId == 0 || zoneMusicId == 0) continue;

        int32_t zmIdx = zoneMusicDbc->findRecordById(zoneMusicId);
        if (zmIdx < 0) continue;
        uint32_t zmRow = static_cast<uint32_t>(zmIdx);
        if (zoneMusicDbc->getFieldCount() < 8) continue;

        uint32_t daySoundId   = zoneMusicDbc->getUInt32(zmRow, 6);
        uint32_t nightSoundId = zoneMusicDbc->getUInt32(zmRow, 7);

        std::vector<std::string> newPaths;
        for (const auto& p : getSoundPaths(daySoundId))   newPaths.push_back(p);
        for (const auto& p : getSoundPaths(nightSoundId)) newPaths.push_back(p);
        if (newPaths.empty()) continue;

        auto& zone = zones[zoneId];
        if (zone.id == 0) zone.id = zoneId;

        // Append paths not already present (preserve hardcoded entries).
        for (const auto& path : newPaths) {
            bool found = false;
            for (const auto& existing : zone.musicPaths) {
                if (existing == path) { found = true; break; }
            }
            if (!found) {
                zone.musicPaths.push_back(path);
                ++zonesEnriched;
            }
        }
    }

    LOG_INFO("Zone music enriched from DBC: ", zones.size(), " zones, ", zonesEnriched, " paths added");

    // A track the data does not have is taken out of the pool rather than
    // picked and failed on: each one was a "Could not read" and a zone gone
    // quiet until the next pick. The table above is written by hand and the
    // data differs by expansion, so this is checked against what is actually
    // there. The client's own tracks (file:) are not game data and are left.
    std::size_t dropped = 0;
    std::string firstDropped;
    for (auto& [zoneId, zone] : zones) {
        (void)zoneId;
        auto& paths = zone.musicPaths;
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
