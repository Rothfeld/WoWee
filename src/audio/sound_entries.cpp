#include "audio/sound_entries.hpp"
#include "audio/audio_engine.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "core/logger.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace wowee {
namespace audio {

namespace {

std::string upperAscii(const std::string& s) {
    std::string out = s;
    for (char& ch : out) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return out;
}

std::string normalizeSoundPathKey(const std::string& s) {
    std::string out = s;
    for (char& ch : out) {
        if (ch == '/') ch = '\\';
        else ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return out;
}

std::string joinSoundPath(const std::string& dir, const std::string& file) {
    if (dir.empty()) return file;
    if (dir.back() == '\\' || dir.back() == '/') return dir + file;
    return dir + "\\" + file;
}

} // namespace

SoundEntryTable& SoundEntryTable::instance() {
    static SoundEntryTable table;
    return table;
}

uint32_t SoundEntryTable::categoryForSoundType(uint32_t dbcSoundType) {
    switch (dbcSoundType) {
        case 3u:  // Character dry footsteps
        case 20u: // Water footstep splashes
            return SESoundCategory::kFootstepOther; // cap = 6
        case 4u:  // Weapon/armor combat impacts
        case 14u: // Object/doodad impacts
        case 19u: // Armor foley ($TRD)
            return SESoundCategory::kCombatImpact;  // cap = 4
        case 6u:  // Weapon swings (WeaponSwingSounds2.dbc)
            return SESoundCategory::kWeaponSwing;   // cap = 2
        case 10u: // Unit combat exertion/injury/death
            return SESoundCategory::kCombatVocal;   // cap = 2
        case 12u: // VocalUISounds error speech
            return SESoundCategory::kVocalUi;       // cap = 1
        case 16u: // DeathThudLookups
            return SESoundCategory::kDeathThud;     // cap = 2
        case 17u: // NPC greetings/farewells/pissed
            return SESoundCategory::kNpcGreeting;   // cap = 1
        case 26u: // Spell fizzle
            return SESoundCategory::kSpellFizzle;   // cap = 1
        case 31u: // Racial flyby narration
            return SESoundCategory::kRacialCinematic; // cap = 1
        default:
            return SESoundCategory::kSfx;           // unlimited (0x7FFFFFFF)
    }
}

bool SoundEntryTable::usesEnvironmentBus(const SoundEntryRecord& rec, uint32_t resolvedCategory) {
    // Non-world 2D channels bypass the 3D environmental reverb/underwater bus:
    // SoundType 2 (UI, e.g. igMainMenuOptionCheckBoxOn), 28 (ZoneMusic), 30 (IntroMusic),
    // 31 (Narration), and 50 (ZoneAmbience).
    switch (rec.soundType) {
        case 2u:
        case 28u:
        case 30u:
        case 31u:
        case 50u:
            return false;
        default:
            break;
    }
    switch (resolvedCategory) {
        case SESoundCategory::kMusic:
        case SESoundCategory::kAmbience:
        case SESoundCategory::kCinematic:
        case SESoundCategory::kScriptMusic:
        case SESoundCategory::kRacialCinematic:
            return false;
        default:
            break;
    }
    // EAXDef == 1 maps to SoundSamplePreferences ID 1 (Exclusion = -10000 mB, e.g. Ashbringer).
    if (rec.eaxDef == 1u) {
        return false;
    }
    return true;
}

SoundEntryTable::SoundEntryTable() : rng_(std::random_device{}()) {}

void SoundEntryTable::reset() {
    loadedAssets_ = nullptr;
    loaded_ = false;
    byId_.clear();
    byName_.clear();
    byPath_.clear();
}

bool SoundEntryTable::ensureLoaded(pipeline::AssetManager* assets) {
    if (!assets || !assets->isInitialized()) return false;
    if (loaded_ && loadedAssets_ == assets) return true;

    auto dbc = assets->loadDBC("SoundEntries.dbc");
    if (!dbc || !dbc->isLoaded() || dbc->getFieldCount() < 24) {
        return false;
    }

    byId_.clear();
    byName_.clear();
    byPath_.clear();
    loadedAssets_ = assets;
    loaded_ = true;

    const uint32_t fc = dbc->getFieldCount();
    byId_.reserve(dbc->getRecordCount());

    for (uint32_t row = 0; row < dbc->getRecordCount(); ++row) {
        const uint32_t id = dbc->getUInt32(row, 0);
        if (id == 0) continue;

        SoundEntryRecord rec;
        rec.id = id;
        rec.soundType = dbc->getUInt32(row, 1);
        rec.name = dbc->getString(row, 2);

        const std::string dir = dbc->getString(row, 23);
        for (uint32_t i = 0; i < 10; ++i) {
            const std::string file = dbc->getString(row, 3 + i);
            if (file.empty()) continue;
            const uint32_t freq = dbc->getUInt32(row, 13 + i);
            rec.variants.push_back(SoundVariant{
                .path = joinSoundPath(dir, file),
                .weight = (freq > 0) ? freq : 1u,
            });
        }

        if (fc > 24) {
            const float vol = dbc->getFloat(row, 24);
            if (std::isfinite(vol) && vol > 0.0f) {
                rec.volume = std::clamp(vol, 0.0f, 1.0f);
            }
        }
        if (fc > 25) {
            rec.flags = dbc->getUInt32(row, 25);
        }
        if (fc > 26) {
            const float minDist = dbc->getFloat(row, 26);
            if (std::isfinite(minDist) && minDist > 0.0f) {
                rec.minDistance = minDist;
            }
        }
        if (fc > 27) {
            const float maxDist = dbc->getFloat(row, 27);
            if (std::isfinite(maxDist) && maxDist > 0.0f) {
                rec.maxDistance = std::max(rec.minDistance, maxDist);
            }
        }
        if (fc > 28) {
            rec.eaxDef = dbc->getUInt32(row, 28);
        }

        if (!rec.variants.empty()) {
            if (!rec.name.empty()) {
                // Later rows in SoundEntries.dbc supersede earlier entries with the same name
                // (e.g. INTERFACESOUND_LOSTTARGETUNIT: row 900 @ 0.75 supersedes row 684 @ 1.0).
                byName_[upperAscii(rec.name)] = id;
            }
            for (const SoundVariant& v : rec.variants) {
                if (!v.path.empty()) {
                    byPath_.try_emplace(normalizeSoundPathKey(v.path), id);
                }
            }
            byId_.emplace(id, std::move(rec));
        }
    }

    if (auto uiLookup = assets->loadDBC("UISoundLookups.dbc");
        uiLookup && uiLookup->isLoaded() && uiLookup->getFieldCount() >= 3) {
        for (uint32_t row = 0; row < uiLookup->getRecordCount(); ++row) {
            const uint32_t soundId = uiLookup->getUInt32(row, 1);
            const std::string alias = uiLookup->getString(row, 2);
            if (soundId != 0 && !alias.empty() && byId_.contains(soundId)) {
                byName_[upperAscii(alias)] = soundId;
            }
        }
    }

    LOG_INFO("SoundEntryTable: loaded ", byId_.size(), " SoundEntries rows, ",
             byName_.size(), " named lookups");
    return true;
}

const SoundEntryRecord* SoundEntryTable::findById(pipeline::AssetManager* assets, uint32_t soundId) {
    if (soundId == 0 || !ensureLoaded(assets)) return nullptr;
    const auto it = byId_.find(soundId);
    return (it != byId_.end()) ? &it->second : nullptr;
}

const SoundEntryRecord* SoundEntryTable::findByName(pipeline::AssetManager* assets, const std::string& name) {
    if (name.empty() || !ensureLoaded(assets)) return nullptr;
    const auto nit = byName_.find(upperAscii(name));
    if (nit == byName_.end()) return nullptr;
    const auto it = byId_.find(nit->second);
    return (it != byId_.end()) ? &it->second : nullptr;
}

const SoundEntryRecord* SoundEntryTable::findByPath(pipeline::AssetManager* assets, const std::string& path) {
    if (path.empty() || !ensureLoaded(assets)) return nullptr;
    return findLoadedByPath(path);
}

const SoundEntryRecord* SoundEntryTable::findLoadedById(uint32_t soundId) const {
    if (!loaded_ || soundId == 0) return nullptr;
    const auto it = byId_.find(soundId);
    return (it != byId_.end()) ? &it->second : nullptr;
}

const SoundEntryRecord* SoundEntryTable::findLoadedByPath(const std::string& path) const {
    if (!loaded_ || path.empty()) return nullptr;
    const auto pit = byPath_.find(normalizeSoundPathKey(path));
    if (pit == byPath_.end()) return nullptr;
    const auto it = byId_.find(pit->second);
    return (it != byId_.end()) ? &it->second : nullptr;
}

std::string SoundEntryTable::pickExistingPath(pipeline::AssetManager* assets, const SoundEntryRecord& entry) {
    if (entry.variants.empty()) return {};

    std::vector<const SoundVariant*> available;
    available.reserve(entry.variants.size());
    uint32_t totalWeight = 0;
    for (const SoundVariant& v : entry.variants) {
        if (!assets || assets->fileExists(v.path)) {
            available.push_back(&v);
            totalWeight += std::max(1u, v.weight);
        }
    }

    if (available.empty()) return {};
    if (available.size() == 1 || totalWeight == 0) return available.front()->path;

    std::uniform_int_distribution<uint32_t> dist(0, totalWeight - 1);
    uint32_t roll = dist(rng_);
    for (const SoundVariant* v : available) {
        const uint32_t w = std::max(1u, v->weight);
        if (roll < w) return v->path;
        roll -= w;
    }
    return available.back()->path;
}

bool SoundEntryTable::prepareEntryPlayback(pipeline::AssetManager* assets, uint32_t soundId,
                                           float volumeScale, float pitch, bool stoppable,
                                           uint32_t categoryOverride, SoundLoopMode loopMode,
                                           const glm::vec3* worldPos3D,
                                           const SoundEntryRecord*& outRec, std::string& outPath,
                                           float& outVolume, float& outPitch, uint32_t& outCategory,
                                           bool& outLoop) {
    if (volumeScale <= 0.0f) return false;
    const SoundEntryRecord* rec = findById(assets, soundId);
    if (!rec) return false;

    auto& engine = AudioEngine::instance();
    // Cull 3D sounds beyond DistanceCutoff (0x461c10) BEFORE archive lookups,
    // RNG rolls, concurrency checks, or loop-mode diagnostics.
    if (worldPos3D != nullptr) {
        const glm::vec3 rel = *worldPos3D - engine.getListenerPosition();
        if (glm::dot(rel, rel) >= rec->maxDistance * rec->maxDistance) {
            return false;
        }
    }

    const uint32_t category = (categoryOverride == kAutoCategory)
        ? categoryForSoundType(rec->soundType)
        : categoryOverride;
    if (!engine.canStartCategory(category)) {
        return false;
    }
    if ((rec->flags & SoundEntryFlags::kNoDuplicates) != 0u && engine.isSoundEntryPlaying(rec->id)) {
        return false;
    }

    const std::string path = pickExistingPath(assets, *rec);
    if (path.empty()) return false;

    // Wow.exe 0x462515..0x462558: vol = rec->volume * volumeScale + (VARY_VOLUME ? rand[-0.15, +0.15] : 0)
    float vol = rec->volume * volumeScale;
    if ((rec->flags & SoundEntryFlags::kVaryVolume) != 0u) {
        constexpr float kVolJitterMin = -0.15000000596046448f; // 0x897194
        constexpr float kVolJitterMax =  0.15000000596046448f; // 0x88d5f0
        std::uniform_real_distribution<float> volDist(kVolJitterMin, kVolJitterMax);
        vol += volDist(rng_);
    }
    vol = std::clamp(vol, 0.0f, 1.0f);
    if (vol <= 0.0f) return false;

    // Wow.exe 0x46255d..0x46258b: VARY_PITCH multiplies pitch by rand[0.85, 1.15]
    float effectivePitch = pitch;
    if ((rec->flags & SoundEntryFlags::kVaryPitch) != 0u) {
        constexpr float kPitchJitterMin = 0.8500000238418579f; // 0x897190
        constexpr float kPitchJitterMax = 1.149999976158142f;  // 0x89718c
        std::uniform_real_distribution<float> pitchDist(kPitchJitterMin, kPitchJitterMax);
        effectivePitch *= pitchDist(rng_);
    }

    bool wantsLoop = false;
    switch (loopMode) {
        case SoundLoopMode::ForceOneShot:
            wantsLoop = false;
            break;
        case SoundLoopMode::ForceLoop:
            wantsLoop = true;
            break;
        case SoundLoopMode::FromEntryFlags:
            wantsLoop = (rec->flags & SoundEntryFlags::kLooping) != 0u;
            break;
    }
    if (wantsLoop && !stoppable) {
        LOG_DEBUG("SoundEntryTable: looping SoundEntry ", rec->id, " ('", rec->name,
                  "') played without stoppable handle; playing one-shot");
    }

    outRec = rec;
    outPath = path;
    outVolume = vol;
    outPitch = effectivePitch;
    outCategory = category;
    outLoop = stoppable && wantsLoop;
    return true;
}

bool SoundEntryTable::play2D(pipeline::AssetManager* assets, uint32_t soundId,
                             float volumeScale, float pitch, uint32_t categoryOverride,
                             SoundLoopMode loopMode) {
    const SoundEntryRecord* rec = nullptr;
    std::string path;
    float vol = 0.0f;
    float effectivePitch = 1.0f;
    uint32_t category = SESoundCategory::kSfx;
    bool loop = false;
    if (!prepareEntryPlayback(assets, soundId, volumeScale, pitch, /*stoppable=*/false,
                              categoryOverride, loopMode, /*worldPos3D=*/nullptr,
                              rec, path, vol, effectivePitch, category, loop)) {
        return false;
    }
    auto data = assets->readFile(path);
    if (data.empty()) return false;
    const SfxPlayOptions opts{
        .soundEntryId = rec->id,
        .soundEntryFlags = rec->flags,
        .category = category,
        .loop = false,
        .eaxDef = rec->eaxDef,
        .envBusMode = usesEnvironmentBus(*rec, category)
            ? EnvironmentBusMode::ForceWet
            : EnvironmentBusMode::ForceDry,
    };
    return AudioEngine::instance().playSound2D(data, vol, effectivePitch, opts, path);
}

uint32_t SoundEntryTable::play2DStoppable(pipeline::AssetManager* assets, uint32_t soundId,
                                          float volumeScale, float pitch, uint32_t categoryOverride,
                                          SoundLoopMode loopMode) {
    const SoundEntryRecord* rec = nullptr;
    std::string path;
    float vol = 0.0f;
    float effectivePitch = 1.0f;
    uint32_t category = SESoundCategory::kSfx;
    bool loop = false;
    if (!prepareEntryPlayback(assets, soundId, volumeScale, pitch, /*stoppable=*/true,
                              categoryOverride, loopMode, /*worldPos3D=*/nullptr,
                              rec, path, vol, effectivePitch, category, loop)) {
        return 0;
    }
    auto data = assets->readFile(path);
    if (data.empty()) return 0;
    const SfxPlayOptions opts{
        .soundEntryId = rec->id,
        .soundEntryFlags = rec->flags,
        .category = category,
        .loop = loop,
        .eaxDef = rec->eaxDef,
        .envBusMode = usesEnvironmentBus(*rec, category)
            ? EnvironmentBusMode::ForceWet
            : EnvironmentBusMode::ForceDry,
    };
    return AudioEngine::instance().playSound2DStoppable(data, vol, effectivePitch, opts, path);
}

bool SoundEntryTable::play3D(pipeline::AssetManager* assets, uint32_t soundId,
                             const glm::vec3& position, float volumeScale, float pitch,
                             uint32_t categoryOverride, SoundLoopMode loopMode) {
    const SoundEntryRecord* rec = nullptr;
    std::string path;
    float vol = 0.0f;
    float effectivePitch = 1.0f;
    uint32_t category = SESoundCategory::kSfx;
    bool loop = false;
    if (!prepareEntryPlayback(assets, soundId, volumeScale, pitch, /*stoppable=*/false,
                              categoryOverride, loopMode, &position,
                              rec, path, vol, effectivePitch, category, loop)) {
        return false;
    }
    auto data = assets->readFile(path);
    if (data.empty()) return false;
    const SfxPlayOptions opts{
        .soundEntryId = rec->id,
        .soundEntryFlags = rec->flags,
        .category = category,
        .loop = false,
        .eaxDef = rec->eaxDef,
        .envBusMode = usesEnvironmentBus(*rec, category)
            ? EnvironmentBusMode::ForceWet
            : EnvironmentBusMode::ForceDry,
    };
    return AudioEngine::instance().playSound3D(
        data, position, vol, effectivePitch, rec->maxDistance, rec->minDistance, opts, path);
}

uint32_t SoundEntryTable::play3DStoppable(pipeline::AssetManager* assets, uint32_t soundId,
                                          const glm::vec3& position, float volumeScale, float pitch,
                                          uint32_t categoryOverride, SoundLoopMode loopMode) {
    const SoundEntryRecord* rec = nullptr;
    std::string path;
    float vol = 0.0f;
    float effectivePitch = 1.0f;
    uint32_t category = SESoundCategory::kSfx;
    bool loop = false;
    if (!prepareEntryPlayback(assets, soundId, volumeScale, pitch, /*stoppable=*/true,
                              categoryOverride, loopMode, &position,
                              rec, path, vol, effectivePitch, category, loop)) {
        return 0;
    }
    auto data = assets->readFile(path);
    if (data.empty()) return 0;
    const SfxPlayOptions opts{
        .soundEntryId = rec->id,
        .soundEntryFlags = rec->flags,
        .category = category,
        .loop = loop,
        .eaxDef = rec->eaxDef,
        .envBusMode = usesEnvironmentBus(*rec, category)
            ? EnvironmentBusMode::ForceWet
            : EnvironmentBusMode::ForceDry,
    };
    return AudioEngine::instance().playSound3DStoppable(
        data, position, vol, effectivePitch, rec->maxDistance, rec->minDistance, opts, path);
}

bool SoundEntryTable::playUnit(pipeline::AssetManager* assets, uint32_t soundId,
                               const glm::vec3& unitBasePos, bool isLocalPlayer,
                               bool listenerAtCharacter, float volumeScale, float pitch,
                               uint32_t categoryOverride) {
    if (isLocalPlayer && listenerAtCharacter) {
        return play2D(assets, soundId, volumeScale * kLocalPlayer2DGain, pitch, categoryOverride);
    }
    const glm::vec3 pos = unitBasePos + glm::vec3(0.0f, 0.0f, kListenerCharacterZOffset);
    return play3D(assets, soundId, pos, volumeScale, pitch, categoryOverride);
}

uint32_t SoundEntryTable::playUnitStoppable(pipeline::AssetManager* assets, uint32_t soundId,
                                            const glm::vec3& unitBasePos, bool isLocalPlayer,
                                            bool listenerAtCharacter, float volumeScale, float pitch,
                                            uint32_t categoryOverride) {
    if (isLocalPlayer && listenerAtCharacter) {
        return play2DStoppable(assets, soundId, volumeScale * kLocalPlayer2DGain, pitch, categoryOverride);
    }
    const glm::vec3 pos = unitBasePos + glm::vec3(0.0f, 0.0f, kListenerCharacterZOffset);
    return play3DStoppable(assets, soundId, pos, volumeScale, pitch, categoryOverride);
}

} // namespace audio
} // namespace wowee
