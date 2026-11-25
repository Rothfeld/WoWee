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
    InjuryCrushingBlow = 5,
    Death = 6,
    Stun = 7,
    Stand = 8,
    Footstep = 9,
    Aggro = 10,
    WingFlap = 11,
    WingGlide = 12,
    Alert = 13,
    Fidget0 = 14,
    Fidget1 = 15,
    Fidget2 = 16,
    Fidget3 = 17,
    Fidget4 = 18,
    CustomAttack0 = 19,
    CustomAttack1 = 20,
    CustomAttack2 = 21,
    CustomAttack3 = 22,
    NPCSound = 23,
    LoopSound = 24,
    CreatureImpactType = 25,
    JumpStart = 26,
    JumpEnd = 27,
    PetAttack = 28,
    PetOrder = 29,
    PetDismiss = 30,
    Birth = 33,
    SpellCastDirected = 34,
    Submerge = 35,
    Submerged = 36,
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

/// Display -> CreatureSoundData row, FootstepTerrainLookup, DeathThudLookups,
/// Material foley, and GameObjectDisplayInfo sound slots, all from DBC rows.
class CreatureSoundIndex {
public:
    bool load(pipeline::AssetManager& assets);
    /// Same, from DBC files already open (tests).
    void load(const pipeline::DBCFile& displayInfo, const pipeline::DBCFile& modelData,
              const pipeline::DBCFile& soundData, const pipeline::DBCFile& terrainLookup,
              const pipeline::DBCFile& terrainType);
    void loadDeathThudLookups(const pipeline::DBCFile& deathThuds);
    void loadMaterials(const pipeline::DBCFile& materials);
    void loadGameObjectDisplays(const pipeline::DBCFile& goDisplays);
    void loadItemDisplayArmorMaterials(const pipeline::DBCFile& itemDisplays);

    [[nodiscard]] uint32_t soundDataForDisplay(uint32_t displayId) const;
    /// CreatureModelData.SizeClass (col 3, 0..4) for a display; defaults to 1 (Medium).
    [[nodiscard]] uint32_t sizeClassForDisplay(uint32_t displayId) const;
    /// SoundEntries id in a column of a CreatureSoundData row; 0 = none.
    [[nodiscard]] uint32_t sound(uint32_t soundDataId, CreatureSoundColumn column) const;
    /// SoundEntries id of a footstep on TerrainType `terrainTypeId`; splash
    /// when the foot lands in shallow water. When `bareFeet` is true on a
    /// medium/large character profile (FootstepID 7), falls back to soft bare
    /// steps (FootstepID 8). 0 = no authored sound.
    [[nodiscard]] uint32_t footstepSound(uint32_t displayId, uint32_t terrainTypeId,
                                         bool splash, bool bareFeet = false) const;
    /// DeathThudLookups.dbc (45 rows): SizeClass x TerrainTypeSoundID -> dry / water SoundEntries id.
    [[nodiscard]] uint32_t deathThudSound(uint32_t displayId, uint32_t terrainTypeId, bool inWater) const;
    [[nodiscard]] uint32_t deathThudSoundBySize(uint32_t sizeClass, uint32_t terrainTypeId, bool inWater) const;
    /// Material.dbc col 2 FoleySoundID (5->1005 Chain, 6->1004 Plate, 8->1003 Leather, Cloth->0).
    [[nodiscard]] uint32_t armorFoleySound(uint32_t materialId) const;
    /// Map ItemClass=4 (Armor) SubClass (1=Cloth, 2=Leather, 3=Mail, 4=Plate) -> Material.dbc ID.
    [[nodiscard]] static uint32_t armorMaterialForItemSubclass(uint32_t itemClass, uint32_t itemSubClass) noexcept;
    /// Fallback armor Material.dbc ID inferred from ItemDisplayInfo.dbc (ItemGroupSoundsID / torso-leg textures).
    [[nodiscard]] uint32_t armorMaterialForDisplayInfo(uint32_t itemDisplayInfoId) const;
    /// GameObjectDisplayInfo.dbc sound slot (0..5 = $GO0..5 cols 2..7; 6..9 = $GC0..3 cols 8..11).
    [[nodiscard]] uint32_t gameObjectSound(uint32_t goDisplayId, uint32_t slotIndex) const;
    /// M2 $CSD resolution: eventData >= 5 is a direct SoundEntries ID (character emotes),
    /// eventData 0..3 indexes CreatureSoundData CustomAttack0..3 (cols 19..22).
    [[nodiscard]] uint32_t customAttackSound(uint32_t displayId, uint32_t eventData) const;

    [[nodiscard]] size_t deathThudRowCount() const noexcept { return deathThuds_.size(); }
    [[nodiscard]] size_t materialCount() const noexcept { return materialFoley_.size(); }
    [[nodiscard]] size_t gameObjectDisplayCount() const noexcept { return goDisplaySounds_.size(); }

private:
    std::unordered_map<uint32_t, uint32_t> displaySound_;      // display -> sound data
    std::unordered_map<uint32_t, uint32_t> displaySizeClass_;  // display -> CreatureModelData SizeClass (col 3)
    std::unordered_map<uint32_t, std::vector<uint32_t>> soundData_;
    std::unordered_map<uint32_t, uint32_t> terrainSound_;      // TerrainType id -> SoundID
    std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> footsteps_;  // (footstep, terrain sound) -> (dry, splash)
    std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> deathThuds_; // (sizeClass, terrain sound) -> (dry, water)
    std::unordered_map<uint32_t, uint32_t> materialFoley_;     // Material.dbc ID -> FoleySoundID (col 2)
    std::unordered_map<uint32_t, std::vector<uint32_t>> goDisplaySounds_; // GameObjectDisplayInfo ID -> 10 slots (cols 2..11)
    std::unordered_map<uint32_t, uint32_t> itemDisplayArmorMat_; // ItemDisplayInfo ID -> Material.dbc ID (5/6/8)
};

} // namespace audio
} // namespace wowee
