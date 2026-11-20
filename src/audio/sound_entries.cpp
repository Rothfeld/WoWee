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

SoundEntryTable::SoundEntryTable() : rng_(std::random_device{}()) {}

void SoundEntryTable::reset() {
    loadedAssets_ = nullptr;
    loaded_ = false;
    byId_.clear();
    byName_.clear();
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

        if (!rec.variants.empty()) {
            if (!rec.name.empty()) {
                byName_.emplace(upperAscii(rec.name), id);
            }
            byId_.emplace(id, std::move(rec));
        }
    }

    // UISoundLookups.dbc (0=ID, 1=SoundID, 2=Name): maps legacy/FrameXML alias
    // names such as TITLEENTERWORLD, LOOTWINDOWCOINSOUND, BAGMENUBUTTONPRESS
    // to SoundEntries.dbc IDs.
    if (auto uiLookup = assets->loadDBC("UISoundLookups.dbc");
        uiLookup && uiLookup->isLoaded() && uiLookup->getFieldCount() >= 3) {
        for (uint32_t row = 0; row < uiLookup->getRecordCount(); ++row) {
            const uint32_t soundId = uiLookup->getUInt32(row, 1);
            const std::string alias = uiLookup->getString(row, 2);
            if (soundId != 0 && !alias.empty() && byId_.contains(soundId)) {
                byName_.emplace(upperAscii(alias), soundId);
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

bool SoundEntryTable::play2D(pipeline::AssetManager* assets, uint32_t soundId,
                             float volumeScale, float pitch) {
    if (volumeScale <= 0.0f) return false;
    const SoundEntryRecord* rec = findById(assets, soundId);
    if (!rec) return false;
    const std::string path = pickExistingPath(assets, *rec);
    if (path.empty()) return false;
    const float vol = std::clamp(rec->volume * volumeScale, 0.0f, 1.0f);
    if (vol <= 0.0f) return false;
    auto data = assets->readFile(path);
    if (data.empty()) return false;
    return AudioEngine::instance().playSound2D(data, vol, pitch);
}

uint32_t SoundEntryTable::play2DStoppable(pipeline::AssetManager* assets, uint32_t soundId,
                                          float volumeScale, float pitch) {
    if (volumeScale <= 0.0f) return 0;
    const SoundEntryRecord* rec = findById(assets, soundId);
    if (!rec) return 0;
    const std::string path = pickExistingPath(assets, *rec);
    if (path.empty()) return 0;
    const float vol = std::clamp(rec->volume * volumeScale, 0.0f, 1.0f);
    if (vol <= 0.0f) return 0;
    auto data = assets->readFile(path);
    if (data.empty()) return 0;
    return AudioEngine::instance().playSound2DStoppable(data, vol, pitch);
}

bool SoundEntryTable::play3D(pipeline::AssetManager* assets, uint32_t soundId,
                             const glm::vec3& position, float volumeScale, float pitch) {
    if (volumeScale <= 0.0f) return false;
    const SoundEntryRecord* rec = findById(assets, soundId);
    if (!rec) return false;
    const std::string path = pickExistingPath(assets, *rec);
    if (path.empty()) return false;
    const float vol = std::clamp(rec->volume * volumeScale, 0.0f, 1.0f);
    if (vol <= 0.0f) return false;
    auto data = assets->readFile(path);
    if (data.empty()) return false;
    return AudioEngine::instance().playSound3D(
        data, position, vol, pitch, rec->maxDistance, rec->minDistance);
}

uint32_t SoundEntryTable::play3DStoppable(pipeline::AssetManager* assets, uint32_t soundId,
                                          const glm::vec3& position, float volumeScale, float pitch) {
    if (volumeScale <= 0.0f) return 0;
    const SoundEntryRecord* rec = findById(assets, soundId);
    if (!rec) return 0;
    const std::string path = pickExistingPath(assets, *rec);
    if (path.empty()) return 0;
    const float vol = std::clamp(rec->volume * volumeScale, 0.0f, 1.0f);
    if (vol <= 0.0f) return 0;
    auto data = assets->readFile(path);
    if (data.empty()) return 0;
    return AudioEngine::instance().playSound3DStoppable(
        data, position, vol, pitch, rec->maxDistance, rec->minDistance);
}

} // namespace audio
} // namespace wowee
