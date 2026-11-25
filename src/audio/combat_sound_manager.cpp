#include "audio/combat_sound_manager.hpp"
#include "audio/sound_entries.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "core/logger.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace wowee {
namespace audio {

namespace {

// Wow.exe 0x467020: underwater weapon swing pitch multiplier at 0x88d620.
constexpr float kUnderwaterSwingPitch = 0.5f;

// Timed fallback delays when a unit has no M2 animation keyframe driver
// (e.g. headless replay tests or before M2 model load) vs safety timeout
// when a unit is registered as keyframe-driven.
constexpr float kHeadlessSwingDelaySec = 0.08f;
constexpr float kHeadlessHitDelaySec = 0.20f;
constexpr float kKeyframeSafetySwingDelaySec = 0.55f;
constexpr float kKeyframeSafetyHitDelaySec = 0.85f;
constexpr float kUnqueuedSwingGraceSec = 0.35f;
constexpr size_t kMaxQueuedEventsPerUnit = 2;
constexpr size_t kMaxTotalQueuedCombatEvents = 24;

// TBC 2.4.3 VictimState values (CMaNGOS Unit.h / Wow.exe 0x62f230 & 0x62f600)
constexpr uint32_t kVictimStateDodge = 2;
constexpr uint32_t kVictimStateParry = 3;
constexpr uint32_t kVictimStateBlocks = 5;
constexpr uint32_t kVictimStateEvades = 6;
constexpr uint32_t kVictimStateImmune = 7;
constexpr uint32_t kVictimStateDeflects = 8;

std::string lowerAscii(std::string s) {
    for (char& ch : s) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return s;
}

bool containsToken(const std::string& haystackLower, const char* needleLower) {
    return haystackLower.find(needleLower) != std::string::npos;
}

} // namespace

bool CombatSoundManager::initialize(pipeline::AssetManager* assets) {
    assets_ = assets;
    weaponDbcBuilt_ = false;
    weaponSizeBySubClass_.clear();
    weaponImpactRows_.clear();
    sheatheRows_.clear();
    materials_.clear();
    creatureSoundById_.clear();
    displayToCreatureSoundId_.clear();
    itemDisplayHints_.clear();
    itemDbcEntries_.clear();
    pendingByAttacker_.clear();
    recentUnqueuedSwingTimer_.clear();

    if (!assets_) {
        LOG_ERROR("CombatSoundManager: AssetManager is null");
        initialized_ = false;
        return false;
    }

    ensureWeaponDbcLoaded();
    initialized_ = true;
    LOG_INFO("CombatSoundManager: initialized (", weaponImpactRows_.size(),
             " WeaponImpactSounds rows, ", sheatheRows_.size(),
             " SheatheSoundLookups rows, ", materials_.size(), " Material rows)");
    return true;
}

void CombatSoundManager::shutdown() {
    pendingByAttacker_.clear();
    recentUnqueuedSwingTimer_.clear();
    underwaterUnits_.clear();
    keyframeDrivenUnits_.clear();
    weaponSizeBySubClass_.clear();
    weaponImpactRows_.clear();
    sheatheRows_.clear();
    materials_.clear();
    creatureSoundById_.clear();
    displayToCreatureSoundId_.clear();
    itemDisplayHints_.clear();
    itemDbcEntries_.clear();
    weaponDbcBuilt_ = false;
    initialized_ = false;
    assets_ = nullptr;
}

void CombatSoundManager::setVolumeScale(float scale) {
    volumeScale_ = std::clamp(scale, 0.0f, 1.0f);
}

void CombatSoundManager::ensureWeaponDbcLoaded() const {
    if (weaponDbcBuilt_ || !assets_ || !assets_->isInitialized()) {
        return;
    }
    weaponDbcBuilt_ = true;

    // 1. WeaponSwingSounds2.dbc: 0=ID, 1=WeaponSize(0..2), 2=Crit(0..1), 3=SoundID (233..238)
    if (auto swingDbc = assets_->loadDBC("WeaponSwingSounds2.dbc");
        swingDbc && swingDbc->isLoaded() && swingDbc->getFieldCount() >= 4) {
        for (uint32_t r = 0; r < swingDbc->getRecordCount(); ++r) {
            const uint32_t wsize = swingDbc->getUInt32(r, 1);
            const uint32_t crit = swingDbc->getUInt32(r, 2) != 0 ? 1u : 0u;
            const uint32_t sid = swingDbc->getUInt32(r, 3);
            if (wsize < 3u && sid != 0u) {
                swingSoundIds_[wsize][crit] = sid;
            }
        }
    }

    // 2. ItemSubClass.dbc: 0=ClassID, 1=SubClassID, 9=WeaponSwingSize (0=Small, 1=Medium, 2=Large)
    if (auto subDbc = assets_->loadDBC("ItemSubClass.dbc");
        subDbc && subDbc->isLoaded() && subDbc->getFieldCount() >= 10) {
        for (uint32_t r = 0; r < subDbc->getRecordCount(); ++r) {
            if (subDbc->getUInt32(r, 0) != 2u) continue;
            const uint32_t subClass = subDbc->getUInt32(r, 1);
            const uint32_t swingSize = subDbc->getUInt32(r, 9);
            WeaponSize ws = WeaponSize::MEDIUM;
            if (swingSize == 0u) ws = WeaponSize::SMALL;
            else if (swingSize >= 2u) ws = WeaponSize::LARGE;
            weaponSizeBySubClass_[subClass] = ws;
        }
    }

    // 3. WeaponImpactSounds.dbc: 0=ID, 1=WeaponSubClass, 2=ParrySoundType(0=wood/blunt,1=metal/pierce),
    //    3..12 = normal (flesh, chain, plate, shield_metal, shield_wood, parry_metal_metal,
    //                    parry_metal_wood, wood, stone, ethereal)
    //    13..22 = crit  (same 10 categories)
    if (auto impDbc = assets_->loadDBC("WeaponImpactSounds.dbc");
        impDbc && impDbc->isLoaded() && impDbc->getFieldCount() >= 23) {
        for (uint32_t r = 0; r < impDbc->getRecordCount(); ++r) {
            const uint32_t subClass = impDbc->getUInt32(r, 1);
            const uint32_t parryType = impDbc->getUInt32(r, 2) != 0 ? 1u : 0u;
            WeaponImpactRow row;
            for (size_t i = 0; i < 10; ++i) {
                row.normal[i] = impDbc->getUInt32(r, static_cast<uint32_t>(3 + i));
                row.crit[i] = impDbc->getUInt32(r, static_cast<uint32_t>(13 + i));
            }
            const uint64_t key = (static_cast<uint64_t>(subClass) << 1u) | parryType;
            weaponImpactRows_[key] = row;
        }
    }

    // 4. Material.dbc: 0=ID, 1=Flags, 2=FoleySoundID, 3=SheatheSoundID, 4=UnsheatheSoundID
    if (auto matDbc = assets_->loadDBC("Material.dbc");
        matDbc && matDbc->isLoaded() && matDbc->getFieldCount() >= 5) {
        for (uint32_t r = 0; r < matDbc->getRecordCount(); ++r) {
            const int32_t matId = matDbc->getInt32(r, 0);
            if (matId <= 0) continue;
            MaterialRecord rec;
            rec.flags = matDbc->getUInt32(r, 1);
            rec.foleySoundId = matDbc->getUInt32(r, 2);
            rec.sheatheSoundId = matDbc->getUInt32(r, 3);
            rec.unsheatheSoundId = matDbc->getUInt32(r, 4);
            materials_[matId] = rec;
        }
    }

    // 5. SheatheSoundLookups.dbc: 0=ID, 1=ItemClass, 2=ItemSubClass, 3=ItemEnvTypes,
    //    4=IsNotShield, 5=SheatheSoundID, 6=UnsheatheSoundID
    if (auto shDbc = assets_->loadDBC("SheatheSoundLookups.dbc");
        shDbc && shDbc->isLoaded() && shDbc->getFieldCount() >= 7) {
        sheatheRows_.reserve(shDbc->getRecordCount());
        for (uint32_t r = 0; r < shDbc->getRecordCount(); ++r) {
            SheatheLookupRecord rec;
            rec.itemClass = shDbc->getUInt32(r, 1);
            rec.itemSubClass = shDbc->getUInt32(r, 2);
            rec.itemEnvTypes = shDbc->getInt32(r, 3);
            rec.notShieldFlag = shDbc->getUInt32(r, 4);
            rec.sheatheSoundId = shDbc->getUInt32(r, 5);
            rec.unsheatheSoundId = shDbc->getUInt32(r, 6);
            sheatheRows_.push_back(rec);
        }
    }

    // 6. CreatureSoundData.dbc + CreatureModelData.dbc + CreatureDisplayInfo.dbc
    if (auto csdDbc = assets_->loadDBC("CreatureSoundData.dbc");
        csdDbc && csdDbc->isLoaded() && csdDbc->getFieldCount() >= 26) {
        creatureSoundById_.reserve(csdDbc->getRecordCount());
        for (uint32_t r = 0; r < csdDbc->getRecordCount(); ++r) {
            const uint32_t id = csdDbc->getUInt32(r, 0);
            if (id == 0) continue;
            CreatureCombatSoundRecord rec;
            rec.exertion = csdDbc->getUInt32(r, 1);
            rec.exertionCrit = csdDbc->getUInt32(r, 2);
            rec.injury = csdDbc->getUInt32(r, 3);
            rec.injuryCrit = csdDbc->getUInt32(r, 4);
            rec.injuryCrushing = csdDbc->getUInt32(r, 5);
            rec.creatureImpactEnum = csdDbc->getUInt32(r, 25);
            creatureSoundById_[id] = rec;
        }
    }

    std::unordered_map<uint32_t, uint32_t> modelToCsd;
    if (auto cmdDbc = assets_->loadDBC("CreatureModelData.dbc");
        cmdDbc && cmdDbc->isLoaded() && cmdDbc->getFieldCount() >= 14) {
        modelToCsd.reserve(cmdDbc->getRecordCount());
        for (uint32_t r = 0; r < cmdDbc->getRecordCount(); ++r) {
            const uint32_t modelId = cmdDbc->getUInt32(r, 0);
            const uint32_t csdId = cmdDbc->getUInt32(r, 13);
            if (modelId != 0 && csdId != 0) {
                modelToCsd[modelId] = csdId;
            }
        }
    }

    if (auto cdiDbc = assets_->loadDBC("CreatureDisplayInfo.dbc");
        cdiDbc && cdiDbc->isLoaded() && cdiDbc->getFieldCount() >= 13) {
        displayToCreatureSoundId_.reserve(cdiDbc->getRecordCount());
        for (uint32_t r = 0; r < cdiDbc->getRecordCount(); ++r) {
            const uint32_t dispId = cdiDbc->getUInt32(r, 0);
            if (dispId == 0) continue;
            const uint32_t modelId = cdiDbc->getUInt32(r, 1);
            const uint32_t overrideCsd = cdiDbc->getUInt32(r, 12);
            uint32_t effectiveCsd = overrideCsd;
            if (effectiveCsd == 0) {
                auto it = modelToCsd.find(modelId);
                if (it != modelToCsd.end()) effectiveCsd = it->second;
            }
            if (effectiveCsd != 0) {
                displayToCreatureSoundId_[dispId] = effectiveCsd;
            }
        }
    }

    // 7. ItemDisplayInfo.dbc hints for remote players whose visible slots carry displayInfoIds
    if (auto idiDbc = assets_->loadDBC("ItemDisplayInfo.dbc");
        idiDbc && idiDbc->isLoaded() && idiDbc->getFieldCount() >= 13) {
        itemDisplayHints_.reserve(idiDbc->getRecordCount());
        for (uint32_t r = 0; r < idiDbc->getRecordCount(); ++r) {
            const uint32_t id = idiDbc->getUInt32(r, 0);
            if (id == 0) continue;
            ItemDisplayHint hint;
            hint.leftModel = lowerAscii(idiDbc->getString(r, 1));
            hint.icon = lowerAscii(idiDbc->getString(r, 5));
            hint.groupSoundId = idiDbc->getUInt32(r, 12);
            itemDisplayHints_.emplace(id, std::move(hint));
        }
    }

    // 7b. Item.dbc: 0=ID, 1=DisplayInfoID, 2=InventoryType (resolves equipped itemId
    //     on frame 1 before CMSG_ITEM_QUERY_SINGLE responses arrive from the server)
    if (auto itemDbc = assets_->loadDBC("Item.dbc");
        itemDbc && itemDbc->isLoaded() && itemDbc->getFieldCount() >= 3) {
        itemDbcEntries_.reserve(itemDbc->getRecordCount());
        for (uint32_t r = 0; r < itemDbc->getRecordCount(); ++r) {
            const uint32_t id = itemDbc->getUInt32(r, 0);
            if (id == 0) continue;
            ItemDbcEntry entry;
            entry.displayInfoId = itemDbc->getUInt32(r, 1);
            entry.inventoryType = static_cast<uint8_t>(itemDbc->getUInt32(r, 2) & 0xFFu);
            itemDbcEntries_.emplace(id, entry);
        }
    }

    // 8. Named lookups in SoundEntries.dbc (Wow.exe 0x8975e8, 0x8975cc, 0x8c8004)
    auto& se = SoundEntryTable::instance();
    if (const auto* m1 = se.findByName(assets_, "(DONOTRENAME)Combat Miss 1H")) {
        miss1HSoundId_ = m1->id;
    }
    if (const auto* m2 = se.findByName(assets_, "(DONOTRENAME)Combat Miss 2H")) {
        miss2HSoundId_ = m2->id;
    }
    if (const auto* defl = se.findByName(assets_, "(DONOTRENAME)ShieldWoodImpact")) {
        deflectSoundId_ = defl->id;
    }
}

CombatSoundManager::WeaponSize CombatSoundManager::getWeaponSizeForSubclass(
        uint32_t weaponSubClass) const {
    ensureWeaponDbcLoaded();
    auto it = weaponSizeBySubClass_.find(weaponSubClass);
    if (it != weaponSizeBySubClass_.end()) {
        return it->second;
    }
    // Fallback matching TBC ItemSubClass.dbc col 9
    switch (weaponSubClass) {
        case 13: // Fist / Unarmed
        case 15: // Dagger
            return WeaponSize::SMALL;
        case 1:  // 2H Axe
        case 5:  // 2H Mace
        case 6:  // Polearm
        case 8:  // 2H Sword
        case 10: // Staff
        case 12: // 2H Exotic
        case 17: // Spear
        case 20: // Fishing Pole
            return WeaponSize::LARGE;
        default:
            return WeaponSize::MEDIUM;
    }
}

bool CombatSoundManager::isTwoHandWeaponSubclass(uint32_t weaponSubClass) {
    // Matches Wow.exe 0x603bd0 switch over weapon subclass for 2H Combat Miss (7081 vs 7080)
    switch (weaponSubClass) {
        case 1:  // 2H Axe
        case 5:  // 2H Mace
        case 6:  // Polearm
        case 8:  // 2H Sword
        case 10: // Staff
        case 12: // 2H Exotic
        case 17: // Spear
        case 20: // Fishing Pole
            return true;
        default:
            return false;
    }
}

uint32_t CombatSoundManager::getWeaponSwingSoundId(WeaponSize size, bool isCrit) const {
    ensureWeaponDbcLoaded();
    const size_t sIdx = std::min<size_t>(static_cast<size_t>(size), 2u);
    const size_t cIdx = isCrit ? 1u : 0u;
    return swingSoundIds_[sIdx][cIdx];
}

uint32_t CombatSoundManager::getWeaponImpactSoundId(
        uint32_t weaponSubClass, bool metalWeapon, ImpactType impactType, bool isCrit) const {
    ensureWeaponDbcLoaded();
    const size_t catIdx = std::min<size_t>(static_cast<size_t>(impactType), 9u);
    const uint64_t primaryKey = (static_cast<uint64_t>(weaponSubClass) << 1u) | (metalWeapon ? 1u : 0u);
    const uint64_t altKey = (static_cast<uint64_t>(weaponSubClass) << 1u) | (metalWeapon ? 0u : 1u);
    const uint64_t unarmedKey = (13ull << 1u);

    auto pickFromRow = [&](const WeaponImpactRow& row) -> uint32_t {
        if (isCrit && row.crit[catIdx] != 0u) {
            return row.crit[catIdx];
        }
        if (row.normal[catIdx] != 0u) {
            return row.normal[catIdx];
        }
        // Fallback within the same row if a specific surface (e.g. bow parry/wood) is 0 in DBC
        if (catIdx == static_cast<size_t>(ImpactType::PARRY_METAL_METAL) &&
            row.normal[static_cast<size_t>(ImpactType::PARRY_METAL_WOOD)] != 0u) {
            return row.normal[static_cast<size_t>(ImpactType::PARRY_METAL_WOOD)];
        }
        return isCrit && row.crit[0] != 0u ? row.crit[0] : row.normal[0];
    };

    if (auto it = weaponImpactRows_.find(primaryKey); it != weaponImpactRows_.end()) {
        if (const uint32_t sid = pickFromRow(it->second); sid != 0u) return sid;
    }
    if (auto it = weaponImpactRows_.find(altKey); it != weaponImpactRows_.end()) {
        if (const uint32_t sid = pickFromRow(it->second); sid != 0u) return sid;
    }
    if (auto it = weaponImpactRows_.find(unarmedKey); it != weaponImpactRows_.end()) {
        return pickFromRow(it->second);
    }
    return 0u;
}

uint32_t CombatSoundManager::getWeaponMissSoundId(bool twoHanded) const {
    ensureWeaponDbcLoaded();
    return twoHanded ? miss2HSoundId_ : miss1HSoundId_;
}

uint32_t CombatSoundManager::getDeflectSoundId() const {
    ensureWeaponDbcLoaded();
    return deflectSoundId_;
}

uint32_t CombatSoundManager::getBowPullSoundId(uint32_t rangedSubClass) const {
    // Wow.exe 0x62fbc0 ($BWP): Bow (2) / Crossbow (18) -> 1144, Gun (3) -> 1147
    switch (rangedSubClass) {
        case 2:
        case 18:
            return 1144u;
        case 3:
            return 1147u;
        default:
            return 0u;
    }
}

uint32_t CombatSoundManager::getBowReleaseSoundId(uint32_t rangedSubClass) const {
    // Wow.exe 0x61c254 ($BWR): Bow/Crossbow -> 1146, Gun -> 1148, Thrown -> 7140, Wand -> 5414
    switch (rangedSubClass) {
        case 2:
        case 18:
            return 1146u;
        case 3:
            return 1148u;
        case 16:
            return 7140u;
        case 19:
            return 5414u;
        default:
            return 0u;
    }
}

uint32_t CombatSoundManager::getMaterialSheatheSoundId(int32_t material, bool unsheathe) const {
    ensureWeaponDbcLoaded();
    auto it = materials_.find(material);
    if (it == materials_.end()) return 0u;
    return unsheathe ? it->second.unsheatheSoundId : it->second.sheatheSoundId;
}

uint32_t CombatSoundManager::getSheatheSoundId(
        uint32_t itemClass, uint32_t itemSubClass, int32_t material,
        bool isShield, bool unsheathe) const {
    ensureWeaponDbcLoaded();

    // 1. Direct Material.dbc lookup (Wow.exe 0x467080: Material 7 Tome -> 11597/11598,
    //    Material 6 Shield/Plate -> 696/701, Material 1 Metal -> 698/700, Material 2 Wood -> 697/699).
    //    For shields (where an item's material may be 1=Metal or 2=Wood rather than 6),
    //    SheatheSoundLookups.dbc (class=4, subClass=5/6, notShieldFlag=0) maps shields to 696/701.
    if (!isShield && material > 0) {
        if (const uint32_t matSid = getMaterialSheatheSoundId(material, unsheathe); matSid != 0u) {
            return matSid;
        }
    }

    // 2. SheatheSoundLookups.dbc (33 rows)
    const uint32_t wantedNotShield = isShield ? 0u : 1u;
    const int32_t envType = (material == 1 || material == 4 || material == 5 || material == 6) ? 1
                          : (material == 2) ? 2 : 0;

    for (const auto& r : sheatheRows_) {
        if (r.itemClass == itemClass && r.itemSubClass == itemSubClass &&
            r.notShieldFlag == wantedNotShield &&
            (isShield || r.itemEnvTypes == envType)) {
            const uint32_t sid = unsheathe ? r.unsheatheSoundId : r.sheatheSoundId;
            if (sid != 0u) return sid;
        }
    }

    // Fallback on any matching itemClass + notShieldFlag + envType in SheatheSoundLookups
    for (const auto& r : sheatheRows_) {
        if (r.itemClass == itemClass && r.notShieldFlag == wantedNotShield &&
            (isShield || r.itemEnvTypes == envType || r.itemEnvTypes == 0)) {
            const uint32_t sid = unsheathe ? r.unsheatheSoundId : r.sheatheSoundId;
            if (sid != 0u) return sid;
        }
    }

    // Final Material.dbc check (e.g. shield with explicit material 6 or 7)
    if (material > 0) {
        if (const uint32_t matSid = getMaterialSheatheSoundId(material, unsheathe); matSid != 0u) {
            return matSid;
        }
    }
    return 0u;
}

bool CombatSoundManager::isMetalMaterial(int32_t materialId) const {
    ensureWeaponDbcLoaded();
    // Wow.exe 0x5f7d30: tests (Material.dbc[materialId].Flags & 1) != 0
    auto it = materials_.find(materialId);
    if (it != materials_.end()) {
        return (it->second.flags & 1u) != 0u;
    }
    return materialId == 1 || materialId == 4 || materialId == 5 || materialId == 6;
}

CombatSoundManager::ImpactType CombatSoundManager::resolveArmorMaterialImpactType(
        int32_t chestMaterialId, uint32_t armorSubClass) const {
    ensureWeaponDbcLoaded();
    // Wow.exe CGPlayer_C::GetImpactType (0x639930):
    //   (Material.Flags & 2) != 0 -> PLATE (2)
    //   (Material.Flags & 4) != 0 -> CHAIN (1)
    //   otherwise                 -> FLESH (0)
    if (chestMaterialId > 0) {
        auto it = materials_.find(chestMaterialId);
        if (it != materials_.end()) {
            if ((it->second.flags & 2u) != 0u) return ImpactType::PLATE;
            if ((it->second.flags & 4u) != 0u) return ImpactType::CHAIN;
            return ImpactType::FLESH;
        }
    }
    if (armorSubClass == 4u) return ImpactType::PLATE;
    if (armorSubClass == 3u) return ImpactType::CHAIN;
    return ImpactType::FLESH;
}

CombatSoundManager::ImpactType CombatSoundManager::mapCreatureImpactEnum(
        uint32_t creatureImpactEnum) {
    // Wow.exe CGUnit_C::GetImpactType (0x626d30) lookup table at 0x8c76b4: (0, 8, 7, 9)
    switch (creatureImpactEnum) {
        case 0: return ImpactType::FLESH;    // 0
        case 1: return ImpactType::STONE;    // 8 (e.g. Infernal CSD 24, Stone Golem CSD 84)
        case 2: return ImpactType::WOOD;     // 7
        case 3: return ImpactType::ETHEREAL; // 9
        default: return ImpactType::FLESH;
    }
}

CombatSoundManager::ImpactType CombatSoundManager::getCreatureImpactTypeForSoundData(
        uint32_t creatureSoundDataId) const {
    ensureWeaponDbcLoaded();
    auto it = creatureSoundById_.find(creatureSoundDataId);
    if (it == creatureSoundById_.end()) return ImpactType::FLESH;
    return mapCreatureImpactEnum(it->second.creatureImpactEnum);
}

CombatSoundManager::ImpactType CombatSoundManager::getCreatureImpactTypeForDisplay(
        uint32_t displayId) const {
    ensureWeaponDbcLoaded();
    auto dit = displayToCreatureSoundId_.find(displayId);
    if (dit == displayToCreatureSoundId_.end()) return ImpactType::FLESH;
    return getCreatureImpactTypeForSoundData(dit->second);
}

uint32_t CombatSoundManager::getCreatureExertionSoundId(uint32_t displayId, bool isCrit) const {
    ensureWeaponDbcLoaded();
    auto dit = displayToCreatureSoundId_.find(displayId);
    if (dit == displayToCreatureSoundId_.end()) return 0u;
    auto cit = creatureSoundById_.find(dit->second);
    if (cit == creatureSoundById_.end()) return 0u;
    if (isCrit && cit->second.exertionCrit != 0u) return cit->second.exertionCrit;
    return cit->second.exertion;
}

uint32_t CombatSoundManager::getCreatureWoundSoundId(uint32_t displayId, bool isCrit) const {
    ensureWeaponDbcLoaded();
    auto dit = displayToCreatureSoundId_.find(displayId);
    if (dit == displayToCreatureSoundId_.end()) return 0u;
    auto cit = creatureSoundById_.find(dit->second);
    if (cit == creatureSoundById_.end()) return 0u;
    if (isCrit && cit->second.injuryCrit != 0u) return cit->second.injuryCrit;
    return cit->second.injury;
}

uint32_t CombatSoundManager::getCreatureCrushingWoundSoundId(uint32_t displayId) const {
    ensureWeaponDbcLoaded();
    auto dit = displayToCreatureSoundId_.find(displayId);
    if (dit == displayToCreatureSoundId_.end()) return 0u;
    auto cit = creatureSoundById_.find(dit->second);
    if (cit == creatureSoundById_.end()) return 0u;
    if (cit->second.injuryCrushing != 0u) return cit->second.injuryCrushing;
    if (cit->second.injuryCrit != 0u) return cit->second.injuryCrit;
    return cit->second.injury;
}

bool CombatSoundManager::playCreatureCrushingWound(
        uint32_t displayId, const glm::vec3& renderPos, bool isLocal) {
    const uint32_t sid = getCreatureCrushingWoundSoundId(displayId);
    if (sid == 0u) return false;
    return playCombatSound(sid, renderPos, isLocal, 1.0f);
}

CombatSoundManager::ResolvedWeaponAudio CombatSoundManager::resolveWeaponFromDisplayInfo(
        uint32_t displayInfoId, uint8_t inventoryType) const {
    ensureWeaponDbcLoaded();
    ResolvedWeaponAudio out;
    if (displayInfoId == 0u) {
        return out; // Unarmed (13, false)
    }

    const bool twoHand = (inventoryType == 17u);
    auto it = itemDisplayHints_.find(displayInfoId);
    if (it != itemDisplayHints_.end()) {
        const std::string& m = it->second.leftModel;
        const std::string& ic = it->second.icon;
        if (containsToken(m, "dagger") || containsToken(ic, "shortblade") || containsToken(ic, "knife")) {
            return {15u, true, 1};
        }
        if (containsToken(m, "staff") || containsToken(m, "stave") || containsToken(ic, "staff")) {
            return {10u, false, 2};
        }
        if (containsToken(m, "polearm") || containsToken(m, "halberd") || containsToken(ic, "polearm") ||
            containsToken(ic, "halberd") || containsToken(ic, "spear")) {
            return {6u, true, 1};
        }
        if (containsToken(m, "axe_2h") || (twoHand && (containsToken(m, "axe") || containsToken(ic, "axe")))) {
            return {1u, true, 1};
        }
        if (containsToken(m, "axe")) {
            return {0u, true, 1};
        }
        if (containsToken(m, "mace_2h") || (twoHand && (containsToken(m, "mace") || containsToken(ic, "hammer") || containsToken(ic, "mace")))) {
            const bool wood = containsToken(m, "club") || containsToken(m, "wood");
            return {5u, !wood, wood ? 2 : 1};
        }
        if (containsToken(m, "mace") || containsToken(ic, "hammer") || containsToken(ic, "mace")) {
            const bool wood = containsToken(m, "club") || containsToken(m, "wood");
            return {4u, !wood, wood ? 2 : 1};
        }
        if (containsToken(m, "sword_2h") || (twoHand && (containsToken(m, "sword") || containsToken(ic, "sword")))) {
            return {8u, true, 1};
        }
        if (containsToken(m, "sword") || containsToken(ic, "sword")) {
            return {7u, true, 1};
        }
        if (containsToken(m, "hand_") || containsToken(m, "fist") || containsToken(ic, "gauntlet") || containsToken(ic, "claw")) {
            return {13u, true, 1};
        }
        if (containsToken(m, "crossbow") || containsToken(ic, "crossbow")) {
            return {18u, false, 2};
        }
        if (containsToken(m, "bow") || containsToken(ic, "bow")) {
            return {2u, false, 2};
        }
        if (containsToken(m, "gun") || containsToken(m, "rifle") || containsToken(m, "firearm") ||
            containsToken(ic, "rifle") || containsToken(ic, "gun")) {
            return {3u, true, 1};
        }
        if (containsToken(m, "wand") || containsToken(ic, "wand")) {
            return {19u, false, 2};
        }
        if (containsToken(m, "thrown") || containsToken(ic, "throwing")) {
            return {16u, true, 1};
        }
    }

    switch (inventoryType) {
        case 15: return {2u, false, 2};   // RANGED_BOW
        case 17: return {8u, true, 1};    // TWO_HAND -> 2H Sword
        case 25: return {16u, true, 1};   // THROWN
        case 26: return {3u, true, 1};    // RANGED_GUN
        case 13:
        case 21:
        case 22:
            return {7u, true, 1};         // 1H Sword
        default:
            return out;
    }
}

CombatSoundManager::ResolvedWeaponAudio CombatSoundManager::resolveWeaponFromItemId(
        uint32_t itemId) const {
    ensureWeaponDbcLoaded();
    if (itemId == 0u) return ResolvedWeaponAudio{};
    auto it = itemDbcEntries_.find(itemId);
    if (it == itemDbcEntries_.end()) return ResolvedWeaponAudio{};
    return resolveWeaponFromDisplayInfo(it->second.displayInfoId, it->second.inventoryType);
}

CombatSoundManager::ImpactType CombatSoundManager::resolveChestDisplayImpactType(
        uint32_t displayInfoId) const {
    ensureWeaponDbcLoaded();
    if (displayInfoId == 0u) return ImpactType::FLESH;
    auto it = itemDisplayHints_.find(displayInfoId);
    if (it == itemDisplayHints_.end()) return ImpactType::FLESH;
    const std::string& ic = it->second.icon;
    if (containsToken(ic, "plate")) return ImpactType::PLATE;
    if (containsToken(ic, "chain") || containsToken(ic, "mail")) return ImpactType::CHAIN;
    return ImpactType::FLESH;
}

bool CombatSoundManager::resolveShieldDisplayIsMetal(uint32_t displayInfoId) const {
    ensureWeaponDbcLoaded();
    if (displayInfoId == 0u) return true;
    auto it = itemDisplayHints_.find(displayInfoId);
    if (it == itemDisplayHints_.end()) return true;
    if (containsToken(it->second.leftModel, "wood") || containsToken(it->second.icon, "wood")) {
        return false;
    }
    return true;
}

CombatSoundManager::CombatOutcome CombatSoundManager::classifyMeleeOutcome(
        bool isMiss, bool isCrit, bool isCrushing, bool isGlancing,
        uint32_t victimState, int32_t totalDamage, uint32_t blockedAmount) {
    // Evade and Deflect are checked before isMiss (server sets HITINFO_MISS on EVADE).
    if (victimState == kVictimStateEvades || victimState == kVictimStateDeflects) {
        return CombatOutcome::EVADE_DEFLECT;
    }
    if (victimState == kVictimStateImmune) {
        return CombatOutcome::IMMUNE_OR_ABSORBED;
    }
    if (isMiss) {
        return CombatOutcome::MISS;
    }
    if (victimState == kVictimStateDodge) {
        return CombatOutcome::DODGE;
    }
    if (victimState == kVictimStateParry) {
        return CombatOutcome::PARRY;
    }
    if (victimState == kVictimStateBlocks || (totalDamage <= 0 && blockedAmount > 0)) {
        return CombatOutcome::BLOCK;
    }
    if (totalDamage <= 0) {
        return CombatOutcome::IMMUNE_OR_ABSORBED;
    }
    if (isCrit) {
        return CombatOutcome::CRIT;
    }
    if (isCrushing) {
        return CombatOutcome::CRUSHING;
    }
    if (isGlancing) {
        return CombatOutcome::GLANCING;
    }
    return CombatOutcome::HIT;
}

void CombatSoundManager::setUnitUnderwater(uint64_t unitGuid, bool underwater) {
    if (unitGuid == 0u) return;
    if (underwater) underwaterUnits_.insert(unitGuid);
    else underwaterUnits_.erase(unitGuid);
}

bool CombatSoundManager::isUnitUnderwater(uint64_t unitGuid) const {
    return unitGuid != 0u && underwaterUnits_.contains(unitGuid);
}

void CombatSoundManager::setUnitHasKeyframeDriver(uint64_t unitGuid, bool hasDriver) {
    if (unitGuid == 0u) return;
    if (hasDriver) keyframeDrivenUnits_.insert(unitGuid);
    else keyframeDrivenUnits_.erase(unitGuid);
}

bool CombatSoundManager::unitHasKeyframeDriver(uint64_t unitGuid) const {
    return unitGuid != 0u && keyframeDrivenUnits_.contains(unitGuid);
}

bool CombatSoundManager::playCombatSound(
        uint32_t soundId, const glm::vec3& renderPos, bool isLocal, float pitch,
        float overrideEntryVolume) const {
    if (soundId == 0u || !assets_ || volumeScale_ <= 0.0f) return false;
    auto& se = SoundEntryTable::instance();
    float effScale = volumeScale_;
    if (overrideEntryVolume > 0.0f) {
        if (const auto* rec = se.findById(assets_, soundId); rec && rec->volume > 0.0f) {
            effScale = volumeScale_ * (overrideEntryVolume / rec->volume);
        }
    }
    // Native Wow.exe plays unit melee swings, hits, misses, bow pull/release, and
    // sheathe/unsheathe as 3D positional sounds even for the local player. Keep 2D
    // only for unit-less legacy callers passing a zero position.
    const bool hasWorldPos = (renderPos.x != 0.0f || renderPos.y != 0.0f || renderPos.z != 0.0f);
    if (isLocal && !hasWorldPos) {
        return se.play2D(assets_, soundId, effScale, pitch);
    }
    return se.play3D(assets_, soundId, renderPos, effScale, pitch);
}

bool CombatSoundManager::executeSwingForEvent(
        QueuedCombatEvent& ev, const glm::vec3* overridePos,
        bool hasUnderwaterOverride, bool underwaterOverride) {
    if (ev.swingConsumed) return false;
    ev.swingConsumed = true;
    if (ev.isRanged) return false;

    const bool underwater = hasUnderwaterOverride ? underwaterOverride
        : (ev.attackerUnderwater || isUnitUnderwater(ev.attackerGuid));
    const float pitch = underwater ? kUnderwaterSwingPitch : 1.0f;
    const WeaponSize wsize = getWeaponSizeForSubclass(ev.attackerWeaponSubClass);
    const bool isCrit = (ev.outcome == CombatOutcome::CRIT);
    const uint32_t sid = getWeaponSwingSoundId(wsize, isCrit);
    const glm::vec3 pos = overridePos ? *overridePos : ev.attackerRenderPos;
    const bool isLocalParticipant = ev.attackerIsLocal || ev.victimIsLocal;

    // Wow.exe 0x00467000 sets SEPlayParams.flags=0x0A, volume=1.0f (0.5f when dampened)
    // for WeaponSwingSounds2.dbc rows, overriding SoundEntries.dbc VolumeFloat (0.70/0.75).
    const bool played = playCombatSound(sid, pos, isLocalParticipant, pitch, /*overrideEntryVolume=*/1.0f);

    if (attackerExertionCallback_ &&
        ev.outcome != CombatOutcome::MISS &&
        ev.outcome != CombatOutcome::EVADE_DEFLECT &&
        ev.outcome != CombatOutcome::IMMUNE_OR_ABSORBED) {
        attackerExertionCallback_(
            ev.attackerGuid, ev.attackerDisplayId, pos,
            ev.attackerIsPlayer, ev.attackerIsLocal, isCrit);
    }
    return played;
}

bool CombatSoundManager::executeHitForEvent(QueuedCombatEvent& ev) {
    if (ev.hitConsumed) return false;
    ev.hitConsumed = true;

    const bool isLocalParticipant = ev.attackerIsLocal || ev.victimIsLocal;
    uint32_t soundId = 0u;

    switch (ev.outcome) {
        case CombatOutcome::IMMUNE_OR_ABSORBED:
            return false;

        case CombatOutcome::MISS:
        case CombatOutcome::DODGE:
            soundId = getWeaponMissSoundId(isTwoHandWeaponSubclass(ev.attackerWeaponSubClass));
            return playCombatSound(soundId, ev.victimRenderPos, isLocalParticipant, 1.0f);

        case CombatOutcome::EVADE_DEFLECT:
            soundId = getDeflectSoundId();
            return playCombatSound(soundId, ev.victimRenderPos, isLocalParticipant, 1.0f);

        case CombatOutcome::PARRY: {
            const ImpactType parryCol = ev.defenderParryWeaponIsMetal
                ? ImpactType::PARRY_METAL_METAL
                : ImpactType::PARRY_METAL_WOOD;
            soundId = getWeaponImpactSoundId(
                ev.attackerWeaponSubClass, ev.attackerWeaponIsMetal, parryCol, /*isCrit=*/false);
            return playCombatSound(soundId, ev.victimRenderPos, isLocalParticipant, 1.0f);
        }

        case CombatOutcome::BLOCK: {
            const ImpactType shieldCol = ev.defenderShieldIsMetal
                ? ImpactType::SHIELD_METAL
                : ImpactType::SHIELD_WOOD;
            soundId = getWeaponImpactSoundId(
                ev.attackerWeaponSubClass, ev.attackerWeaponIsMetal, shieldCol, /*isCrit=*/false);
            return playCombatSound(soundId, ev.victimRenderPos, isLocalParticipant, 1.0f);
        }

        case CombatOutcome::HIT:
        case CombatOutcome::CRIT:
        case CombatOutcome::CRUSHING:
        case CombatOutcome::GLANCING: {
            const bool isCrit = (ev.outcome == CombatOutcome::CRIT);
            soundId = getWeaponImpactSoundId(
                ev.attackerWeaponSubClass, ev.attackerWeaponIsMetal, ev.victimImpactType, isCrit);
            const bool played = playCombatSound(soundId, ev.victimRenderPos, isLocalParticipant, 1.0f);

            if (!ev.suppressVictimWound && victimWoundCallback_) {
                victimWoundCallback_(
                    ev.victimGuid, ev.victimDisplayId, ev.victimRenderPos,
                    ev.victimIsPlayer, ev.victimIsLocal, ev.outcome);
            }
            return played;
        }
    }
    return false;
}

void CombatSoundManager::queueCombatEvent(QueuedCombatEvent ev) {
    ensureWeaponDbcLoaded();
    if (ev.attackerGuid == 0u && ev.victimGuid == 0u) return;

    const bool hasKeyframes = unitHasKeyframeDriver(ev.attackerGuid);
    ev.ageSeconds = 0.0f;
    ev.hitConsumed = false;
    ev.swingFallbackDelay = hasKeyframes ? kKeyframeSafetySwingDelaySec : kHeadlessSwingDelaySec;
    ev.hitFallbackDelay = hasKeyframes ? kKeyframeSafetyHitDelaySec : kHeadlessHitDelaySec;

    if (ev.isRanged) {
        ev.swingConsumed = true;
    } else if (auto it = recentUnqueuedSwingTimer_.find(ev.attackerGuid);
               it != recentUnqueuedSwingTimer_.end() && it->second > 0.0f) {
        // An M2 $CSS keyframe just fired slightly ahead of packet arrival;
        // consume that swing so we do not double-whoosh.
        ev.swingConsumed = true;
        recentUnqueuedSwingTimer_.erase(it);
    }

    // Cap total queued combat events across all attackers so high-density multi-mob
    // pulls never build a multi-second post-combat audio backlog.
    while (pendingCombatEventCount(0u) >= kMaxTotalQueuedCombatEvents) {
        auto oldestMapIt = pendingByAttacker_.end();
        float oldestAge = -1.0f;
        for (auto it = pendingByAttacker_.begin(); it != pendingByAttacker_.end(); ++it) {
            if (!it->second.empty() && it->second.front().ageSeconds > oldestAge) {
                oldestAge = it->second.front().ageSeconds;
                oldestMapIt = it;
            }
        }
        if (oldestMapIt == pendingByAttacker_.end()) break;
        oldestMapIt->second.pop_front();
        if (oldestMapIt->second.empty()) {
            pendingByAttacker_.erase(oldestMapIt);
        }
    }

    auto& q = pendingByAttacker_[ev.attackerGuid];
    while (q.size() >= kMaxQueuedEventsPerUnit) {
        q.pop_front();
    }
    q.push_back(ev);
}

void CombatSoundManager::update(float deltaTime) {
    if (deltaTime <= 0.0f) return;

    for (auto it = recentUnqueuedSwingTimer_.begin(); it != recentUnqueuedSwingTimer_.end(); ) {
        it->second -= deltaTime;
        if (it->second <= 0.0f) it = recentUnqueuedSwingTimer_.erase(it);
        else ++it;
    }

    for (auto mapIt = pendingByAttacker_.begin(); mapIt != pendingByAttacker_.end(); ) {
        auto& q = mapIt->second;
        for (auto& ev : q) {
            ev.ageSeconds += deltaTime;
            if (!ev.swingConsumed && ev.ageSeconds >= ev.swingFallbackDelay) {
                executeSwingForEvent(ev, nullptr, false, false);
            }
            if (!ev.hitConsumed && ev.ageSeconds >= ev.hitFallbackDelay) {
                executeHitForEvent(ev);
            }
        }
        while (!q.empty() && q.front().swingConsumed && q.front().hitConsumed) {
            q.pop_front();
        }
        if (q.empty()) mapIt = pendingByAttacker_.erase(mapIt);
        else ++mapIt;
    }
}

void CombatSoundManager::clearPendingCombatEvents() {
    pendingByAttacker_.clear();
    recentUnqueuedSwingTimer_.clear();
}

void CombatSoundManager::clearUnitEvents(uint64_t unitGuid) {
    if (unitGuid == 0u) return;
    pendingByAttacker_.erase(unitGuid);
    recentUnqueuedSwingTimer_.erase(unitGuid);
    keyframeDrivenUnits_.erase(unitGuid);
    underwaterUnits_.erase(unitGuid);
    for (auto mapIt = pendingByAttacker_.begin(); mapIt != pendingByAttacker_.end(); ) {
        auto& q = mapIt->second;
        q.erase(std::remove_if(q.begin(), q.end(), [unitGuid](const QueuedCombatEvent& ev) {
            return ev.victimGuid == unitGuid;
        }), q.end());
        if (q.empty()) mapIt = pendingByAttacker_.erase(mapIt);
        else ++mapIt;
    }
}

size_t CombatSoundManager::pendingCombatEventCount(uint64_t attackerGuid) const {
    if (attackerGuid != 0u) {
        auto it = pendingByAttacker_.find(attackerGuid);
        return it != pendingByAttacker_.end() ? it->second.size() : 0u;
    }
    size_t total = 0u;
    for (const auto& [guid, q] : pendingByAttacker_) {
        total += q.size();
    }
    return total;
}

bool CombatSoundManager::onUnitSwingEvent(
        uint64_t unitGuid, const glm::vec3& renderPos, bool isLocalPlayer,
        bool offHand, bool underwater, uint32_t fallbackWeaponSubClass) {
    ensureWeaponDbcLoaded();
    if (unitGuid != 0u) {
        keyframeDrivenUnits_.insert(unitGuid);
    }
    (void)offHand;

    if (auto it = pendingByAttacker_.find(unitGuid); it != pendingByAttacker_.end()) {
        for (auto& ev : it->second) {
            if (!ev.swingConsumed && !ev.isRanged) {
                ev.swingFallbackDelay = kKeyframeSafetySwingDelaySec;
                ev.hitFallbackDelay = std::max(ev.hitFallbackDelay, ev.ageSeconds + 0.45f);
                const bool effectiveUnderwater = underwater || ev.attackerUnderwater || isUnitUnderwater(unitGuid);
                return executeSwingForEvent(ev, &renderPos, true, effectiveUnderwater);
            }
        }
    }

    // No packet queued yet (client-predicted swing or standalone keyframe call):
    // play weapon swing immediately and record grace window so a packet arriving
    // shortly afterward skips duplicate swing playback.
    if (unitGuid != 0u) {
        recentUnqueuedSwingTimer_[unitGuid] = kUnqueuedSwingGraceSec;
    }
    const bool effectiveUnderwater = underwater || isUnitUnderwater(unitGuid);
    const WeaponSize wsize = getWeaponSizeForSubclass(fallbackWeaponSubClass);
    return playWeaponSwing3D(wsize, /*isCrit=*/false, renderPos, isLocalPlayer, effectiveUnderwater);
}

bool CombatSoundManager::onUnitHitEvent(
        uint64_t attackerGuid, const glm::vec3& attackerRenderPos, bool isLocalAttacker) {
    ensureWeaponDbcLoaded();
    (void)attackerRenderPos;
    (void)isLocalAttacker;
    if (attackerGuid != 0u) {
        keyframeDrivenUnits_.insert(attackerGuid);
    }

    auto it = pendingByAttacker_.find(attackerGuid);
    if (it == pendingByAttacker_.end() || it->second.empty()) {
        return false;
    }

    for (auto qIt = it->second.begin(); qIt != it->second.end(); ++qIt) {
        if (!qIt->hitConsumed) {
            QueuedCombatEvent ev = *qIt;
            it->second.erase(qIt);
            if (it->second.empty()) {
                pendingByAttacker_.erase(it);
            }
            return executeHitForEvent(ev);
        }
    }
    return false;
}

bool CombatSoundManager::onUnitBowPullEvent(
        uint64_t unitGuid, const glm::vec3& renderPos, bool isLocalPlayer, uint32_t rangedSubClass) {
    ensureWeaponDbcLoaded();
    if (unitGuid != 0u) {
        keyframeDrivenUnits_.insert(unitGuid);
    }
    const uint32_t sid = getBowPullSoundId(rangedSubClass);
    return playCombatSound(sid, renderPos, isLocalPlayer, 1.0f);
}

bool CombatSoundManager::onUnitBowReleaseEvent(
        uint64_t unitGuid, const glm::vec3& renderPos, bool isLocalPlayer, uint32_t rangedSubClass) {
    ensureWeaponDbcLoaded();
    if (unitGuid != 0u) {
        keyframeDrivenUnits_.insert(unitGuid);
    }
    const uint32_t sid = getBowReleaseSoundId(rangedSubClass);
    return playCombatSound(sid, renderPos, isLocalPlayer, 1.0f);
}

bool CombatSoundManager::onUnitSheatheEvent(
        uint64_t unitGuid, const glm::vec3& renderPos, bool isLocalPlayer,
        uint32_t itemClass, uint32_t itemSubClass, int32_t material,
        bool isShield, bool unsheathe) {
    ensureWeaponDbcLoaded();
    if (unitGuid != 0u) {
        keyframeDrivenUnits_.insert(unitGuid);
    }
    const uint32_t sid = getSheatheSoundId(itemClass, itemSubClass, material, isShield, unsheathe);
    return playCombatSound(sid, renderPos, isLocalPlayer, 1.0f);
}

void CombatSoundManager::playWeaponSwing(WeaponSize size, bool isCrit, bool underwater) {
    (void)playWeaponSwing3D(size, isCrit, glm::vec3(0.0f), /*isLocal=*/true, underwater);
}

bool CombatSoundManager::playWeaponSwing3D(
        WeaponSize size, bool isCrit, const glm::vec3& renderPos, bool isLocal, bool underwater) {
    const uint32_t sid = getWeaponSwingSoundId(size, isCrit);
    const float pitch = underwater ? kUnderwaterSwingPitch : 1.0f;
    return playCombatSound(sid, renderPos, isLocal, pitch, /*overrideEntryVolume=*/1.0f);
}

void CombatSoundManager::playWeaponMiss(bool twoHanded) {
    (void)playWeaponMiss3D(twoHanded, glm::vec3(0.0f), /*isLocal=*/true);
}

bool CombatSoundManager::playWeaponMiss3D(bool twoHanded, const glm::vec3& renderPos, bool isLocal) {
    const uint32_t sid = getWeaponMissSoundId(twoHanded);
    return playCombatSound(sid, renderPos, isLocal, 1.0f);
}

void CombatSoundManager::playImpact(WeaponSize weaponSize, ImpactType impactType, bool isCrit) {
    uint32_t subClass = 7u; // 1H Sword
    if (weaponSize == WeaponSize::SMALL) subClass = 15u;      // Dagger
    else if (weaponSize == WeaponSize::LARGE) subClass = 8u;  // 2H Sword
    playWeaponImpactBySubclass(subClass, /*metalWeapon=*/true, impactType, isCrit);
}

void CombatSoundManager::playWeaponImpactBySubclass(
        uint32_t weaponSubClass, bool metalWeapon, ImpactType impactType, bool isCrit) {
    (void)playWeaponImpact3D(weaponSubClass, metalWeapon, impactType, isCrit,
                             glm::vec3(0.0f), /*isLocal=*/true);
}

bool CombatSoundManager::playWeaponImpact3D(
        uint32_t weaponSubClass, bool metalWeapon, ImpactType impactType, bool isCrit,
        const glm::vec3& renderPos, bool isLocal) {
    const uint32_t sid = getWeaponImpactSoundId(weaponSubClass, metalWeapon, impactType, isCrit);
    return playCombatSound(sid, renderPos, isLocal, 1.0f);
}

bool CombatSoundManager::playSheathe(
        uint32_t itemClass, uint32_t itemSubClass, int32_t material,
        bool isShield, bool unsheathe) {
    const uint32_t sid = getSheatheSoundId(itemClass, itemSubClass, material, isShield, unsheathe);
    return playCombatSound(sid, glm::vec3(0.0f), /*isLocal=*/true, 1.0f);
}

bool CombatSoundManager::playRangedShot(uint32_t rangedSubClass) {
    const uint32_t sid = getBowReleaseSoundId(rangedSubClass);
    return playCombatSound(sid, glm::vec3(0.0f), /*isLocal=*/true, 1.0f);
}

} // namespace audio
} // namespace wowee
