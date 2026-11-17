#include "audio/footstep_manager.hpp"
#include "audio/audio_engine.hpp"
#include "audio/creature_sound_index.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "core/logger.hpp"

#include <unordered_set>

namespace wowee {
namespace audio {

FootstepManager::FootstepManager() : rng(std::random_device{}()) {}

FootstepManager::~FootstepManager() {
    shutdown();
}

bool FootstepManager::initialize(pipeline::AssetManager* assets) {
    assetManager = assets;
    if (!assetManager) return false;
    sounds_ = std::make_unique<CreatureSoundIndex>();
    if (!sounds_->load(*assetManager)) {
        sounds_.reset();
        return false;
    }
    return true;
}

void FootstepManager::shutdown() {
    sounds_.reset();
    assetManager = nullptr;
}

void FootstepManager::update(float) {}

void FootstepManager::playFootstep(uint32_t displayId, uint32_t terrainTypeId, bool wading) {
    if (!sounds_ || !AudioEngine::instance().isInitialized()) return;
    const uint32_t soundId = sounds_->footstepSound(displayId, terrainTypeId, wading);
    if (soundId == 0) {
        static std::unordered_set<uint64_t> reported;
        if (reported.insert((uint64_t{displayId} << 32) | terrainTypeId).second) {
            LOG_DEBUG("Footstep: display ", displayId, " on terrain type ", terrainTypeId,
                      wading ? " (wading)" : "", " has no FootstepTerrainLookup sound");
        }
        return;
    }
    const auto soundEntries = assetManager->loadDBC("SoundEntries.dbc");
    if (!soundEntries) {
        LOG_ERROR("Footstep: SoundEntries.dbc unavailable");
        return;
    }
    const auto entry = readSoundEntry(*soundEntries, soundId);
    if (!entry) {
        LOG_ERROR("Footstep: SoundEntries row ", soundId, " missing or names no file");
        return;
    }
    const std::string& file = pickSoundFile(*entry, rng);
    if (!AudioEngine::instance().playSound2D(file, entry->volume * volumeScale, 1.0f))
        LOG_WARNING("Footstep: could not play ", file);
}

} // namespace audio
} // namespace wowee
