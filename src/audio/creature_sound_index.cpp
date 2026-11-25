#include "audio/creature_sound_index.hpp"

#include "core/logger.hpp"
#include "pipeline/dbc_loader.hpp"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <numeric>

namespace wowee {
namespace audio {

namespace {

bool containsTokenCI(const std::string& haystack, const char* needle) {
    if (haystack.empty() || !needle || !*needle) return false;
    std::string lower = haystack;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.find(needle) != std::string::npos;
}

} // namespace

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
    assert(soundData.getFieldCount() >= 31);
    assert(terrainLookup.getFieldCount() == 5);
    assert(terrainType.getFieldCount() == 6);
    std::unordered_map<uint32_t, uint32_t> modelSound;
    std::unordered_map<uint32_t, uint32_t> modelSize;
    for (uint32_t r = 0; r < modelData.getRecordCount(); ++r) {
        const uint32_t mid = modelData.getUInt32(r, 0);
        modelSound[mid] = modelData.getUInt32(r, 13);
        if (modelData.getFieldCount() > 3) {
            modelSize[mid] = modelData.getUInt32(r, 3); // SizeClass (0..4)
        }
    }
    displaySound_.clear();
    displaySizeClass_.clear();
    for (uint32_t r = 0; r < displayInfo.getRecordCount(); ++r) {
        const uint32_t dispId = displayInfo.getUInt32(r, 0);
        const uint32_t mid = displayInfo.getUInt32(r, 1);
        uint32_t id = displayInfo.getUInt32(r, 12 < displayInfo.getFieldCount() ? 2 : 2);
        // CreatureDisplayInfo col 12 is NPCSoundID; col 2 is SoundID override -> else CreatureModelData.SoundID (col 13)
        if (id == 0) {
            const auto it = modelSound.find(mid);
            if (it != modelSound.end()) id = it->second;
        }
        if (id != 0) displaySound_[dispId] = id;
        if (const auto sIt = modelSize.find(mid); sIt != modelSize.end()) {
            displaySizeClass_[dispId] = sIt->second;
        }
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

void CreatureSoundIndex::loadDeathThudLookups(const pipeline::DBCFile& deathThuds) {
    assert(deathThuds.getFieldCount() >= 5);
    deathThuds_.clear();
    for (uint32_t r = 0; r < deathThuds.getRecordCount(); ++r) {
        const uint32_t sizeClass = deathThuds.getUInt32(r, 1);
        const uint32_t terrainSoundId = deathThuds.getUInt32(r, 2);
        const uint32_t drySoundId = deathThuds.getUInt32(r, 3);
        const uint32_t waterSoundId = deathThuds.getUInt32(r, 4);
        deathThuds_[(uint64_t{sizeClass} << 32) | terrainSoundId] = {drySoundId, waterSoundId};
    }
}

void CreatureSoundIndex::loadMaterials(const pipeline::DBCFile& materials) {
    assert(materials.getFieldCount() >= 3);
    materialFoley_.clear();
    for (uint32_t r = 0; r < materials.getRecordCount(); ++r) {
        const uint32_t matId = materials.getUInt32(r, 0);
        const uint32_t foleySoundId = materials.getUInt32(r, 2);
        materialFoley_[matId] = foleySoundId;
    }
}

void CreatureSoundIndex::loadGameObjectDisplays(const pipeline::DBCFile& goDisplays) {
    assert(goDisplays.getFieldCount() >= 12);
    goDisplaySounds_.clear();
    for (uint32_t r = 0; r < goDisplays.getRecordCount(); ++r) {
        const uint32_t dispId = goDisplays.getUInt32(r, 0);
        std::vector<uint32_t> slots(10, 0u);
        bool anyNonZero = false;
        for (uint32_t s = 0; s < 10; ++s) {
            slots[s] = goDisplays.getUInt32(r, 2 + s);
            if (slots[s] != 0) anyNonZero = true;
        }
        if (anyNonZero) {
            goDisplaySounds_[dispId] = std::move(slots);
        }
    }
}

void CreatureSoundIndex::loadItemDisplayArmorMaterials(const pipeline::DBCFile& itemDisplays) {
    if (itemDisplays.getFieldCount() < 21) return;
    itemDisplayArmorMat_.clear();
    for (uint32_t r = 0; r < itemDisplays.getRecordCount(); ++r) {
        const uint32_t dispId = itemDisplays.getUInt32(r, 0);
        uint32_t mat = 0;
        const uint32_t groupSound = itemDisplays.getUInt32(r, 12);
        if (groupSound == 9) {
            mat = 6; // Plate
        } else if (groupSound == 10 || groupSound == 11) {
            mat = 5; // Chain/Mail
        }
        if (mat == 0) {
            for (uint32_t col : {18u, 19u, 20u}) {
                const std::string tex = itemDisplays.getString(r, col);
                if (tex.empty()) continue;
                if (containsTokenCI(tex, "plate")) { mat = 6; break; }
                if (containsTokenCI(tex, "mail") || containsTokenCI(tex, "chain")) { mat = 5; break; }
                if (containsTokenCI(tex, "leather")) { mat = 8; break; }
            }
        }
        if (mat != 0) {
            itemDisplayArmorMat_[dispId] = mat;
        }
    }
}

uint32_t CreatureSoundIndex::soundDataForDisplay(uint32_t displayId) const {
    const auto it = displaySound_.find(displayId);
    return it != displaySound_.end() ? it->second : 0;
}

uint32_t CreatureSoundIndex::sizeClassForDisplay(uint32_t displayId) const {
    const auto it = displaySizeClass_.find(displayId);
    return it != displaySizeClass_.end() ? std::min<uint32_t>(it->second, 4u) : 1u;
}

uint32_t CreatureSoundIndex::sound(uint32_t soundDataId, CreatureSoundColumn column) const {
    const auto it = soundData_.find(soundDataId);
    if (it == soundData_.end()) return 0;
    const auto c = static_cast<uint32_t>(column);
    if (c >= it->second.size()) return 0;
    return it->second[c];
}

uint32_t CreatureSoundIndex::footstepSound(uint32_t displayId, uint32_t terrainTypeId,
                                           bool splash, bool bareFeet) const {
    uint32_t footstep = sound(soundDataForDisplay(displayId), CreatureSoundColumn::Footstep);
    // 306 of 799 sound sets leave the column 0. FootstepTerrainLookup does
    // carry footstep id 0 (AncientProtectorStep on every terrain), but that
    // many creatures stomping like an Ancient is not plausible; 0 is taken as
    // no footstep until a native capture says otherwise.
    if (footstep == 0) return 0;
    if (bareFeet && footstep == 7) {
        // Playable medium/large races (FootstepID 7 = CharacterMediumLarge)
        // drop to softer light/bare steps (FootstepID 8 = CharacterSmall) when
        // no boots are equipped.
        footstep = 8;
    }
    const auto terrain = terrainSound_.find(terrainTypeId);
    if (terrain == terrainSound_.end()) return 0;
    const auto it = footsteps_.find((uint64_t{footstep} << 32) | terrain->second);
    if (it == footsteps_.end()) return 0;
    return splash ? it->second.second : it->second.first;
}

uint32_t CreatureSoundIndex::deathThudSoundBySize(uint32_t sizeClass, uint32_t terrainTypeId,
                                                  bool inWater) const {
    const uint32_t clampedSize = std::min<uint32_t>(sizeClass, 4u);
    uint32_t terrainSoundId = 1u; // Dirt fallback when no terrain tile is loaded
    if (const auto tIt = terrainSound_.find(terrainTypeId); tIt != terrainSound_.end()) {
        terrainSoundId = tIt->second;
    }
    const auto it = deathThuds_.find((uint64_t{clampedSize} << 32) | terrainSoundId);
    if (it == deathThuds_.end()) return 0;
    if (inWater) {
        if (it->second.second != 0) return it->second.second;
        if (const auto dirtIt = deathThuds_.find((uint64_t{clampedSize} << 32) | 1u);
            dirtIt != deathThuds_.end() && dirtIt->second.second != 0) {
            return dirtIt->second.second;
        }
    }
    return it->second.first;
}

uint32_t CreatureSoundIndex::deathThudSound(uint32_t displayId, uint32_t terrainTypeId,
                                            bool inWater) const {
    if (displayId == 0) return 0;
    return deathThudSoundBySize(sizeClassForDisplay(displayId), terrainTypeId, inWater);
}

uint32_t CreatureSoundIndex::armorFoleySound(uint32_t materialId) const {
    if (materialId == 0) return 0;
    const auto it = materialFoley_.find(materialId);
    return it != materialFoley_.end() ? it->second : 0;
}

uint32_t CreatureSoundIndex::armorMaterialForItemSubclass(uint32_t itemClass,
                                                          uint32_t itemSubClass) noexcept {
    if (itemClass != 4) return 0; // ITEM_CLASS_ARMOR
    switch (itemSubClass) {
        case 2: return 8; // Leather -> Material 8 (FoleySoundLeather 1003)
        case 3: return 5; // Mail/Chain -> Material 5 (FoleySoundChain 1005)
        case 4: return 6; // Plate -> Material 6 (FoleySoundPlate 1004)
        default: return 0; // Cloth / Misc -> silent
    }
}

uint32_t CreatureSoundIndex::armorMaterialForDisplayInfo(uint32_t itemDisplayInfoId) const {
    if (itemDisplayInfoId == 0) return 0;
    const auto it = itemDisplayArmorMat_.find(itemDisplayInfoId);
    return it != itemDisplayArmorMat_.end() ? it->second : 0;
}

uint32_t CreatureSoundIndex::gameObjectSound(uint32_t goDisplayId, uint32_t slotIndex) const {
    if (goDisplayId == 0 || slotIndex >= 10) return 0;
    const auto it = goDisplaySounds_.find(goDisplayId);
    if (it == goDisplaySounds_.end() || slotIndex >= it->second.size()) return 0;
    return it->second[slotIndex];
}

uint32_t CreatureSoundIndex::customAttackSound(uint32_t displayId, uint32_t eventData) const {
    if (eventData >= 5) {
        // Playable character M2s author direct SoundEntries IDs in $CSD event.data
        // (e.g. 6576 ClapSounds, 6919 Chicken, 6921 Cry).
        return eventData;
    }
    if (eventData <= 3) {
        const auto col = static_cast<CreatureSoundColumn>(
            static_cast<uint32_t>(CreatureSoundColumn::CustomAttack0) + eventData);
        return sound(soundDataForDisplay(displayId), col);
    }
    return 0;
}

} // namespace audio
} // namespace wowee
