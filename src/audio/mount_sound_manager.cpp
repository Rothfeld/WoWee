#include "audio/mount_sound_manager.hpp"
#include "audio/audio_engine.hpp"
#include "audio/creature_sound_index.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "core/logger.hpp"

#include <vector>

namespace wowee {
namespace audio {

MountSoundManager::MountSoundManager() = default;

MountSoundManager::~MountSoundManager() {
    shutdown();
}

bool MountSoundManager::initialize(pipeline::AssetManager* assets) {
    assetManager_ = assets;
    if (!assetManager_) {
        LOG_WARNING("Mount sound manager: no asset manager");
        return false;
    }
    sounds_ = std::make_unique<CreatureSoundIndex>();
    if (!sounds_->load(*assetManager_)) {
        sounds_.reset();
        return false;
    }
    return true;
}

void MountSoundManager::shutdown() {
    mounted_ = false;
    sounds_.reset();
    assetManager_ = nullptr;
}

void MountSoundManager::update(float) {}

void MountSoundManager::onMount(uint32_t creatureDisplayId, bool isFlying, const std::string& modelPath) {
    mounted_ = true;
    flying_ = isFlying;
    moving_ = false;
    soundData_ = sounds_ ? sounds_->soundDataForDisplay(creatureDisplayId) : 0;
    LOG_INFO("Mount sound: display ", creatureDisplayId, " sound set ", soundData_,
             " flying=", flying_, " model=", modelPath);
    if (soundData_ == 0)
        LOG_WARNING("Mount sound: display ", creatureDisplayId, " names no CreatureSoundData row");
}

void MountSoundManager::onDismount() {
    mounted_ = false;
    soundData_ = 0;
}

void MountSoundManager::playSoundEntry(uint32_t soundId, const char* what) {
    if (!assetManager_ || !AudioEngine::instance().isInitialized()) return;
    const auto soundEntries = assetManager_->loadDBC("SoundEntries.dbc");
    if (!soundEntries) {
        LOG_ERROR("Mount sound: SoundEntries.dbc unavailable");
        return;
    }
    const auto entry = readSoundEntry(*soundEntries, soundId);
    if (!entry) {
        LOG_ERROR("Mount sound: ", what, " SoundEntries row ", soundId, " missing or names no file");
        return;
    }
    const std::string& file = pickSoundFile(*entry, rng_);
    if (!AudioEngine::instance().playSound2D(file, entry->volume * volumeScale_, 1.0f))
        LOG_WARNING("Mount sound: could not play ", file);
}

void MountSoundManager::play(CreatureSoundColumn column, const char* what) {
    if (!mounted_ || !sounds_ || soundData_ == 0) return;
    const uint32_t soundId = sounds_->sound(soundData_, column);
    if (soundId == 0) {
        LOG_DEBUG("Mount sound: sound set ", soundData_, " has no ", what);
        return;
    }
    playSoundEntry(soundId, what);
}

void MountSoundManager::playRearUpSound() {
    // Which sound, if any, the original plays with the rear-up flourish is
    // not in the data this client reads; awaiting a native capture.
    LOG_DEBUG("Mount sound: rear-up has no authored sound column");
}

void MountSoundManager::playJumpSound() {
    play(CreatureSoundColumn::JumpStart, "JumpStart");
}

void MountSoundManager::playLandSound() {
    play(CreatureSoundColumn::JumpEnd, "JumpEnd");
}

void MountSoundManager::playIdleSound() {
    if (!mounted_ || moving_ || !sounds_ || soundData_ == 0) return;
    // The five Fidget columns; a fidget plays one of those the set names.
    std::vector<uint32_t> fidgets;
    for (uint32_t c = 0; c < 5; ++c) {
        const auto column = static_cast<CreatureSoundColumn>(
            static_cast<uint32_t>(CreatureSoundColumn::Fidget0) + c);
        if (const uint32_t id = sounds_->sound(soundData_, column)) fidgets.push_back(id);
    }
    if (fidgets.empty()) return;
    playSoundEntry(fidgets[std::uniform_int_distribution<size_t>(0, fidgets.size() - 1)(rng_)], "Fidget");
}

} // namespace audio
} // namespace wowee
