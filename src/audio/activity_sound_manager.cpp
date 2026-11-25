#include "audio/activity_sound_manager.hpp"
#include "audio/audio_engine.hpp"
#include "audio/sound_entries.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "core/logger.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace wowee {
namespace audio {

namespace {

uint32_t sanitizeSoundId(uint32_t id) {
    return (id == 0xFFFFFFFFu) ? 0u : id;
}

std::string normalizeModelStem(std::string s) {
    for (char& ch : s) {
        if (ch == '/') ch = '\\';
        else ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (const size_t dot = s.rfind('.'); dot != std::string::npos) {
        s.erase(dot);
    }
    return s;
}

uint8_t raceIdFromToken(const std::string& tokenLower) {
    if (tokenLower.find("bloodelf") != std::string::npos || tokenLower.find("blood elf") != std::string::npos) return 10;
    if (tokenLower.find("nightelf") != std::string::npos || tokenLower.find("night elf") != std::string::npos) return 4;
    if (tokenLower.find("draenei") != std::string::npos) return 11;
    if (tokenLower.find("scourge") != std::string::npos || tokenLower.find("undead") != std::string::npos) return 5;
    if (tokenLower.find("tauren") != std::string::npos) return 6;
    if (tokenLower.find("gnome") != std::string::npos) return 7;
    if (tokenLower.find("troll") != std::string::npos) return 8;
    if (tokenLower.find("dwarf") != std::string::npos) return 3;
    if (tokenLower.find("orc") != std::string::npos) return 2;
    if (tokenLower.find("goblin") != std::string::npos) return 9;
    if (tokenLower.find("human") != std::string::npos) return 1;
    return 0;
}

bool raceFolderAndBaseForId(uint8_t raceId, std::string& folder, std::string& base) {
    switch (raceId) {
        case 1:  folder = "Human";    base = "Human";    return true;
        case 2:  folder = "Orc";      base = "Orc";      return true;
        case 3:  folder = "Dwarf";    base = "Dwarf";    return true;
        case 4:  folder = "NightElf"; base = "NightElf"; return true;
        case 5:  folder = "Scourge";  base = "Scourge";  return true;
        case 6:  folder = "Tauren";   base = "Tauren";   return true;
        case 7:  folder = "Gnome";    base = "Gnome";    return true;
        case 8:  folder = "Troll";    base = "Troll";    return true;
        case 10: folder = "BloodElf"; base = "BloodElf"; return true;
        case 11: folder = "Draenei";  base = "Draenei";  return true;
        case 9:  folder = "Goblin";   base = "Goblin";   return true;
        default: return false;
    }
}

// Tauren Druids use Horde-specific CreatureDisplayInfo IDs in TBC 2.4.3 when
// shapeshifted (while SpellShapeshiftForm.dbc col 23 stores the Alliance/default ID).
uint32_t hordeShapeshiftDisplayOverride(uint8_t formId, uint8_t raceId) {
    if (raceId != 6) return 0; // Tauren
    switch (formId) {
        case 1:  return 8571u;  // Tauren Cat Form (csd 1089)
        case 5:
        case 8:  return 2289u;  // Tauren Bear / Dire Bear Form (csd 83)
        case 27: return 21244u; // Tauren Swift Flight Form
        case 29: return 20872u; // Tauren Flight Form
        case 31: return 15375u; // Tauren Moonkin Form (csd 204)
        default: return 0u;
    }
}

std::vector<std::string> exertionPrefixes(const std::string& raceBase, bool male) {
    const std::string gender = male ? "Male" : "Female";
    const std::string race = (raceBase == "Scourge") ? "Undead" : raceBase;
    const std::string stem = race + gender;
    const std::string base = "Sound\\Character\\PlayerExertions\\";

    std::vector<std::string> prefixes;
    prefixes.push_back(base + stem + "Final\\" + stem + "Main");
    prefixes.push_back(base + stem + "\\" + stem + "Main");
    prefixes.push_back(base + stem + "Final\\" + stem + "Final");
    prefixes.push_back("Sound\\Character\\" + stem + "PC\\" + stem + "PC");
    if (race == "Human" && !male) {
        prefixes.push_back(base + stem + "Final\\HumanFeamleMain");
    }
    if (race == "Troll" && !male) {
        prefixes.push_back(base + stem + "Final\\TrollFemalMain");
    }
    return prefixes;
}

} // namespace

ActivitySoundManager::ActivitySoundManager() : rng(std::random_device{}()) {}
ActivitySoundManager::~ActivitySoundManager() { shutdown(); }

bool ActivitySoundManager::initialize(pipeline::AssetManager* assets) {
    shutdown();
    assetManager = assets;
    if (!assetManager) return false;

    ensureDbcLoaded();

    preloadCandidates(splashEnterClips, {
        "Sound\\Character\\Footsteps\\EnterWaterSplash\\EnterWaterSmallA.wav",
        "Sound\\Character\\Footsteps\\EnterWaterSplash\\EnterWaterMediumA.wav",
        "Sound\\Character\\Footsteps\\EnterWaterSplash\\EnterWaterGiantA.wav",
        "Sound\\Character\\Footsteps\\WaterSplash\\FootStepsMediumWaterA.wav",
        "Sound\\Character\\Footsteps\\WaterSplash\\FootStepsMediumWaterB.wav",
        "Sound\\Character\\Footsteps\\WaterSplash\\FootStepsMediumWaterC.wav",
        "Sound\\Character\\Footsteps\\WaterSplash\\FootStepsMediumWaterD.wav",
        "Sound\\Character\\Footsteps\\WaterSplash\\FootStepsMediumWaterE.wav"
    });
    splashExitClips = splashEnterClips;

    preloadCandidates(meleeSwingClips, {
        "Sound\\Item\\Weapons\\WeaponSwings\\mWooshMedium1.wav",
        "Sound\\Item\\Weapons\\WeaponSwings\\mWooshMedium2.wav",
        "Sound\\Item\\Weapons\\WeaponSwings\\mWooshMedium3.wav",
        "Sound\\Item\\Weapons\\WeaponSwings\\mWooshLarge1.wav",
        "Sound\\Item\\Weapons\\WeaponSwings\\mWooshLarge2.wav",
        "Sound\\Item\\Weapons\\WeaponSwings\\mWooshLarge3.wav",
        "Sound\\Item\\Weapons\\MissSwings\\MissWhoosh1Handed.wav",
        "Sound\\Item\\Weapons\\MissSwings\\MissWhoosh2Handed.wav"
    });

    initialized = true;
    core::Logger::getInstance().info("Activity SFX initialized: splash=", splashEnterClips.size(),
                                     " csdProfiles=", soundDataProfiles_.size());
    return true;
}

void ActivitySoundManager::shutdown() {
    stopSwimLoop();
    stopOneShot();
    std::remove(loopTempPath.c_str());
    splashEnterClips.clear();
    splashExitClips.clear();
    swimLoopClips.clear();
    hardLandClips.clear();
    meleeSwingClips.clear();
    raceDisplayIds_.clear();
    displayToSoundData_.clear();
    modelStemToSoundData_.clear();
    soundDataProfiles_.clear();
    shapeshiftDisplays_.clear();
    baseRaceId_ = 0;
    baseGender_ = 0;
    baseDisplayId_ = 0;
    baseSoundDataId_ = 0;
    activeDisplayId_ = 0;
    shapeshiftFormId_ = 0;
    voiceProfileKey.clear();
    dbcLoaded_ = false;
    swimmingActive = false;
    swimMoving = false;
    initialized = false;
    assetManager = nullptr;
}

void ActivitySoundManager::ensureDbcLoaded() {
    if (dbcLoaded_ || !assetManager || !assetManager->isInitialized()) return;

    auto cmdDbc = assetManager->loadDBC("CreatureModelData.dbc");
    auto cdiDbc = assetManager->loadDBC("CreatureDisplayInfo.dbc");
    auto csdDbc = assetManager->loadDBC("CreatureSoundData.dbc");
    if (!cmdDbc || !cmdDbc->isLoaded() || cmdDbc->getFieldCount() < 14 ||
        !cdiDbc || !cdiDbc->isLoaded() || cdiDbc->getFieldCount() < 3 ||
        !csdDbc || !csdDbc->isLoaded() || csdDbc->getFieldCount() < 28) {
        return;
    }

    dbcLoaded_ = true;

    std::unordered_map<uint32_t, uint32_t> modelToSoundData;
    modelToSoundData.reserve(cmdDbc->getRecordCount());
    for (uint32_t r = 0; r < cmdDbc->getRecordCount(); ++r) {
        const uint32_t modelId = cmdDbc->getUInt32(r, 0);
        const uint32_t soundDataId = sanitizeSoundId(cmdDbc->getUInt32(r, 13));
        if (modelId == 0) continue;
        if (soundDataId != 0) {
            modelToSoundData[modelId] = soundDataId;
            const std::string modelPath = cmdDbc->getString(r, 2);
            if (!modelPath.empty()) {
                modelStemToSoundData_[normalizeModelStem(modelPath)] = soundDataId;
            }
        }
    }

    displayToSoundData_.reserve(cdiDbc->getRecordCount());
    for (uint32_t r = 0; r < cdiDbc->getRecordCount(); ++r) {
        const uint32_t displayId = cdiDbc->getUInt32(r, 0);
        const uint32_t modelId = cdiDbc->getUInt32(r, 1);
        uint32_t soundDataId = sanitizeSoundId(cdiDbc->getUInt32(r, 2));
        if (soundDataId == 0) {
            if (const auto it = modelToSoundData.find(modelId); it != modelToSoundData.end()) {
                soundDataId = it->second;
            }
        }
        if (displayId != 0 && soundDataId != 0) {
            displayToSoundData_[displayId] = soundDataId;
        }
    }

    soundDataProfiles_.reserve(csdDbc->getRecordCount());
    for (uint32_t r = 0; r < csdDbc->getRecordCount(); ++r) {
        const uint32_t id = csdDbc->getUInt32(r, 0);
        if (id == 0) continue;
        CreatureVocalProfile p;
        p.soundDataId      = id;
        p.exertionId       = sanitizeSoundId(csdDbc->getUInt32(r, 1));
        p.exertionCritId   = sanitizeSoundId(csdDbc->getUInt32(r, 2));
        p.injuryId         = sanitizeSoundId(csdDbc->getUInt32(r, 3));
        p.injuryCritId     = sanitizeSoundId(csdDbc->getUInt32(r, 4));
        p.injuryCrushingId = sanitizeSoundId(csdDbc->getUInt32(r, 5));
        p.deathId          = sanitizeSoundId(csdDbc->getUInt32(r, 6));
        p.jumpStartId      = sanitizeSoundId(csdDbc->getUInt32(r, 26));
        p.jumpEndId        = sanitizeSoundId(csdDbc->getUInt32(r, 27));
        soundDataProfiles_[id] = p;
    }

    if (auto racesDbc = assetManager->loadDBC("ChrRaces.dbc");
        racesDbc && racesDbc->isLoaded() && racesDbc->getFieldCount() >= 6) {
        for (uint32_t r = 0; r < racesDbc->getRecordCount(); ++r) {
            const uint32_t raceId = racesDbc->getUInt32(r, 0);
            if (raceId == 0 || raceId > 255) continue;
            const uint32_t maleDisp = racesDbc->getUInt32(r, 4);
            const uint32_t femDisp  = racesDbc->getUInt32(r, 5);
            if (maleDisp != 0) raceDisplayIds_[static_cast<uint16_t>((raceId << 1u) | 0u)] = maleDisp;
            if (femDisp != 0)  raceDisplayIds_[static_cast<uint16_t>((raceId << 1u) | 1u)] = femDisp;
        }
    }

    if (auto ssfDbc = assetManager->loadDBC("SpellShapeshiftForm.dbc");
        ssfDbc && ssfDbc->isLoaded() && ssfDbc->getFieldCount() >= 24) {
        // TBC 2.4.3 SpellShapeshiftForm.dbc (35 fields): col 23..26 = CreatureDisplayID[0..3]
        // WotLK 3.3.5a (35 fields): same or check field count.
        const uint32_t dispBaseCol = 23u;
        for (uint32_t r = 0; r < ssfDbc->getRecordCount(); ++r) {
            const uint32_t formId = ssfDbc->getUInt32(r, 0);
            if (formId == 0 || formId > 255) continue;
            ShapeshiftDisplayEntry entry{};
            for (uint32_t i = 0; i < 4 && (dispBaseCol + i) < ssfDbc->getFieldCount(); ++i) {
                entry.displayIds[i] = sanitizeSoundId(ssfDbc->getUInt32(r, dispBaseCol + i));
            }
            if (entry.displayIds[0] != 0 || entry.displayIds[1] != 0) {
                shapeshiftDisplays_[static_cast<uint8_t>(formId)] = entry;
            }
        }
    }
}

void ActivitySoundManager::update([[maybe_unused]] float deltaTime) {
    reapProcesses();

    if (swimmingActive && swimMoving && !swimLoopClips.empty()) {
        auto now = std::chrono::steady_clock::now();
        float elapsed = std::chrono::duration<float>(now - lastSwimStrokeAt).count();

        if (lastSwimStrokeAt.time_since_epoch().count() == 0 || elapsed >= 0.8f) {
            std::uniform_int_distribution<size_t> clipDist(0, swimLoopClips.size() - 1);
            const Sample& sample = swimLoopClips[clipDist(rng)];
            float volume = 0.6f * volumeScale;
            AudioEngine::instance().playSound2D(sample.data, volume, 1.0f);
            lastSwimStrokeAt = now;
        }
    } else if (!swimmingActive) {
        lastSwimStrokeAt = std::chrono::steady_clock::time_point{};
    }
}

void ActivitySoundManager::preloadCandidates(std::vector<Sample>& out, const std::vector<std::string>& candidates) {
    if (!assetManager) return;
    for (const auto& path : candidates) {
        if (!assetManager->fileExists(path)) continue;
        auto data = assetManager->readFile(path);
        if (data.empty()) continue;
        out.push_back({path, std::move(data)});
    }
}

void ActivitySoundManager::rebuildSwimLoopClipsForProfile([[maybe_unused]] const std::string& raceFolder,
                                                          [[maybe_unused]] const std::string& raceBase,
                                                          [[maybe_unused]] bool male) {
    swimLoopClips.clear();
    preloadCandidates(swimLoopClips, {
        "Sound\\Character\\Footsteps\\WaterSplash\\FootStepsMediumWaterA.wav",
        "Sound\\Character\\Footsteps\\WaterSplash\\FootStepsMediumWaterB.wav",
        "Sound\\Character\\Footsteps\\WaterSplash\\FootStepsMediumWaterC.wav",
        "Sound\\Character\\Footsteps\\WaterSplash\\FootStepsMediumWaterD.wav",
        "Sound\\Character\\Footsteps\\WaterSplash\\FootStepsMediumWaterE.wav",
        "Sound\\Character\\Footsteps\\WaterSplash\\FootStepsSmallWaterA.wav",
        "Sound\\Character\\Footsteps\\WaterSplash\\FootStepsSmallWaterB.wav",
        "Sound\\Character\\Footsteps\\WaterSplash\\FootStepsSmallWaterC.wav",
        "Sound\\Character\\Footsteps\\WaterSplash\\FootStepsSmallWaterD.wav",
        "Sound\\Character\\Footsteps\\WaterSplash\\FootStepsSmallWaterE.wav"
    });
}

void ActivitySoundManager::rebuildHardLandClipsForProfile(const std::string& raceFolder,
                                                          const std::string& raceBase,
                                                          bool male) {
    hardLandClips.clear();
    const std::string gender = male ? "Male" : "Female";
    const std::string prefix = "Sound\\Character\\" + raceFolder + "\\";
    const std::string stem = raceBase + gender;
    std::vector<std::string> candidates = {
        prefix + stem + "\\" + stem + "LandHard01.wav",
        prefix + stem + "\\" + stem + "LandHard02.wav",
        prefix + stem + "LandHard01.wav",
        prefix + stem + "LandHard02.wav"
    };
    for (const auto& exert : exertionPrefixes(raceBase, male)) {
        candidates.push_back(exert + "Land.wav");
    }
    preloadCandidates(hardLandClips, candidates);
}

void ActivitySoundManager::startSwimLoop() {}

void ActivitySoundManager::stopSwimLoop() {
    platform::killProcess(swimLoopPid);
}

void ActivitySoundManager::stopOneShot() {}

void ActivitySoundManager::reapProcesses() {
    if (swimLoopPid != INVALID_PROCESS) {
        platform::isProcessRunning(swimLoopPid);
    }
}

CreatureVocalProfile ActivitySoundManager::resolveVocalProfileForDisplay(uint32_t displayId) {
    ensureDbcLoaded();
    if (displayId == 0) return {};
    const auto dit = displayToSoundData_.find(displayId);
    if (dit == displayToSoundData_.end()) return {};
    const auto sit = soundDataProfiles_.find(dit->second);
    if (sit == soundDataProfiles_.end()) return {};
    CreatureVocalProfile out = sit->second;
    out.displayId = displayId;
    return out;
}

CreatureVocalProfile ActivitySoundManager::resolveVocalProfileForRace(uint8_t raceId, uint8_t gender) {
    ensureDbcLoaded();
    if (raceId == 0) return {};
    const uint16_t key = static_cast<uint16_t>((static_cast<uint16_t>(raceId) << 1u) | (gender ? 1u : 0u));
    const auto rit = raceDisplayIds_.find(key);
    if (rit == raceDisplayIds_.end()) return {};
    return resolveVocalProfileForDisplay(rit->second);
}

CreatureVocalProfile ActivitySoundManager::resolveVocalProfileForShapeshift(uint8_t formId, uint8_t raceId) {
    ensureDbcLoaded();
    if (formId == 0) return {};
    if (const uint32_t hordeDisp = hordeShapeshiftDisplayOverride(formId, raceId); hordeDisp != 0) {
        if (auto p = resolveVocalProfileForDisplay(hordeDisp); p.soundDataId != 0) {
            return p;
        }
    }
    const auto it = shapeshiftDisplays_.find(formId);
    if (it == shapeshiftDisplays_.end()) return {};
    const bool isHorde = (raceId == 2 || raceId == 5 || raceId == 6 || raceId == 8 || raceId == 10);
    if (isHorde && it->second.displayIds[1] != 0) {
        if (auto p = resolveVocalProfileForDisplay(it->second.displayIds[1]); p.soundDataId != 0) {
            return p;
        }
    }
    if (it->second.displayIds[0] != 0) {
        return resolveVocalProfileForDisplay(it->second.displayIds[0]);
    }
    return {};
}

CreatureVocalProfile ActivitySoundManager::getActiveVocalProfile() {
    ensureDbcLoaded();

    // 1) Explicit non-humanoid active display ID (UNIT_FIELD_DISPLAYID from shapeshift,
    //    Tauren Cat/Bear/Moonkin, Polymorph, Illusion, Noggenfogger, etc.)
    if (activeDisplayId_ != 0 && activeDisplayId_ != baseDisplayId_) {
        if (auto p = resolveVocalProfileForDisplay(activeDisplayId_); p.soundDataId != 0) {
            return p;
        }
    }

    // 2) Active SpellShapeshiftForm ID when UNIT_FIELD_DISPLAYID has not overridden it
    if (shapeshiftFormId_ != 0) {
        if (auto p = resolveVocalProfileForShapeshift(shapeshiftFormId_, baseRaceId_); p.soundDataId != 0) {
            return p;
        }
    }

    // 3) Base character display ID from ChrRaces.dbc
    if (baseDisplayId_ != 0) {
        if (auto p = resolveVocalProfileForDisplay(baseDisplayId_); p.soundDataId != 0) {
            return p;
        }
    }

    // 4) Direct CreatureModelData stem lookup (e.g. non-race creature model path)
    if (baseSoundDataId_ != 0) {
        if (const auto sit = soundDataProfiles_.find(baseSoundDataId_); sit != soundDataProfiles_.end()) {
            return sit->second;
        }
    }

    return {};
}

void ActivitySoundManager::playJump() {
    if (!assetManager || volumeScale <= 0.0f || !AudioEngine::instance().isInitialized()) {
        return;
    }

    // TBC 2.4.3 CreatureSoundData.dbc col 26 (SoundJumpStartID):
    // All 20 playable race-gender rows have SoundJumpStartID == 0 (silent jump;
    // unused Vanilla alpha *MainJump.wav files are never referenced by any DBC).
    // Creature shapeshift forms that author SoundJumpStartID (e.g. Druid Flight
    // Form 20857 / Swift Flight Form 21243 -> SoundEntries 10905) play it here.
    const CreatureVocalProfile profile = getActiveVocalProfile();
    if (profile.jumpStartId == 0) {
        return;
    }

    auto now = std::chrono::steady_clock::now();
    if (lastJumpAt.time_since_epoch().count() != 0) {
        if (std::chrono::duration<float>(now - lastJumpAt).count() < 0.35f) return;
    }

    if (SoundEntryTable::instance().play2D(assetManager, profile.jumpStartId, volumeScale)) {
        lastJumpAt = now;
    }
}

void ActivitySoundManager::playLanding(bool hardLanding) {
    if (!AudioEngine::instance().isInitialized()) {
        return;
    }

    auto now = std::chrono::steady_clock::now();
    if (lastLandAt.time_since_epoch().count() != 0) {
        if (std::chrono::duration<float>(now - lastLandAt).count() < 0.10f) return;
    }
    lastLandAt = now;

    const CreatureVocalProfile profile = getActiveVocalProfile();
    if (profile.jumpEndId != 0 && assetManager && volumeScale > 0.0f) {
        SoundEntryTable::instance().play2D(assetManager, profile.jumpEndId, volumeScale);
        return;
    }

    const bool creatureShapeshifted =
        (profile.displayId != 0 && baseDisplayId_ != 0 && profile.displayId != baseDisplayId_);
    if (hardLanding && !creatureShapeshifted && !hardLandClips.empty()) {
        std::uniform_int_distribution<size_t> dist(0, hardLandClips.size() - 1);
        const Sample& sample = hardLandClips[dist(rng)];
        std::uniform_real_distribution<float> volumeDist(0.80f, 0.88f);
        std::uniform_real_distribution<float> pitchDist(0.97f, 1.03f);
        AudioEngine::instance().playSound2D(sample.data, volumeDist(rng) * volumeScale, pitchDist(rng));
    }
}

void ActivitySoundManager::playMeleeSwing() {
    if (!AudioEngine::instance().isInitialized() || meleeSwingClips.empty()) {
        if (meleeSwingClips.empty() && !meleeSwingWarned) {
            core::Logger::getInstance().warning("No melee swing SFX found in assets");
            meleeSwingWarned = true;
        }
        return;
    }

    auto now = std::chrono::steady_clock::now();
    if (lastMeleeSwingAt.time_since_epoch().count() != 0) {
        if (std::chrono::duration<float>(now - lastMeleeSwingAt).count() < 0.12f) return;
    }

    std::uniform_int_distribution<size_t> dist(0, meleeSwingClips.size() - 1);
    const Sample& sample = meleeSwingClips[dist(rng)];

    std::uniform_real_distribution<float> volumeDist(0.76f, 0.84f);
    std::uniform_real_distribution<float> pitchDist(0.96f, 1.04f);

    if (AudioEngine::instance().playSound2D(sample.data, volumeDist(rng) * volumeScale, pitchDist(rng))) {
        lastMeleeSwingAt = now;
    }
}

void ActivitySoundManager::setSwimmingState(bool swimming, bool moving) {
    swimMoving = moving;
    if (swimming == swimmingActive) return;
    swimmingActive = swimming;
    if (swimmingActive) {
        LOG_INFO("Swimming started - playing swim loop");
        startSwimLoop();
    } else {
        LOG_INFO("Swimming stopped - stopping swim loop");
        stopSwimLoop();
    }
}

void ActivitySoundManager::applyRaceBaseStrings(const std::string& raceFolder,
                                                const std::string& raceBase,
                                                bool male) {
    ensureDbcLoaded();

    std::string lower = raceBase + " " + raceFolder;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    baseRaceId_ = raceIdFromToken(lower);
    baseGender_ = male ? 0u : 1u;
    baseDisplayId_ = 0;
    if (baseRaceId_ != 0) {
        const uint16_t rkey = static_cast<uint16_t>((static_cast<uint16_t>(baseRaceId_) << 1u) | baseGender_);
        if (const auto it = raceDisplayIds_.find(rkey); it != raceDisplayIds_.end()) {
            baseDisplayId_ = it->second;
        }
    }

    lastJumpAt = {};
    lastLandAt = {};
    lastAttackGruntAt = {};
    rebuildSwimLoopClipsForProfile(raceFolder, raceBase, male);
    rebuildHardLandClipsForProfile(raceFolder, raceBase, male);
}

void ActivitySoundManager::setCharacterVoiceProfile(const std::string& modelName) {
    if (!assetManager || modelName.empty()) return;
    ensureDbcLoaded();

    const std::string stem = normalizeModelStem(modelName);
    if (stem == voiceProfileKey) return;
    voiceProfileKey = stem;

    activeDisplayId_ = 0;
    shapeshiftFormId_ = 0;
    baseSoundDataId_ = 0;
    if (const auto mit = modelStemToSoundData_.find(stem); mit != modelStemToSoundData_.end()) {
        baseSoundDataId_ = mit->second;
    }

    const bool male = (stem.find("female") == std::string::npos);
    const uint8_t detectedRace = raceIdFromToken(stem);
    std::string folder = "Human";
    std::string base = "Human";
    if (detectedRace != 0) {
        raceFolderAndBaseForId(detectedRace, folder, base);
        applyRaceBaseStrings(folder, base, male);
    } else {
        baseRaceId_ = 0;
        baseGender_ = male ? 0u : 1u;
        baseDisplayId_ = 0;
        lastJumpAt = {};
        lastLandAt = {};
        lastAttackGruntAt = {};
        swimLoopClips.clear();
        hardLandClips.clear();
    }

    const CreatureVocalProfile p = getActiveVocalProfile();
    core::Logger::getInstance().info("Activity SFX voice profile: ", voiceProfileKey,
                                     " csd=", p.soundDataId,
                                     " exertion=", p.exertionId,
                                     " injury=", p.injuryId,
                                     " death=", p.deathId,
                                     " jumpStart=", p.jumpStartId);
}

void ActivitySoundManager::setCharacterVoiceProfile(const std::string& raceFolder,
                                                    const std::string& raceBase,
                                                    bool male) {
    if (!assetManager) return;
    std::string key = raceFolder + "|" + raceBase + "|" + (male ? "M" : "F");
    if (key == voiceProfileKey) return;
    voiceProfileKey = key;
    baseSoundDataId_ = 0;
    applyRaceBaseStrings(raceFolder, raceBase, male);

    const CreatureVocalProfile p = getActiveVocalProfile();
    core::Logger::getInstance().info("Activity SFX voice profile (explicit): ", voiceProfileKey,
                                     " csd=", p.soundDataId,
                                     " exertion=", p.exertionId,
                                     " injury=", p.injuryId,
                                     " death=", p.deathId,
                                     " jumpStart=", p.jumpStartId);
}

void ActivitySoundManager::setCharacterRaceGender(uint8_t raceId, uint8_t gender) {
    if (!assetManager || raceId == 0) return;
    const uint8_t normGender = gender ? 1u : 0u;
    if (baseRaceId_ == raceId && baseGender_ == normGender && baseDisplayId_ != 0) return;
    std::string folder;
    std::string base;
    if (!raceFolderAndBaseForId(raceId, folder, base)) return;
    setCharacterVoiceProfile(folder, base, normGender == 0);
}

void ActivitySoundManager::setActiveDisplayId(uint32_t displayId) {
    if (activeDisplayId_ == displayId) return;
    activeDisplayId_ = displayId;
}

void ActivitySoundManager::setShapeshiftForm(uint8_t formId) {
    if (shapeshiftFormId_ == formId) return;
    shapeshiftFormId_ = formId;
}

bool ActivitySoundManager::playSplash(const std::vector<Sample>& clips) {
    if (!AudioEngine::instance().isInitialized() || clips.empty()) return false;
    std::uniform_int_distribution<size_t> clipDist(0, clips.size() - 1);
    const Sample& sample = clips[clipDist(rng)];
    std::uniform_real_distribution<float> pitchDist(0.95f, 1.05f);
    return AudioEngine::instance().playSound2D(sample.data, 0.95f * volumeScale, pitchDist(rng));
}

void ActivitySoundManager::playWaterEnter() {
    LOG_INFO("Water entry detected - attempting to play splash sound");
    auto now = std::chrono::steady_clock::now();
    if (lastSplashAt.time_since_epoch().count() != 0) {
        if (std::chrono::duration<float>(now - lastSplashAt).count() < 0.20f) {
            LOG_DEBUG("Water splash throttled (too soon)");
            return;
        }
    }
    if (playSplash(splashEnterClips)) {
        lastSplashAt = now;
    } else {
        LOG_WARNING("Water splash enter sound unavailable (", splashEnterClips.size(), " clips loaded)");
    }
}

void ActivitySoundManager::playWaterExit() {
    LOG_INFO("Water exit detected - attempting to play splash sound");
    auto now = std::chrono::steady_clock::now();
    if (lastSplashAt.time_since_epoch().count() != 0) {
        if (std::chrono::duration<float>(now - lastSplashAt).count() < 0.20f) {
            LOG_DEBUG("Water splash throttled (too soon)");
            return;
        }
    }
    if (playSplash(splashExitClips)) {
        lastSplashAt = now;
    } else {
        LOG_WARNING("Water splash exit sound unavailable (", splashExitClips.size(), " clips loaded)");
    }
}

bool ActivitySoundManager::playExertion(bool isCrit) {
    if (!assetManager || volumeScale <= 0.0f || !AudioEngine::instance().isInitialized()) return false;
    const CreatureVocalProfile profile = getActiveVocalProfile();
    const uint32_t soundId = (isCrit && profile.exertionCritId != 0)
        ? profile.exertionCritId
        : profile.exertionId;
    if (soundId == 0) return false;
    return SoundEntryTable::instance().play2D(assetManager, soundId, volumeScale);
}

void ActivitySoundManager::playAttackGrunt() {
    if (!assetManager || volumeScale <= 0.0f || !AudioEngine::instance().isInitialized()) return;
    auto now = std::chrono::steady_clock::now();
    if (lastAttackGruntAt.time_since_epoch().count() != 0) {
        if (std::chrono::duration<float>(now - lastAttackGruntAt).count() < 1.5f) return;
    }
    std::uniform_int_distribution<int> chance(0, 9);
    if (chance(rng) > 2) return;

    if (playExertion(false)) {
        lastAttackGruntAt = now;
    }
}

void ActivitySoundManager::playWound(bool isCrit) {
    if (!assetManager || volumeScale <= 0.0f || !AudioEngine::instance().isInitialized()) return;
    const CreatureVocalProfile profile = getActiveVocalProfile();
    const uint32_t soundId = (isCrit && profile.injuryCritId != 0)
        ? profile.injuryCritId
        : profile.injuryId;
    if (soundId == 0) return;
    (void)SoundEntryTable::instance().play2D(assetManager, soundId, volumeScale);
}

bool ActivitySoundManager::playCrushingWound() {
    if (!assetManager || volumeScale <= 0.0f || !AudioEngine::instance().isInitialized()) return false;
    const CreatureVocalProfile profile = getActiveVocalProfile();
    uint32_t soundId = profile.injuryCrushingId;
    if (soundId == 0) soundId = profile.injuryCritId;
    if (soundId == 0) soundId = profile.injuryId;
    if (soundId == 0) return false;
    return SoundEntryTable::instance().play2D(assetManager, soundId, volumeScale);
}

void ActivitySoundManager::playDeath() {
    if (!assetManager || volumeScale <= 0.0f || !AudioEngine::instance().isInitialized()) return;
    const CreatureVocalProfile profile = getActiveVocalProfile();
    if (profile.deathId == 0) return;
    (void)SoundEntryTable::instance().play2D(assetManager, profile.deathId, volumeScale);
}

bool ActivitySoundManager::playUnitExertion3D(uint32_t displayId, bool isCrit, const glm::vec3& position) {
    if (!assetManager || volumeScale <= 0.0f || !AudioEngine::instance().isInitialized()) return false;
    const CreatureVocalProfile profile = resolveVocalProfileForDisplay(displayId);
    const uint32_t soundId = (isCrit && profile.exertionCritId != 0)
        ? profile.exertionCritId
        : profile.exertionId;
    if (soundId == 0) return false;
    return SoundEntryTable::instance().play3D(assetManager, soundId, position, volumeScale);
}

bool ActivitySoundManager::playUnitWound3D(uint32_t displayId, bool isCrit, bool isCrushing,
                                           const glm::vec3& position) {
    if (!assetManager || volumeScale <= 0.0f || !AudioEngine::instance().isInitialized()) return false;
    const CreatureVocalProfile profile = resolveVocalProfileForDisplay(displayId);
    uint32_t soundId = 0;
    if (isCrushing && profile.injuryCrushingId != 0) {
        soundId = profile.injuryCrushingId;
    } else if ((isCrit || isCrushing) && profile.injuryCritId != 0) {
        soundId = profile.injuryCritId;
    } else {
        soundId = profile.injuryId;
    }
    if (soundId == 0) return false;
    return SoundEntryTable::instance().play3D(assetManager, soundId, position, volumeScale);
}

bool ActivitySoundManager::playUnitDeath3D(uint32_t displayId, const glm::vec3& position) {
    if (!assetManager || volumeScale <= 0.0f || !AudioEngine::instance().isInitialized()) return false;
    const CreatureVocalProfile profile = resolveVocalProfileForDisplay(displayId);
    if (profile.deathId == 0) return false;
    return SoundEntryTable::instance().play3D(assetManager, profile.deathId, position, volumeScale);
}

bool ActivitySoundManager::playPlayerExertion3D(uint8_t raceId, uint8_t gender, uint32_t activeDisplayId,
                                                bool isCrit, const glm::vec3& position) {
    if (activeDisplayId != 0) {
        if (playUnitExertion3D(activeDisplayId, isCrit, position)) return true;
    }
    const CreatureVocalProfile base = resolveVocalProfileForRace(raceId, gender);
    return base.displayId != 0 && playUnitExertion3D(base.displayId, isCrit, position);
}

bool ActivitySoundManager::playPlayerWound3D(uint8_t raceId, uint8_t gender, uint32_t activeDisplayId,
                                             bool isCrit, bool isCrushing, const glm::vec3& position) {
    if (activeDisplayId != 0) {
        if (playUnitWound3D(activeDisplayId, isCrit, isCrushing, position)) return true;
    }
    const CreatureVocalProfile base = resolveVocalProfileForRace(raceId, gender);
    return base.displayId != 0 && playUnitWound3D(base.displayId, isCrit, isCrushing, position);
}

bool ActivitySoundManager::playPlayerDeath3D(uint8_t raceId, uint8_t gender, uint32_t activeDisplayId,
                                             const glm::vec3& position) {
    if (activeDisplayId != 0) {
        if (playUnitDeath3D(activeDisplayId, position)) return true;
    }
    const CreatureVocalProfile base = resolveVocalProfileForRace(raceId, gender);
    return base.displayId != 0 && playUnitDeath3D(base.displayId, position);
}

} // namespace audio
} // namespace wowee
