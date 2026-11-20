#define MINIAUDIO_IMPLEMENTATION
#include "audio/audio_engine.hpp"
#include "core/logger.hpp"
#include "pipeline/asset_manager.hpp"


#include "../../extern/miniaudio.h"

#include <cstring>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <shared_mutex>
#include <unordered_map>

namespace wowee {
namespace audio {

namespace {

/// Owns malloc'd storage for a miniaudio object and unwinds it correctly on
/// any early return.
///
/// miniaudio initialises into caller-provided storage, so teardown is two
/// steps -- uninit the object, then free the storage -- and the uninit must
/// not run on storage that was never initialised. That is the distinction the
/// hand-written cleanup here used to encode by having a different free()
/// sequence at each of five early returns per function, duplicated across two
/// functions. Every one of them had to be right, and nothing checked that.
///
/// Call markInitialised() once the matching ma_*_init has succeeded, and
/// release() to hand the pointer to activeSounds_, which owns it from then on.
template <typename T, void (*Uninit)(T*)>
class MaStorage {
public:
    MaStorage() : ptr_(static_cast<T*>(std::malloc(sizeof(T)))) {}
    ~MaStorage() {
        if (ptr_ != nullptr) {
            if (initialised_) {
                Uninit(ptr_);
            }
            std::free(ptr_);
        }
    }

    MaStorage(const MaStorage&) = delete;
    MaStorage& operator=(const MaStorage&) = delete;

    explicit operator bool() const { return ptr_ != nullptr; }
    T* get() const { return ptr_; }
    void markInitialised() { initialised_ = true; }

    /// Give up ownership. The caller frees it from here on.
    T* release() {
        T* p = ptr_;
        ptr_ = nullptr;
        initialised_ = false;
        return p;
    }

private:
    T* ptr_ = nullptr;
    bool initialised_ = false;
};

using AudioBufferStorage = MaStorage<ma_audio_buffer, ma_audio_buffer_uninit>;
using SoundStorage = MaStorage<ma_sound, ma_sound_uninit>;

struct DecodedWavCacheEntry {
    ma_format format = ma_format_unknown;
    ma_uint32 channels = 0;
    ma_uint32 sampleRate = 0;
    ma_uint64 frames = 0;
    std::shared_ptr<std::vector<uint8_t>> pcmData;
};

static std::unordered_map<uint64_t, DecodedWavCacheEntry> gDecodedWavCache;
// Protects gDecodedWavCache - shared_lock for reads, unique_lock for writes.
// Required because playSound2D() can be called from multiple threads
// (main thread, async loaders, animation callbacks).
static std::shared_mutex gDecodedWavCacheMutex;

static uint64_t makeWavCacheKey(const std::vector<uint8_t>& wavData) {
    // FNV-1a over the first 256 bytes + last 256 bytes + total size.
    // Full-content hash would be correct but slow for large files; sampling the
    // edges catches virtually all distinct files while keeping cost O(1).
    constexpr uint64_t FNV_OFFSET = 14695981039346656037ull;
    constexpr uint64_t FNV_PRIME  = 1099511628211ull;
    uint64_t h = FNV_OFFSET;
    auto mix = [&](uint8_t b) { h ^= b; h *= FNV_PRIME; };

    const size_t sz = wavData.size();
    const size_t head = std::min(sz, size_t(256));
    for (size_t i = 0; i < head; ++i) mix(wavData[i]);
    if (sz > 256) {
        const size_t tail_start = sz > 512 ? sz - 256 : 256;
        for (size_t i = tail_start; i < sz; ++i) mix(wavData[i]);
    }
    // Mix in the total size so files with identical head/tail but different
    // lengths still produce different keys.
    for (int s = 0; s < 8; ++s) mix(static_cast<uint8_t>(sz >> (s * 8)));
    return h;
}

static bool decodeWavCached(const std::vector<uint8_t>& wavData, DecodedWavCacheEntry& out) {
    if (wavData.empty()) return false;

    const uint64_t key = makeWavCacheKey(wavData);

    // Fast path: shared (read) lock for cache hits - allows concurrent lookups.
    {
        std::shared_lock<std::shared_mutex> readLock(gDecodedWavCacheMutex);
        if (auto it = gDecodedWavCache.find(key); it != gDecodedWavCache.end()) {
            out = it->second;
            return true;
        }
    }

    ma_decoder decoder;
    ma_decoder_config decoderConfig = ma_decoder_config_init_default();
    ma_result result = ma_decoder_init_memory(
        wavData.data(),
        wavData.size(),
        &decoderConfig,
        &decoder
    );
    if (result != MA_SUCCESS) {
        LOG_ERROR("AudioEngine: Failed to decode WAV data (", wavData.size(), " bytes): error ", result);
        return false;
    }

    ma_uint64 totalFrames = 0;
    result = ma_decoder_get_length_in_pcm_frames(&decoder, &totalFrames);
    if (result != MA_SUCCESS) totalFrames = 0;

    ma_format format = decoder.outputFormat;
    ma_uint32 channels = decoder.outputChannels;
    ma_uint32 sampleRate = decoder.outputSampleRate;
    ma_uint64 maxFrames = sampleRate * 60;
    if (totalFrames == 0 || totalFrames > maxFrames) totalFrames = maxFrames;

    size_t bufferSize = totalFrames * channels * ma_get_bytes_per_sample(format);
    auto pcmData = std::make_shared<std::vector<uint8_t>>(bufferSize);
    ma_uint64 framesRead = 0;
    result = ma_decoder_read_pcm_frames(&decoder, pcmData->data(), totalFrames, &framesRead);
    ma_decoder_uninit(&decoder);
    if ((result != MA_SUCCESS && result != MA_AT_END) || framesRead == 0) {
        LOG_ERROR("AudioEngine: Failed to read frames from WAV: error ", result, ", framesRead=", framesRead);
        return false;
    }

    pcmData->resize(framesRead * channels * ma_get_bytes_per_sample(format));

    DecodedWavCacheEntry entry;
    entry.format = format;
    entry.channels = channels;
    entry.sampleRate = sampleRate;
    entry.frames = framesRead;
    entry.pcmData = pcmData;
    // Evict oldest half when cache grows too large. 256 entries ≈ 50-100 MB of decoded
    // PCM data depending on file lengths; halving keeps memory bounded while retaining
    // recently-heard sounds (footsteps, UI clicks, combat hits) for instant replay.
    // Exclusive (write) lock - only one thread can evict + insert.
    {
        std::lock_guard<std::shared_mutex> writeLock(gDecodedWavCacheMutex);
        // Re-check in case another thread inserted while we were decoding.
        if (auto it = gDecodedWavCache.find(key); it != gDecodedWavCache.end()) {
            out = it->second;
            return true;
        }
        constexpr size_t kMaxCachedSounds = 256;
        if (gDecodedWavCache.size() >= kMaxCachedSounds) {
            auto it = gDecodedWavCache.begin();
            std::advance(it, gDecodedWavCache.size() / 2);
            gDecodedWavCache.erase(gDecodedWavCache.begin(), it);
        }
        gDecodedWavCache.emplace(key, entry);
    }
    out = entry;
    return true;
}

} // namespace

AudioEngine& AudioEngine::instance() {
    static AudioEngine instance;
    return instance;
}

AudioEngine::AudioEngine() = default;

AudioEngine::~AudioEngine() {
    shutdown();
}

bool AudioEngine::initialize() {
    if (initialized_) {
        LOG_WARNING("AudioEngine already initialized");
        return true;
    }

    // Allocate miniaudio engine
    engine_ = new ma_engine();

    // The default config, but with the device's callback our own: it mixes the
    // same way miniaudio's does and also feeds the screen recorder's tap.
    ma_engine_config engineConfig = ma_engine_config_init();
    engineConfig.dataCallback = &AudioEngine::deviceDataCallback;
    ma_result result = ma_engine_init(&engineConfig, engine_);
    if (result != MA_SUCCESS) {
        LOG_ERROR("Failed to initialize miniaudio engine: ", result);
        delete engine_;
        engine_ = nullptr;
        return false;
    }

    // Set default master volume
    ma_engine_set_volume(engine_, masterVolume_);

    // Log audio backend info
    ma_backend backend = ma_engine_get_device(engine_)->pContext->backend;
    const char* backendName = "unknown";
    switch (backend) {
        case ma_backend_wasapi: backendName = "WASAPI"; break;
        case ma_backend_dsound: backendName = "DirectSound"; break;
        case ma_backend_winmm: backendName = "WinMM"; break;
        case ma_backend_coreaudio: backendName = "CoreAudio"; break;
        case ma_backend_sndio: backendName = "sndio"; break;
        case ma_backend_audio4: backendName = "audio(4)"; break;
        case ma_backend_oss: backendName = "OSS"; break;
        case ma_backend_pulseaudio: backendName = "PulseAudio"; break;
        case ma_backend_alsa: backendName = "ALSA"; break;
        case ma_backend_jack: backendName = "JACK"; break;
        case ma_backend_aaudio: backendName = "AAudio"; break;
        case ma_backend_opensl: backendName = "OpenSL|ES"; break;
        case ma_backend_webaudio: backendName = "WebAudio"; break;
        case ma_backend_custom: backendName = "Custom"; break;
        case ma_backend_null: backendName = "Null (no output)"; break;
        default: break;
    }

    initialized_ = true;
    headless_ = false;
    LOG_INFO("AudioEngine initialized (miniaudio, backend: ", backendName, ")");
    return true;
}

bool AudioEngine::initializeHeadless(uint32_t sampleRate, uint32_t channels) {
    if (initialized_) {
        shutdown();
    }

    engine_ = new ma_engine();
    ma_engine_config engineConfig = ma_engine_config_init();
    engineConfig.noDevice = MA_TRUE;
    engineConfig.channels = std::max(1u, channels);
    engineConfig.sampleRate = std::max(8000u, sampleRate);
    ma_result result = ma_engine_init(&engineConfig, engine_);
    if (result != MA_SUCCESS) {
        LOG_ERROR("Failed to initialize headless miniaudio engine: ", result);
        delete engine_;
        engine_ = nullptr;
        return false;
    }

    ma_engine_set_volume(engine_, suspended_ ? 0.0f : masterVolume_);
    initialized_ = true;
    headless_ = true;
    LOG_INFO("AudioEngine initialized in headless test mode (", engineConfig.sampleRate, " Hz, ",
             engineConfig.channels, " ch)");
    return true;
}

std::string AudioEngine::getOutputDeviceName() const {
    if (!initialized_ || !engine_) return {};
    if (headless_) return "Headless Test Mixer";
    const ma_device* device = ma_engine_get_device(const_cast<ma_engine*>(engine_));
    if (!device || device->playback.name[0] == '\0') return {};
    return device->playback.name;
}

void AudioEngine::shutdown() {
    if (!initialized_) {
        return;
    }

    // Stop music
    stopMusic();

    // Clean up all active sounds
    for (auto& activeSound : activeSounds_) {
        ma_sound_uninit(activeSound.sound);
        std::free(activeSound.sound);
        ma_audio_buffer* buffer = static_cast<ma_audio_buffer*>(activeSound.buffer);
        ma_audio_buffer_uninit(buffer);
        std::free(buffer);
    }
    activeSounds_.clear();

    capturing_.store(false, std::memory_order_release);
    if (engine_) {
        ma_engine_uninit(engine_);
        delete engine_;
        engine_ = nullptr;
    }
    // The device is gone, so nothing writes to the ring any more.
    if (captureRing_) {
        auto* ring = static_cast<ma_pcm_rb*>(captureRing_);
        ma_pcm_rb_uninit(ring);
        delete ring;
        captureRing_ = nullptr;
    }

    initialized_ = false;
    headless_ = false;
    LOG_INFO("AudioEngine shutdown");
}

void AudioEngine::setMasterVolume(float volume) {
    masterVolume_ = glm::clamp(volume, 0.0f, 1.0f);
    if (engine_) {
        if (!suspended_) ma_engine_set_volume(engine_, masterVolume_);
    }
}
void AudioEngine::setSuspended(bool suspended) {
    if (suspended_ == suspended) return;
    suspended_ = suspended;
    // The engine's own level is the one thing that changes; masterVolume_ is
    // left alone so the slider and the resume both still read it.
    if (engine_) ma_engine_set_volume(engine_, suspended_ ? 0.0f : masterVolume_);
}


void AudioEngine::setListenerPosition(const glm::vec3& position) {
    listenerPosition_ = position;
    if (engine_) {
        ma_engine_listener_set_position(engine_, 0, position.x, position.y, position.z);
    }
}

void AudioEngine::setListenerOrientation(const glm::vec3& forward, const glm::vec3& up) {
    listenerForward_ = forward;
    listenerUp_ = up;
    if (engine_) {
        ma_engine_listener_set_direction(engine_, 0, forward.x, forward.y, forward.z);
        ma_engine_listener_set_world_up(engine_, 0, up.x, up.y, up.z);
    }
}

uint32_t AudioEngine::startSound2DInternal(const std::vector<uint8_t>& wavData, float volume,
                                           float pitch, bool stoppable,
                                           const std::string& debugPath) {
    LOG_INFO("sfx2d: bytes=", wavData.size(), " vol=", volume);
    const float clampedPitch = glm::clamp(pitch, 0.25f, 4.0f);
    auto notify = [&](bool started, const DecodedWavCacheEntry& dec) {
        if (!playbackObserver_) return;
        PlaybackEvent ev;
        ev.path = debugPath;
        ev.byteSize = wavData.size();
        ev.decodedFrames = dec.frames;
        ev.sampleRate = dec.sampleRate;
        ev.volume = volume;
        ev.pitch = clampedPitch;
        ev.spatial = false;
        ev.music = false;
        ev.started = started;
        playbackObserver_(ev);
    };

    DecodedWavCacheEntry emptyDec;
    if (!initialized_ || !engine_ || wavData.empty() || masterVolume_ <= 0.0f || volume <= 0.0f) {
        notify(false, emptyDec);
        return 0;
    }

    DecodedWavCacheEntry decoded;
    if (!decodeWavCached(wavData, decoded) || !decoded.pcmData || decoded.frames == 0) {
        notify(false, emptyDec);
        return 0;
    }

    ma_audio_buffer_config bufferConfig = ma_audio_buffer_config_init(
        decoded.format,
        decoded.channels,
        decoded.frames,
        decoded.pcmData->data(),
        nullptr
    );
    bufferConfig.sampleRate = decoded.sampleRate;

    AudioBufferStorage audioBuffer;
    if (!audioBuffer || ma_audio_buffer_init(&bufferConfig, audioBuffer.get()) != MA_SUCCESS) {
        notify(false, decoded);
        return 0;
    }
    audioBuffer.markInitialised();

    const bool customPitch = std::abs(clampedPitch - 1.0f) > 1e-3f;
    const ma_uint32 flags = static_cast<ma_uint32>(MA_SOUND_FLAG_DECODE) |
                            static_cast<ma_uint32>(MA_SOUND_FLAG_ASYNC) |
                            static_cast<ma_uint32>(MA_SOUND_FLAG_NO_SPATIALIZATION) |
                            (customPitch ? 0u : static_cast<ma_uint32>(MA_SOUND_FLAG_NO_PITCH));

    SoundStorage sound;
    if (!sound ||
        ma_sound_init_from_data_source(engine_, audioBuffer.get(), flags, nullptr, sound.get()) != MA_SUCCESS) {
        notify(false, decoded);
        return 0;
    }
    sound.markInitialised();

    ma_sound_set_volume(sound.get(), volume);
    if (customPitch) {
        ma_sound_set_pitch(sound.get(), clampedPitch);
    }
    if (ma_sound_start(sound.get()) != MA_SUCCESS) {
        notify(false, decoded);
        return 0;
    }

    uint32_t id = 0;
    if (stoppable) {
        id = nextSoundId_++;
        if (nextSoundId_ == 0) nextSoundId_ = 1;
    }
    activeSounds_.push_back({sound.release(), audioBuffer.release(), decoded.pcmData, id});
    notify(true, decoded);
    return stoppable ? id : 1u;
}

bool AudioEngine::playSound2D(const std::vector<uint8_t>& wavData, float volume, float pitch) {
    return startSound2DInternal(wavData, volume, pitch, false, {}) != 0;
}

uint32_t AudioEngine::playSound2DStoppable(const std::vector<uint8_t>& wavData, float volume, float pitch) {
    return startSound2DInternal(wavData, volume, pitch, true, {});
}

uint32_t AudioEngine::playSound2DStoppable(const std::string& mpqPath, float volume, float pitch) {
    LOG_INFO("sfx: ", mpqPath, " vol=", volume);
    if (!assetManager_) {
        LOG_WARNING("AudioEngine::playSound2DStoppable(path): no AssetManager set");
        return 0;
    }
    auto data = assetManager_->readFile(mpqPath);
    if (data.empty()) {
        LOG_WARNING("AudioEngine::playSound2DStoppable: failed to load '", mpqPath, "'");
        return 0;
    }
    return startSound2DInternal(data, volume, pitch, true, mpqPath);
}

void AudioEngine::stopSound(uint32_t id) {
    if (id == 0) return;
    for (auto it = activeSounds_.begin(); it != activeSounds_.end(); ++it) {
        if (it->id == id) {
            ma_sound_stop(it->sound);
            ma_sound_uninit(it->sound);
            std::free(it->sound);
            ma_audio_buffer* buffer = static_cast<ma_audio_buffer*>(it->buffer);
            ma_audio_buffer_uninit(buffer);
            std::free(buffer);
            activeSounds_.erase(it);
            return;
        }
    }
}

bool AudioEngine::playSound2D(const std::string& mpqPath, float volume, float pitch) {
    LOG_INFO("sfx: ", mpqPath, " vol=", volume);
    if (!assetManager_) {
        LOG_WARNING("AudioEngine::playSound2D(path): no AssetManager set");
        return false;
    }
    auto data = assetManager_->readFile(mpqPath);
    if (data.empty()) {
        LOG_WARNING("AudioEngine::playSound2D: failed to load '", mpqPath, "'");
        return false;
    }
    return startSound2DInternal(data, volume, pitch, false, mpqPath) != 0;
}

uint32_t AudioEngine::startSound3DInternal(const std::vector<uint8_t>& wavData, const glm::vec3& position,
                                           float volume, float pitch, float maxDistance,
                                           float minDistance, bool stoppable,
                                           const std::string& debugPath) {
    const float clampedPitch = glm::clamp(pitch, 0.25f, 4.0f);
    const float safeMinDist = std::max(0.1f, minDistance);
    const float safeMaxDist = std::max(safeMinDist, maxDistance);
    auto notify = [&](bool started, const DecodedWavCacheEntry& dec) {
        if (!playbackObserver_) return;
        PlaybackEvent ev;
        ev.path = debugPath;
        ev.byteSize = wavData.size();
        ev.decodedFrames = dec.frames;
        ev.sampleRate = dec.sampleRate;
        ev.volume = volume;
        ev.pitch = clampedPitch;
        ev.spatial = true;
        ev.music = false;
        ev.position = position;
        ev.minDistance = safeMinDist;
        ev.maxDistance = safeMaxDist;
        ev.started = started;
        playbackObserver_(ev);
    };

    DecodedWavCacheEntry emptyDec;
    if (!initialized_ || !engine_ || wavData.empty() || masterVolume_ <= 0.0f || volume <= 0.0f) {
        notify(false, emptyDec);
        return 0;
    }

    DecodedWavCacheEntry decoded;
    if (!decodeWavCached(wavData, decoded) || !decoded.pcmData || decoded.frames == 0) {
        notify(false, emptyDec);
        return 0;
    }

    ma_audio_buffer_config bufferConfig = ma_audio_buffer_config_init(
        decoded.format,
        decoded.channels,
        decoded.frames,
        decoded.pcmData->data(),
        nullptr
    );
    bufferConfig.sampleRate = decoded.sampleRate;

    AudioBufferStorage audioBuffer;
    if (!audioBuffer || ma_audio_buffer_init(&bufferConfig, audioBuffer.get()) != MA_SUCCESS) {
        notify(false, decoded);
        return 0;
    }
    audioBuffer.markInitialised();

    SoundStorage sound;
    if (!sound ||
        ma_sound_init_from_data_source(engine_, audioBuffer.get(),
                                       MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_ASYNC,
                                       nullptr, sound.get()) != MA_SUCCESS) {
        notify(false, decoded);
        return 0;
    }
    sound.markInitialised();

    ma_sound_set_position(sound.get(), position.x, position.y, position.z);
    ma_sound_set_volume(sound.get(), volume);
    ma_sound_set_pitch(sound.get(), clampedPitch);
    ma_sound_set_attenuation_model(sound.get(), ma_attenuation_model_inverse);
    ma_sound_set_min_gain(sound.get(), 0.0f);
    ma_sound_set_max_gain(sound.get(), 1.0f);
    ma_sound_set_min_distance(sound.get(), safeMinDist);
    ma_sound_set_max_distance(sound.get(), safeMaxDist);
    ma_sound_set_rolloff(sound.get(), 1.0f);

    if (ma_sound_start(sound.get()) != MA_SUCCESS) {
        notify(false, decoded);
        return 0;
    }

    uint32_t id = 0;
    if (stoppable) {
        id = nextSoundId_++;
        if (nextSoundId_ == 0) nextSoundId_ = 1;
    }
    activeSounds_.push_back({sound.release(), audioBuffer.release(), decoded.pcmData, id});
    notify(true, decoded);
    return stoppable ? id : 1u;
}

bool AudioEngine::playSound3D(const std::vector<uint8_t>& wavData, const glm::vec3& position,
                              float volume, float pitch, float maxDistance, float minDistance) {
    return startSound3DInternal(wavData, position, volume, pitch, maxDistance, minDistance, false, {}) != 0;
}

bool AudioEngine::playSound3D(const std::string& mpqPath, const glm::vec3& position,
                              float volume, float pitch, float maxDistance, float minDistance) {
    if (!assetManager_) {
        LOG_WARNING("AudioEngine::playSound3D(path): no AssetManager set");
        return false;
    }
    auto data = assetManager_->readFile(mpqPath);
    if (data.empty()) {
        LOG_WARNING("AudioEngine::playSound3D: failed to load '", mpqPath, "'");
        return false;
    }
    return startSound3DInternal(data, position, volume, pitch, maxDistance, minDistance, false, mpqPath) != 0;
}

uint32_t AudioEngine::playSound3DStoppable(const std::vector<uint8_t>& wavData, const glm::vec3& position,
                                           float volume, float pitch, float maxDistance, float minDistance) {
    return startSound3DInternal(wavData, position, volume, pitch, maxDistance, minDistance, true, {});
}

uint32_t AudioEngine::playSound3DStoppable(const std::string& mpqPath, const glm::vec3& position,
                                           float volume, float pitch, float maxDistance, float minDistance) {
    if (!assetManager_) {
        LOG_WARNING("AudioEngine::playSound3DStoppable(path): no AssetManager set");
        return 0;
    }
    auto data = assetManager_->readFile(mpqPath);
    if (data.empty()) {
        LOG_WARNING("AudioEngine::playSound3DStoppable: failed to load '", mpqPath, "'");
        return 0;
    }
    return startSound3DInternal(data, position, volume, pitch, maxDistance, minDistance, true, mpqPath);
}

bool AudioEngine::playMusic(std::shared_ptr<const std::vector<uint8_t>> musicData,
                            float volume, bool loop) {
    if (!initialized_ || !engine_ || !musicData || musicData->empty()) {
        return false;
    }

    LOG_INFO("AudioEngine::playMusic - data size: ", musicData->size(), " bytes, volume: ", volume);

    // Stop any currently playing music
    stopMusic();

    // Keep the encoded bytes alive for as long as miniaudio's decoder streams them.
    musicData_ = std::move(musicData);
    musicVolume_ = volume;

    // Create decoder from memory (for streaming MP3/OGG)
    ma_decoder* decoder = new ma_decoder();
    ma_decoder_config decoderConfig = ma_decoder_config_init_default();
    ma_result result = ma_decoder_init_memory(
        musicData_->data(),
        musicData_->size(),
        &decoderConfig,
        decoder
    );

    if (result != MA_SUCCESS) {
        LOG_ERROR("Failed to create music decoder: ", result);
        delete decoder;
        musicData_.reset();
        return false;
    }

    LOG_INFO("Decoder created - format: ", decoder->outputFormat,
             ", channels: ", decoder->outputChannels,
             ", sampleRate: ", decoder->outputSampleRate);

    musicDecoder_ = decoder;

    // Create streaming sound from decoder
    musicSound_ = static_cast<ma_sound*>(std::malloc(sizeof(ma_sound)));
    if (!musicSound_) {
        ma_decoder_uninit(decoder);
        delete decoder;
        musicDecoder_ = nullptr;
        musicData_.reset();
        return false;
    }
    result = ma_sound_init_from_data_source(
        engine_,
        decoder,
        MA_SOUND_FLAG_STREAM | MA_SOUND_FLAG_NO_PITCH | MA_SOUND_FLAG_NO_SPATIALIZATION,
        nullptr,
        musicSound_
    );

    if (result != MA_SUCCESS) {
        LOG_ERROR("Failed to create music sound: ", result);
        ma_decoder_uninit(decoder);
        delete decoder;
        musicDecoder_ = nullptr;
        std::free(musicSound_);
        musicSound_ = nullptr;
        musicData_.reset();
        return false;
    }

    // Set volume and looping
    ma_sound_set_volume(musicSound_, volume);
    ma_sound_set_looping(musicSound_, loop ? MA_TRUE : MA_FALSE);

    // Start playback
    result = ma_sound_start(musicSound_);
    if (result != MA_SUCCESS) {
        LOG_ERROR("Failed to start music playback: ", result);
        ma_sound_uninit(musicSound_);
        std::free(musicSound_);
        musicSound_ = nullptr;
        ma_decoder_uninit(decoder);
        delete decoder;
        musicDecoder_ = nullptr;
        // The two failure paths above release this and this one did not, so a
        // failed start pinned the whole encoded file until the next playMusic
        // reassigned the member.
        musicData_.reset();
        return false;
    }

    LOG_INFO("Music playback started successfully - volume: ", volume,
             ", loop: ", loop,
             ", is_playing: ", ma_sound_is_playing(musicSound_));
    if (playbackObserver_) {
        PlaybackEvent ev;
        ev.byteSize = musicData_ ? musicData_->size() : 0;
        ev.sampleRate = decoder->outputSampleRate;
        ev.volume = volume;
        ev.music = true;
        ev.started = true;
        playbackObserver_(ev);
    }

    return true;
}

void AudioEngine::stopMusic() {
    if (musicSound_) {
        ma_sound_uninit(musicSound_);
        std::free(musicSound_);
        musicSound_ = nullptr;
    }
    if (musicDecoder_) {
        ma_decoder* decoder = static_cast<ma_decoder*>(musicDecoder_);
        ma_decoder_uninit(decoder);
        delete decoder;
        musicDecoder_ = nullptr;
    }
    musicData_.reset();
}

bool AudioEngine::isMusicPlaying() const {
    if (!musicSound_) {
        return false;
    }
    return ma_sound_is_playing(musicSound_) == MA_TRUE;
}

void AudioEngine::setMusicVolume(float volume) {
    musicVolume_ = glm::clamp(volume, 0.0f, 1.0f);
    if (musicSound_) {
        ma_sound_set_volume(musicSound_, musicVolume_);
    }
}

void AudioEngine::update(float deltaTime) {
    (void)deltaTime;

    if (!initialized_ || !engine_) {
        return;
    }

    // Clean up finished sounds - swap-and-pop avoids the O(N) shift that
    // vector::erase does for each removal (and the ref-count atomics in
    // ActiveSound's shared_ptr made that shift noticeably more expensive).
    for (size_t i = 0; i < activeSounds_.size(); ) {
        if (!ma_sound_is_playing(activeSounds_[i].sound)) {
            ma_sound_uninit(activeSounds_[i].sound);
            std::free(activeSounds_[i].sound);
            ma_audio_buffer* buffer = static_cast<ma_audio_buffer*>(activeSounds_[i].buffer);
            ma_audio_buffer_uninit(buffer);
            std::free(buffer);
            activeSounds_[i] = std::move(activeSounds_.back());
            activeSounds_.pop_back();
        } else {
            ++i;
        }
    }
}

void AudioEngine::deviceDataCallback(ma_device* device, void* out, const void* in, uint32_t frames) {
    (void)in;
    // What miniaudio's own callback does, less an Emscripten-only job pump.
    auto* engine = static_cast<ma_engine*>(device->pUserData);
    ma_engine_read_pcm_frames(engine, out, frames, nullptr);

    AudioEngine& self = instance();
    if (!self.capturing_.load(std::memory_order_acquire)) return;
    auto* ring = static_cast<ma_pcm_rb*>(self.captureRing_);
    const ma_uint32 channels = ma_engine_get_channels(engine);
    const auto* src = static_cast<const float*>(out);
    // Whatever does not fit is dropped: the recorder has fallen a second
    // behind, and the audio thread is the one thing that must never wait.
    ma_uint32 remaining = frames;
    while (remaining > 0) {
        ma_uint32 chunk = remaining;
        void* dst = nullptr;
        if (ma_pcm_rb_acquire_write(ring, &chunk, &dst) != MA_SUCCESS || chunk == 0) break;
        std::memcpy(dst, src, static_cast<size_t>(chunk) * channels * sizeof(float));
        ma_pcm_rb_commit_write(ring, chunk);
        src += static_cast<size_t>(chunk) * channels;
        remaining -= chunk;
    }
}

bool AudioEngine::beginOutputCapture() {
    if (!initialized_ || !engine_) return false;
    if (!captureRing_) {
        // A second of the device's output: the recorder drains it every few
        // tens of milliseconds, so this is room for a long stall.
        auto* ring = new ma_pcm_rb();
        if (ma_pcm_rb_init(ma_format_f32, ma_engine_get_channels(engine_),
                           ma_engine_get_sample_rate(engine_), nullptr, nullptr, ring) != MA_SUCCESS) {
            delete ring;
            LOG_WARNING("AudioEngine: could not make the capture buffer - recording without sound");
            return false;
        }
        captureRing_ = ring;
    }
    // Drop what an earlier capture left unread. The reader's side, so it is
    // safe with the audio thread writing.
    auto* ring = static_cast<ma_pcm_rb*>(captureRing_);
    const ma_uint32 stale = ma_pcm_rb_available_read(ring);
    if (stale > 0) ma_pcm_rb_seek_read(ring, stale);
    capturing_.store(true, std::memory_order_release);
    return true;
}

void AudioEngine::endOutputCapture() {
    capturing_.store(false, std::memory_order_release);
}

uint32_t AudioEngine::readCapturedOutput(float* dst, uint32_t maxFrames) {
    if (!captureRing_ || !dst || maxFrames == 0) return 0;
    auto* ring = static_cast<ma_pcm_rb*>(captureRing_);
    const ma_uint32 channels = ma_pcm_rb_get_channels(ring);
    uint32_t read = 0;
    while (read < maxFrames) {
        ma_uint32 chunk = maxFrames - read;
        void* src = nullptr;
        if (ma_pcm_rb_acquire_read(ring, &chunk, &src) != MA_SUCCESS || chunk == 0) break;
        std::memcpy(dst + static_cast<size_t>(read) * channels, src,
                    static_cast<size_t>(chunk) * channels * sizeof(float));
        ma_pcm_rb_commit_read(ring, chunk);
        read += chunk;
    }
    return read;
}

uint32_t AudioEngine::capturedOutputAvailable() const {
    if (!captureRing_) return 0;
    return ma_pcm_rb_available_read(static_cast<ma_pcm_rb*>(captureRing_));
}

uint32_t AudioEngine::getOutputChannels() const {
    return engine_ ? ma_engine_get_channels(engine_) : 0;
}

uint32_t AudioEngine::getOutputSampleRate() const {
    return engine_ ? ma_engine_get_sample_rate(engine_) : 0;
}

uint32_t AudioEngine::renderMixFrames(float* dstInterleaved, uint32_t frameCount) {
    if (!initialized_ || !engine_ || !dstInterleaved || frameCount == 0) return 0;
    const uint32_t ch = std::max(1u, ma_engine_get_channels(engine_));
    std::memset(dstInterleaved, 0, static_cast<size_t>(frameCount) * ch * sizeof(float));
    ma_uint64 framesRead = 0;
    const ma_result res = ma_engine_read_pcm_frames(engine_, dstInterleaved, frameCount, &framesRead);
    if (res != MA_SUCCESS && res != MA_AT_END) {
        return 0;
    }
    update(static_cast<float>(frameCount) / static_cast<float>(std::max(1u, ma_engine_get_sample_rate(engine_))));
    return frameCount;
}

} // namespace audio
} // namespace wowee
