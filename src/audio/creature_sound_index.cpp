#include "audio/creature_sound_index.hpp"

#include "core/logger.hpp"
#include "pipeline/dbc_loader.hpp"

#include <cassert>
#include <numeric>

namespace wowee {
namespace audio {

std::optional<SoundEntry> readSoundEntry(const pipeline::DBCFile& soundEntries, uint32_t id) {
    const int32_t found = soundEntries.findRecordById(id);
    if (found < 0) return std::nullopt;
    const auto row = static_cast<uint32_t>(found);
    const std::string directory = soundEntries.getString(row, 23);
    SoundEntry entry;
    for (uint32_t file = 0; file < 10; ++file) {
        const std::string name = soundEntries.getString(row, 3 + file);
        if (name.empty()) continue;
        entry.files.push_back(directory.empty() ? name : directory + "\\" + name);
        entry.weights.push_back(soundEntries.getUInt32(row, 13 + file));
    }
    if (entry.files.empty()) return std::nullopt;
    entry.volume = soundEntries.getFloat(row, 24);
    return entry;
}

const std::string& pickSoundFile(const SoundEntry& entry, std::mt19937& rng) {
    assert(!entry.files.empty());
    const uint64_t total = std::accumulate(entry.weights.begin(), entry.weights.end(), uint64_t{0});
    if (total == 0) {
        return entry.files[std::uniform_int_distribution<size_t>(0, entry.files.size() - 1)(rng)];
    }
    return entry.files[std::discrete_distribution<size_t>(entry.weights.begin(), entry.weights.end())(rng)];
}

void CreatureSoundIndex::load(const pipeline::DBCFile& displayInfo, const pipeline::DBCFile& modelData,
                              const pipeline::DBCFile& soundData, const pipeline::DBCFile& terrainLookup,
                              const pipeline::DBCFile& terrainType) {
    assert(soundData.getFieldCount() == 37);
    assert(terrainLookup.getFieldCount() == 5);
    assert(terrainType.getFieldCount() == 6);
    std::unordered_map<uint32_t, uint32_t> modelSound;
    for (uint32_t r = 0; r < modelData.getRecordCount(); ++r)
        modelSound[modelData.getUInt32(r, 0)] = modelData.getUInt32(r, 13);
    displaySound_.clear();
    for (uint32_t r = 0; r < displayInfo.getRecordCount(); ++r) {
        uint32_t id = displayInfo.getUInt32(r, 2);
        if (id == 0) {
            const auto it = modelSound.find(displayInfo.getUInt32(r, 1));
            if (it != modelSound.end()) id = it->second;
        }
        if (id != 0) displaySound_[displayInfo.getUInt32(r, 0)] = id;
    }
    soundData_.clear();
    for (uint32_t r = 0; r < soundData.getRecordCount(); ++r) {
        std::vector<uint32_t> row(soundData.getFieldCount());
        for (uint32_t c = 0; c < row.size(); ++c) row[c] = soundData.getUInt32(r, c);
        soundData_[row[0]] = std::move(row);
    }
    terrainSound_.clear();
    for (uint32_t r = 0; r < terrainType.getRecordCount(); ++r)
        terrainSound_[terrainType.getUInt32(r, 0)] = terrainType.getUInt32(r, 4);
    footsteps_.clear();
    for (uint32_t r = 0; r < terrainLookup.getRecordCount(); ++r) {
        const uint64_t key = (uint64_t{terrainLookup.getUInt32(r, 1)} << 32) | terrainLookup.getUInt32(r, 2);
        footsteps_[key] = {terrainLookup.getUInt32(r, 3), terrainLookup.getUInt32(r, 4)};
    }
    LOG_INFO("Creature sounds: ", displaySound_.size(), " displays, ", soundData_.size(),
             " sound sets, ", footsteps_.size(), " footstep lookups");
}

uint32_t CreatureSoundIndex::soundDataForDisplay(uint32_t displayId) const {
    const auto it = displaySound_.find(displayId);
    return it != displaySound_.end() ? it->second : 0;
}

uint32_t CreatureSoundIndex::sound(uint32_t soundDataId, CreatureSoundColumn column) const {
    const auto it = soundData_.find(soundDataId);
    if (it == soundData_.end()) return 0;
    const auto c = static_cast<uint32_t>(column);
    assert(c < it->second.size());
    return it->second[c];
}

uint32_t CreatureSoundIndex::footstepSound(uint32_t displayId, uint32_t terrainTypeId, bool splash) const {
    const uint32_t footstep = sound(soundDataForDisplay(displayId), CreatureSoundColumn::Footstep);
    // 306 of 799 sound sets leave the column 0. FootstepTerrainLookup does
    // carry footstep id 0 (AncientProtectorStep on every terrain), but that
    // many creatures stomping like an Ancient is not plausible; 0 is taken as
    // no footstep until a native capture says otherwise.
    if (footstep == 0) return 0;
    const auto terrain = terrainSound_.find(terrainTypeId);
    if (terrain == terrainSound_.end()) return 0;
    const auto it = footsteps_.find((uint64_t{footstep} << 32) | terrain->second);
    if (it == footsteps_.end()) return 0;
    return splash ? it->second.second : it->second.first;
}

} // namespace audio
} // namespace wowee
