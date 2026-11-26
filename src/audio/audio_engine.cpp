#define MINIAUDIO_IMPLEMENTATION
#include "audio/audio_engine.hpp"
#include "audio/environment_reverb.hpp"
#include "audio/sound_entries.hpp"
#include "core/logger.hpp"
#include "pipeline/asset_manager.hpp"

#include "../../extern/miniaudio.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <iterator>
#include <limits>
#include <memory>
#include <shared_mutex>
#include <sstream>
#include <iomanip>
#include <unordered_map>

namespace wowee {
namespace audio {

namespace {

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
static std::shared_mutex gDecodedWavCacheMutex;

static uint64_t makeWavCacheKey(const std::vector<uint8_t>& wavData) {
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
    for (int s = 0; s < 8; ++s) mix(static_cast<uint8_t>(sz >> (s * 8)));
    return h;
}

static bool decodeWavCached(const std::vector<uint8_t>& wavData, DecodedWavCacheEntry& out) {
    if (wavData.empty()) return false;

    const uint64_t key = makeWavCacheKey(wavData);

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
    {
        std::lock_guard<std::shared_mutex> writeLock(gDecodedWavCacheMutex);
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

// Exact Wow.exe build 8606 concurrency caps at 0x897018[0..17]
constexpr std::array<uint32_t, SESoundCategory::kCount> kSESoundCategoryCaps = {
    0x7FFFFFFFu, //  0: SFX
    0x7FFFFFFFu, //  1: MUSIC
    0x7FFFFFFFu, //  2: AMBIENCE
    0x7FFFFFFFu, //  3: CINEMATIC
    0x7FFFFFFFu, //  4: SCRIPTSOUND
    0x7FFFFFFFu, //  5: SCRIPTMUSIC
    1u,          //  6: RACIALCINEMATIC
    1u,          //  7: VOCAL_UI (Error speech)
    2u,          //  8: ITEM_PICKUP
    1u,          //  9: NPC_GREETING
    2u,          // 10: WEAPON_SWING
    2u,          // 11: DEATH_THUD
    1u,          // 12: VOCAL_EMOTE
    6u,          // 13: FOOTSTEP_OTHER
    4u,          // 14: COMBAT_IMPACT
    1u,          // 15: SPELL_FIZZLE
    2u,          // 16: COMBAT_VOCAL
    4u,          // 17: FOOTSTEP_SELF
};

double monotonicSecondsNow() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

int64_t realtimeNsNow() {
    struct timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000ll + static_cast<int64_t>(ts.tv_nsec);
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned int>(c));
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    return out;
}

float safeJsonFloat(float v) {
    return std::isfinite(v) ? v : 0.0f;
}

void resolveSoundMetadata(uint32_t& ioSoundId, std::string& outName, const std::string& path) {
    const auto& table = SoundEntryTable::instance();
    if (ioSoundId != 0) {
        if (const SoundEntryRecord* rec = table.findLoadedById(ioSoundId)) {
            outName = rec->name;
            return;
        }
    }
    if (!path.empty()) {
        if (const SoundEntryRecord* rec = table.findLoadedByPath(path)) {
            ioSoundId = rec->id;
            outName = rec->name;
        }
    }
}

struct EnvEffectMaNode {
    ma_node_base base;
    EnvironmentReverbProcessor* processor = nullptr;
};

void envEffectNodeProcessPcmFrames(ma_node* pNode, const float** ppFramesIn, ma_uint32* pFrameCountIn,
                                   float** ppFramesOut, ma_uint32* pFrameCountOut) {
    (void)pFrameCountIn;
    auto* node = reinterpret_cast<EnvEffectMaNode*>(pNode);
    const ma_uint32 frameCount = pFrameCountOut ? *pFrameCountOut : 0u;
    const float* inBuf = (ppFramesIn && ppFramesIn[0]) ? ppFramesIn[0] : nullptr;
    float* outBuf = (ppFramesOut && ppFramesOut[0]) ? ppFramesOut[0] : nullptr;
    if (node && node->processor && outBuf && frameCount > 0u) {
        node->processor->processFrames(inBuf, outBuf, frameCount);
    } else if (outBuf && frameCount > 0u) {
        const ma_uint32 ch = ma_node_get_output_channels(pNode, 0);
        std::fill_n(outBuf, static_cast<size_t>(frameCount) * ch, 0.0f);
    }
}

const ma_node_vtable kEnvEffectNodeVtable = {
    envEffectNodeProcessPcmFrames,
    nullptr,
    1,
    1,
    static_cast<ma_uint32>(MA_NODE_FLAG_CONTINUOUS_PROCESSING),
};

} // namespace

float AudioEngine::computeDistanceAttenuation(float minDistance, float maxDistance, float distance) {
    // Wow.exe build 8606 SESound::ComputeAttenuation (0x756bd0):
    //   const double kRolloff = 4.0 (0x892db0);
    //   const double kFadeStartRatio = 0.9 (0x8a3218);
    if (!std::isfinite(distance) || !std::isfinite(maxDistance) || maxDistance <= 0.0f || distance >= maxDistance) {
        return 0.0f;
    }
    const float safeMin = std::max(0.0f, std::isfinite(minDistance) ? minDistance : 0.0f);
    if (distance <= safeMin || safeMin >= maxDistance) {
        return 1.0f;
    }

    constexpr double kRolloff = 4.0;
    constexpr float kFadeStartRatio = 0.8999999761581421f; // float(0x8a3218)

    float atten = static_cast<float>(
        static_cast<double>(safeMin) /
        (static_cast<double>(safeMin) + kRolloff * static_cast<double>(distance - safeMin)));

    const float fadeStart = kFadeStartRatio * maxDistance;
    if (distance > fadeStart && maxDistance > fadeStart) {
        const float fadeFactor = 1.0f - (distance - fadeStart) / (maxDistance - fadeStart);
        atten *= fadeFactor;
    }
    return std::clamp(atten, 0.0f, 1.0f);
}

uint32_t AudioEngine::maxConcurrentForCategory(uint32_t category) {
    if (category < kSESoundCategoryCaps.size()) {
        return kSESoundCategoryCaps[category];
    }
    return 0x7FFFFFFFu;
}

AudioEngine& AudioEngine::instance() {
    static AudioEngine instance;
    return instance;
}

AudioEngine::AudioEngine() = default;

AudioEngine::~AudioEngine() {
    shutdown();
    closeAudioTrace();
}

bool AudioEngine::initEnvironmentGraph() {
    if (!engine_) return false;
    const uint32_t sr = std::max(8000u, ma_engine_get_sample_rate(engine_));
    const uint32_t ch = std::max(1u, ma_engine_get_channels(engine_));

    envProcessor_ = std::make_unique<EnvironmentReverbProcessor>();
    envProcessor_->configure(sr, ch);

    auto* node = static_cast<EnvEffectMaNode*>(std::calloc(1, sizeof(EnvEffectMaNode)));
    if (!node) {
        envProcessor_.reset();
        return false;
    }
    node->processor = envProcessor_.get();

    const ma_uint32 inCh[1] = {ch};
    const ma_uint32 outCh[1] = {ch};
    ma_node_config nodeCfg = ma_node_config_init();
    nodeCfg.vtable = &kEnvEffectNodeVtable;
    nodeCfg.pInputChannels = inCh;
    nodeCfg.pOutputChannels = outCh;

    ma_node_graph* graph = ma_engine_get_node_graph(engine_);
    if (ma_node_init(graph, &nodeCfg, nullptr, &node->base) != MA_SUCCESS) {
        std::free(node);
        envProcessor_.reset();
        return false;
    }
    if (ma_node_attach_output_bus(&node->base, 0, ma_engine_get_endpoint(engine_), 0) != MA_SUCCESS) {
        ma_node_uninit(&node->base, nullptr);
        std::free(node);
        envProcessor_.reset();
        return false;
    }
    envEffectNode_ = node;

    auto* group = static_cast<ma_sound*>(std::calloc(1, sizeof(ma_sound)));
    if (!group) {
        uninitEnvironmentGraph();
        return false;
    }
    const ma_uint32 groupFlags = static_cast<ma_uint32>(MA_SOUND_FLAG_NO_SPATIALIZATION) |
                                 static_cast<ma_uint32>(MA_SOUND_FLAG_NO_PITCH);
    if (ma_sound_group_init(engine_, groupFlags, nullptr, group) != MA_SUCCESS) {
        std::free(group);
        uninitEnvironmentGraph();
        return false;
    }
    if (ma_node_attach_output_bus(group, 0, &node->base, 0) != MA_SUCCESS) {
        ma_sound_group_uninit(group);
        std::free(group);
        uninitEnvironmentGraph();
        return false;
    }
    envGroup_ = group;
    return true;
}

void AudioEngine::uninitEnvironmentGraph() {
    if (envGroup_) {
        ma_sound_group_uninit(envGroup_);
        std::free(envGroup_);
        envGroup_ = nullptr;
    }
    if (envEffectNode_) {
        auto* node = static_cast<EnvEffectMaNode*>(envEffectNode_);
        ma_node_uninit(&node->base, nullptr);
        std::free(node);
        envEffectNode_ = nullptr;
    }
    envProcessor_.reset();
}

ma_sound* AudioEngine::selectTargetGroup(uint32_t resolvedSoundId, bool spatial,
                                         const SfxPlayOptions& options) const {
    if (!envGroup_) return nullptr;
    if (options.envBusMode == EnvironmentBusMode::ForceDry) return nullptr;
    if (options.envBusMode == EnvironmentBusMode::ForceWet) return envGroup_;

    switch (options.category) {
        case SESoundCategory::kMusic:
        case SESoundCategory::kAmbience:
        case SESoundCategory::kCinematic:
        case SESoundCategory::kScriptMusic:
        case SESoundCategory::kRacialCinematic:
            return nullptr;
        default:
            break;
    }
    if (options.eaxDef == 1u) {
        return nullptr;
    }
    if (resolvedSoundId != 0u) {
        if (const SoundEntryRecord* rec = SoundEntryTable::instance().findLoadedById(resolvedSoundId)) {
            return SoundEntryTable::usesEnvironmentBus(*rec, options.category) ? envGroup_ : nullptr;
        }
    }
    if (options.eaxDef >= 2u || spatial) {
        return envGroup_;
    }
    return nullptr;
}

void AudioEngine::setEnvironmentPreset(const SoundProviderPrefRecord* preset, bool enabled) {
    if (envProcessor_) {
        envProcessor_->setTargetPreset(preset, enabled);
    }
}

void AudioEngine::clearEnvironmentPreset() {
    if (envProcessor_) {
        envProcessor_->clearTargetPreset();
    }
}

uint32_t AudioEngine::activeEnvironmentPresetId() const {
    return envProcessor_ ? envProcessor_->activePresetId() : 0u;
}

bool AudioEngine::isEnvironmentReverbEnabled() const {
    return envProcessor_ ? envProcessor_->isEnabled() : false;
}

void AudioEngine::refreshEngineMasterGain() {
    if (engine_) {
        ma_engine_set_volume(engine_, isMutedInBackground() ? 0.0f : masterVolume_);
    }
}

void AudioEngine::refreshSpatialVolumes() {
    for (auto& active : activeSounds_) {
        if (!active.spatial || !active.sound) continue;
        const float dist = glm::length(active.position - listenerPosition_);
        const float atten = computeDistanceAttenuation(active.minDistance, active.maxDistance, dist);
        ma_sound_set_volume(active.sound, active.baseVolume * atten);
    }
}

void AudioEngine::reapFinishedSounds() {
    if (!initialized_ || !engine_) return;
    for (size_t i = 0; i < activeSounds_.size(); ) {
        if (!ma_sound_is_playing(activeSounds_[i].sound)) {
            const double monoNow = monotonicSecondsNow();
            const double elapsed = std::max(0.0, monoNow - activeSounds_[i].startMonoSec);
            const uint64_t seq = activeSounds_[i].traceSeq;
            const uint32_t hid = activeSounds_[i].id;
            const uint32_t sid = activeSounds_[i].soundEntryId;
            const std::string sname = activeSounds_[i].soundEntryName;
            const std::string spath = activeSounds_[i].path;
            ma_sound_uninit(activeSounds_[i].sound);
            std::free(activeSounds_[i].sound);
            ma_audio_buffer* buffer = static_cast<ma_audio_buffer*>(activeSounds_[i].buffer);
            ma_audio_buffer_uninit(buffer);
            std::free(buffer);
            activeSounds_[i] = std::move(activeSounds_.back());
            activeSounds_.pop_back();
            {
                std::lock_guard<std::mutex> lock(traceMutex_);
                if (traceFile_) {
                    std::ostringstream os;
                    os << std::fixed << std::setprecision(6);
                    os << "{\"event\":\"end\",\"client\":\"wowee\""
                       << ",\"seq\":" << seq
                       << ",\"handle_id\":" << hid
                       << ",\"mono_s\":" << monoNow
                       << ",\"ts_ns\":" << realtimeNsNow()
                       << ",\"sound_id\":" << sid
                       << ",\"sound_name\":\"" << jsonEscape(sname) << "\""
                       << ",\"path\":\"" << jsonEscape(spath) << "\""
                       << ",\"reason\":\"natural_end\""
                       << ",\"elapsed_s\":" << elapsed
                       << "}";
                    writeTraceLineUnlocked(os.str());
                }
            }
        } else {
            ++i;
        }
    }
}

bool AudioEngine::initialize() {
    if (initialized_) {
        LOG_WARNING("AudioEngine already initialized");
        return true;
    }

    engine_ = new ma_engine();

    ma_engine_config engineConfig = ma_engine_config_init();
    engineConfig.dataCallback = &AudioEngine::deviceDataCallback;
    ma_result result = ma_engine_init(&engineConfig, engine_);
    if (result != MA_SUCCESS) {
        LOG_ERROR("Failed to initialize miniaudio engine: ", result);
        delete engine_;
        engine_ = nullptr;
        return false;
    }

    refreshEngineMasterGain();
    if (!initEnvironmentGraph()) {
        LOG_WARNING("AudioEngine: environmental reverb node graph failed to initialize; running dry");
    }

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
    {
        std::lock_guard<std::mutex> lock(traceMutex_);
        if (!traceFile_) {
            if (const char* envPath = std::getenv("WOWEE_AUDIO_TRACE"); envPath && *envPath) {
                traceFile_ = std::fopen(envPath, "a");
                if (traceFile_) traceFilePath_ = envPath;
            }
        }
    }
    emitInitTrace(backendName);
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

    refreshEngineMasterGain();
    if (!initEnvironmentGraph()) {
        LOG_WARNING("AudioEngine: headless environmental reverb node graph failed to initialize");
    }
    initialized_ = true;
    headless_ = true;
    {
        std::lock_guard<std::mutex> lock(traceMutex_);
        if (!traceFile_) {
            if (const char* envPath = std::getenv("WOWEE_AUDIO_TRACE"); envPath && *envPath) {
                traceFile_ = std::fopen(envPath, "a");
                if (traceFile_) traceFilePath_ = envPath;
            }
        }
    }
    emitInitTrace("Headless");
    LOG_INFO("AudioEngine initialized in headless test mode (", engineConfig.sampleRate, " Hz, ",
             engineConfig.channels, " ch)");
    return true;
}

bool AudioEngine::setAudioTracePath(const std::string& jsonlPath) {
    std::lock_guard<std::mutex> lock(traceMutex_);
    if (traceFile_) {
        std::fflush(traceFile_);
        std::fclose(traceFile_);
        traceFile_ = nullptr;
        traceFilePath_.clear();
    }
    if (jsonlPath.empty()) return true;
    traceFile_ = std::fopen(jsonlPath.c_str(), "w");
    if (!traceFile_) return false;
    traceFilePath_ = jsonlPath;
    return true;
}

void AudioEngine::closeAudioTrace() {
    std::lock_guard<std::mutex> lock(traceMutex_);
    if (traceFile_) {
        std::fflush(traceFile_);
        std::fclose(traceFile_);
        traceFile_ = nullptr;
        traceFilePath_.clear();
    }
}

void AudioEngine::writeTraceLineUnlocked(const std::string& jsonLine) {
    if (!traceFile_) return;
    std::fwrite(jsonLine.data(), 1, jsonLine.size(), traceFile_);
    std::fputc('\n', traceFile_);
    std::fflush(traceFile_);
}

void AudioEngine::emitInitTrace(const char* backendName) {
    std::lock_guard<std::mutex> lock(traceMutex_);
    if (!traceFile_) return;
    std::ostringstream os;
    os << std::fixed << std::setprecision(6);
    os << "{\"event\":\"init\",\"client\":\"wowee\""
       << ",\"mono_s\":" << monotonicSecondsNow()
       << ",\"ts_ns\":" << realtimeNsNow()
       << ",\"backend\":\"" << jsonEscape(backendName ? backendName : "") << "\""
       << ",\"device\":\"" << jsonEscape(getOutputDeviceName()) << "\""
       << ",\"sample_rate\":" << getOutputSampleRate()
       << ",\"channels\":" << getOutputChannels()
       << ",\"master_volume\":" << safeJsonFloat(masterVolume_)
       << ",\"suspended\":" << (suspended_ ? "true" : "false")
       << "}";
    writeTraceLineUnlocked(os.str());
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

    stopMusic();

    for (auto& activeSound : activeSounds_) {
        ma_sound_uninit(activeSound.sound);
        std::free(activeSound.sound);
        ma_audio_buffer* buffer = static_cast<ma_audio_buffer*>(activeSound.buffer);
        ma_audio_buffer_uninit(buffer);
        std::free(buffer);
    }
    activeSounds_.clear();

    uninitEnvironmentGraph();
    capturing_.store(false, std::memory_order_release);
    if (engine_) {
        ma_engine_uninit(engine_);
        delete engine_;
        engine_ = nullptr;
    }
    if (captureRing_) {
        auto* ring = static_cast<ma_pcm_rb*>(captureRing_);
        ma_pcm_rb_uninit(ring);
        delete ring;
        captureRing_ = nullptr;
    }

    initialized_ = false;
    headless_ = false;
    suspended_ = false;
    windowFocused_ = true;
    backgroundSoundEnabled_ = false;
    listenerPosition_ = glm::vec3(0.0f);
    listenerForward_ = glm::vec3(0.0f, 0.0f, -1.0f);
    listenerUp_ = glm::vec3(0.0f, 1.0f, 0.0f);
    LOG_INFO("AudioEngine shutdown");
}

void AudioEngine::setMaxChannels(uint32_t maxChannels) {
    maxChannels_ = std::clamp(maxChannels, 16u, 128u);
}

bool AudioEngine::exceedsPerEntryOverlapLimit(uint32_t soundEntryId, bool stoppable,
                                              bool loopEnabled, double monoNow) const {
    if (soundEntryId == 0u || stoppable || loopEnabled) {
        return false;
    }
    constexpr uint32_t kMaxConcurrentSameEntry = 4u;
    constexpr uint32_t kMaxSameEntryInBurstWindow = 2u;
    constexpr double kBurstCoalesceWindowSec = 0.025;

    uint32_t sameIdCount = 0u;
    uint32_t burstCount = 0u;
    for (const auto& active : activeSounds_) {
        if (active.looping || active.soundEntryId != soundEntryId) continue;
        ++sameIdCount;
        if (monoNow - active.startMonoSec <= kBurstCoalesceWindowSec) {
            ++burstCount;
        }
    }
    return sameIdCount >= kMaxConcurrentSameEntry || burstCount >= kMaxSameEntryInBurstWindow;
}

bool AudioEngine::ensureChannelSlotAvailable(bool stoppable, bool loopEnabled) {
    if (activeSounds_.size() < maxChannels_) {
        return true;
    }

    // Steal the oldest fire-and-forget one-shot so active voices stay bounded by
    // Sound_NumChannels without starving stoppable/looping sounds.
    size_t victimIdx = activeSounds_.size();
    double oldestStart = std::numeric_limits<double>::max();
    for (size_t i = 0; i < activeSounds_.size(); ++i) {
        const auto& a = activeSounds_[i];
        if (!a.looping && a.id == 0u && a.startMonoSec < oldestStart) {
            oldestStart = a.startMonoSec;
            victimIdx = i;
        }
    }
    if (victimIdx == activeSounds_.size() && (stoppable || loopEnabled)) {
        for (size_t i = 0; i < activeSounds_.size(); ++i) {
            const auto& a = activeSounds_[i];
            if (!a.looping && a.startMonoSec < oldestStart) {
                oldestStart = a.startMonoSec;
                victimIdx = i;
            }
        }
    }
    if (victimIdx == activeSounds_.size()) {
        return false;
    }

    auto& victim = activeSounds_[victimIdx];
    const double monoNow = monotonicSecondsNow();
    const double elapsed = std::max(0.0, monoNow - victim.startMonoSec);
    const uint64_t seq = victim.traceSeq;
    const uint32_t hid = victim.id;
    const uint32_t sid = victim.soundEntryId;
    const std::string sname = victim.soundEntryName;
    const std::string spath = victim.path;

    ma_sound_stop(victim.sound);
    ma_sound_uninit(victim.sound);
    std::free(victim.sound);
    ma_audio_buffer* buffer = static_cast<ma_audio_buffer*>(victim.buffer);
    ma_audio_buffer_uninit(buffer);
    std::free(buffer);
    activeSounds_[victimIdx] = std::move(activeSounds_.back());
    activeSounds_.pop_back();

    {
        std::lock_guard<std::mutex> lock(traceMutex_);
        if (traceFile_) {
            std::ostringstream os;
            os << std::fixed << std::setprecision(6);
            os << "{\"event\":\"stop\",\"client\":\"wowee\""
               << ",\"seq\":" << seq
               << ",\"handle_id\":" << hid
               << ",\"mono_s\":" << monoNow
               << ",\"ts_ns\":" << realtimeNsNow()
               << ",\"sound_id\":" << sid
               << ",\"sound_name\":\"" << jsonEscape(sname) << "\""
               << ",\"path\":\"" << jsonEscape(spath) << "\""
               << ",\"reason\":\"channel_evicted\""
               << ",\"elapsed_s\":" << elapsed
               << "}";
            writeTraceLineUnlocked(os.str());
        }
    }
    return true;
}

void AudioEngine::setMasterVolume(float volume) {
    masterVolume_ = glm::clamp(volume, 0.0f, 1.0f);
    refreshEngineMasterGain();
}

void AudioEngine::setSuspended(bool suspended) {
    if (suspended_ == suspended) return;
    suspended_ = suspended;
    refreshEngineMasterGain();
}

void AudioEngine::setWindowFocused(bool focused) {
    if (windowFocused_ == focused) return;
    windowFocused_ = focused;
    refreshEngineMasterGain();
}

void AudioEngine::setBackgroundSoundEnabled(bool enabled) {
    if (backgroundSoundEnabled_ == enabled) return;
    backgroundSoundEnabled_ = enabled;
    refreshEngineMasterGain();
}

void AudioEngine::setListenerPosition(const glm::vec3& position) {
    listenerPosition_ = position;
    if (engine_) {
        ma_engine_listener_set_position(engine_, 0, position.x, position.y, position.z);
        refreshSpatialVolumes();
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

bool AudioEngine::isSoundPlaying(uint32_t id) {
    if (id == 0) return false;
    reapFinishedSounds();
    for (const auto& active : activeSounds_) {
        if (active.id == id) return true;
    }
    return false;
}

bool AudioEngine::isSoundLooping(uint32_t id) const {
    if (id == 0) return false;
    for (const auto& active : activeSounds_) {
        if (active.id == id) return active.looping;
    }
    return false;
}

bool AudioEngine::isSoundEntryPlaying(uint32_t soundEntryId) {
    if (soundEntryId == 0) return false;
    reapFinishedSounds();
    for (const auto& active : activeSounds_) {
        if (active.soundEntryId == soundEntryId) return true;
    }
    return false;
}

uint32_t AudioEngine::getActiveCategoryCount(uint32_t category) {
    reapFinishedSounds();
    uint32_t count = 0;
    for (const auto& active : activeSounds_) {
        if (active.category == category) {
            ++count;
        }
    }
    return count;
}

bool AudioEngine::canStartCategory(uint32_t category) {
    if (category >= SESoundCategory::kCount) {
        return false;
    }
    const uint32_t cap = maxConcurrentForCategory(category);
    if (cap >= 0x7FFFFFFFu) {
        return true;
    }
    return getActiveCategoryCount(category) < cap;
}

void AudioEngine::setSoundPosition(uint32_t id, const glm::vec3& position) {
    if (id == 0) return;
    for (auto& active : activeSounds_) {
        if (active.id == id && active.sound) {
            active.position = position;
            ma_sound_set_position(active.sound, position.x, position.y, position.z);
            if (active.spatial) {
                const float dist = glm::length(position - listenerPosition_);
                const float atten = computeDistanceAttenuation(active.minDistance, active.maxDistance, dist);
                ma_sound_set_volume(active.sound, active.baseVolume * atten);
            }
            return;
        }
    }
}

uint32_t AudioEngine::startSound2DInternal(const std::vector<uint8_t>& wavData, float volume,
                                           float pitch, bool stoppable,
                                           const SfxPlayOptions& options,
                                           const std::string& debugPath) {
    LOG_INFO("sfx2d: bytes=", wavData.size(), " vol=", volume);
    const float clampedPitch = glm::clamp(pitch, 0.25f, 4.0f);
    const bool loopEnabled = stoppable && options.loop;
    uint32_t resolvedId = options.soundEntryId;
    std::string resolvedName;
    resolveSoundMetadata(resolvedId, resolvedName, debugPath);

    auto notify = [&](bool started, uint32_t handleId, uint64_t traceSeq,
                      double monoNow, const DecodedWavCacheEntry& dec) {
        if (playbackObserver_) {
            PlaybackEvent ev;
            ev.soundEntryId = resolvedId;
            ev.soundEntryName = resolvedName;
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
        }
        std::lock_guard<std::mutex> lock(traceMutex_);
        if (!traceFile_) return;
        const double dur = (dec.sampleRate > 0)
            ? (static_cast<double>(dec.frames) / static_cast<double>(dec.sampleRate)) / static_cast<double>(clampedPitch)
            : 0.0;
        std::ostringstream os;
        os << std::fixed << std::setprecision(6);
        os << "{\"event\":\"start\",\"client\":\"wowee\""
           << ",\"seq\":" << traceSeq
           << ",\"handle_id\":" << handleId
           << ",\"mono_s\":" << monoNow
           << ",\"ts_ns\":" << realtimeNsNow()
           << ",\"sound_id\":" << resolvedId
           << ",\"sound_name\":\"" << jsonEscape(resolvedName) << "\""
           << ",\"path\":\"" << jsonEscape(debugPath) << "\""
           << ",\"spatial\":false,\"loop\":" << (loopEnabled ? "true" : "false") << ",\"music\":false"
           << ",\"volume\":" << safeJsonFloat(volume)
           << ",\"master_volume\":" << safeJsonFloat(masterVolume_)
           << ",\"suspended\":" << (suspended_ ? "true" : "false")
           << ",\"pitch\":" << safeJsonFloat(clampedPitch)
           << ",\"min_dist\":0.0,\"max_dist\":0.0"
           << ",\"source_pos\":[0.0,0.0,0.0]"
           << ",\"listener_pos\":[" << safeJsonFloat(listenerPosition_.x) << ","
           << safeJsonFloat(listenerPosition_.y) << "," << safeJsonFloat(listenerPosition_.z) << "]"
           << ",\"listener_fwd\":[" << safeJsonFloat(listenerForward_.x) << ","
           << safeJsonFloat(listenerForward_.y) << "," << safeJsonFloat(listenerForward_.z) << "]"
           << ",\"sample_rate\":" << dec.sampleRate
           << ",\"decoded_frames\":" << dec.frames
           << ",\"duration_s\":" << dur
           << ",\"started\":" << (started ? "true" : "false")
           << "}";
        writeTraceLineUnlocked(os.str());
    };

    DecodedWavCacheEntry emptyDec;
    if (!initialized_ || !engine_ || wavData.empty() || masterVolume_ <= 0.0f || volume <= 0.0f) {
        notify(false, 0, nextTraceSeq_++, monotonicSecondsNow(), emptyDec);
        return 0;
    }

    reapFinishedSounds();

    // Wow.exe 0x4621ef -> 0x4664b0: suppress fire-and-forget one-shots while muted in background
    if (isMutedInBackground() && !stoppable && !options.loop) {
        notify(false, 0, nextTraceSeq_++, monotonicSecondsNow(), emptyDec);
        return 0;
    }

    // Wow.exe 0x4620c0..0x4620da: per-category concurrency cap (0x897018)
    if (!canStartCategory(options.category)) {
        notify(false, 0, nextTraceSeq_++, monotonicSecondsNow(), emptyDec);
        return 0;
    }

    // Flags & 0x020 (NO_DUPLICATES)
    if ((options.soundEntryFlags & SoundEntryFlags::kNoDuplicates) != 0u &&
        options.soundEntryId != 0 && isSoundEntryPlaying(options.soundEntryId)) {
        notify(false, 0, nextTraceSeq_++, monotonicSecondsNow(), emptyDec);
        return 0;
    }

    const double preStartMono = monotonicSecondsNow();
    if (exceedsPerEntryOverlapLimit(resolvedId, stoppable, loopEnabled, preStartMono) ||
        !ensureChannelSlotAvailable(stoppable, loopEnabled)) {
        notify(false, 0, nextTraceSeq_++, preStartMono, emptyDec);
        return 0;
    }

    DecodedWavCacheEntry decoded;
    if (!decodeWavCached(wavData, decoded) || !decoded.pcmData || decoded.frames == 0) {
        notify(false, 0, nextTraceSeq_++, monotonicSecondsNow(), emptyDec);
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
        notify(false, 0, nextTraceSeq_++, monotonicSecondsNow(), decoded);
        return 0;
    }
    audioBuffer.markInitialised();

    const bool customPitch = std::abs(clampedPitch - 1.0f) > 1e-3f;
    const ma_uint32 flags = static_cast<ma_uint32>(MA_SOUND_FLAG_DECODE) |
                            static_cast<ma_uint32>(MA_SOUND_FLAG_ASYNC) |
                            static_cast<ma_uint32>(MA_SOUND_FLAG_NO_SPATIALIZATION) |
                            (customPitch ? 0u : static_cast<ma_uint32>(MA_SOUND_FLAG_NO_PITCH));

    ma_sound* targetGroup = selectTargetGroup(resolvedId, /*spatial=*/false, options);
    SoundStorage sound;
    if (!sound ||
        ma_sound_init_from_data_source(engine_, audioBuffer.get(), flags, targetGroup, sound.get()) != MA_SUCCESS) {
        notify(false, 0, nextTraceSeq_++, monotonicSecondsNow(), decoded);
        return 0;
    }
    sound.markInitialised();

    // FMOD's 2D mono-to-stereo equal-power center pan law scales mono channels
    // by 1/sqrt(2) (-3.01 dB per L/R speaker) so mono 2D SFX match stereo
    // acoustic energy; miniaudio otherwise copies mono 1.0 to both L and R.
    constexpr float kMono2DCenterPanGain = 0.70710678f;
    const float renderVolume = (decoded.channels == 1) ? (volume * kMono2DCenterPanGain) : volume;
    ma_sound_set_volume(sound.get(), renderVolume);
    if (customPitch) {
        ma_sound_set_pitch(sound.get(), clampedPitch);
    }
    if (loopEnabled) {
        ma_sound_set_looping(sound.get(), MA_TRUE);
    }
    if (ma_sound_start(sound.get()) != MA_SUCCESS) {
        notify(false, 0, nextTraceSeq_++, monotonicSecondsNow(), decoded);
        return 0;
    }

    uint32_t id = 0;
    if (stoppable) {
        id = nextSoundId_++;
        if (nextSoundId_ == 0) nextSoundId_ = 1;
    }
    const uint64_t traceSeq = nextTraceSeq_++;
    const double monoNow = monotonicSecondsNow();
    ActiveSound active;
    active.sound = sound.release();
    active.buffer = audioBuffer.release();
    active.pcmDataRef = decoded.pcmData;
    active.id = id;
    active.soundEntryId = resolvedId;
    active.category = options.category;
    active.looping = loopEnabled;
    active.spatial = false;
    active.baseVolume = volume;
    active.traceSeq = traceSeq;
    active.soundEntryName = resolvedName;
    active.path = debugPath;
    active.startMonoSec = monoNow;
    activeSounds_.push_back(std::move(active));
    notify(true, id, traceSeq, monoNow, decoded);
    return stoppable ? id : 1u;
}

bool AudioEngine::playSound2D(const std::vector<uint8_t>& wavData, float volume, float pitch,
                              const std::string& debugPath, uint32_t soundEntryId) {
    SfxPlayOptions opts;
    opts.soundEntryId = soundEntryId;
    return startSound2DInternal(wavData, volume, pitch, false, opts, debugPath) != 0;
}

bool AudioEngine::playSound2D(const std::vector<uint8_t>& wavData, float volume, float pitch,
                              const SfxPlayOptions& options, const std::string& debugPath) {
    return startSound2DInternal(wavData, volume, pitch, false, options, debugPath) != 0;
}

uint32_t AudioEngine::playSound2DStoppable(const std::vector<uint8_t>& wavData, float volume, float pitch,
                                           const std::string& debugPath, uint32_t soundEntryId) {
    SfxPlayOptions opts;
    opts.soundEntryId = soundEntryId;
    return startSound2DInternal(wavData, volume, pitch, true, opts, debugPath);
}

uint32_t AudioEngine::playSound2DStoppable(const std::vector<uint8_t>& wavData, float volume, float pitch,
                                           const SfxPlayOptions& options, const std::string& debugPath) {
    return startSound2DInternal(wavData, volume, pitch, true, options, debugPath);
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
    return startSound2DInternal(data, volume, pitch, true, SfxPlayOptions{}, mpqPath);
}

void AudioEngine::stopSound(uint32_t id) {
    if (id == 0) return;
    for (auto it = activeSounds_.begin(); it != activeSounds_.end(); ++it) {
        if (it->id == id) {
            const double monoNow = monotonicSecondsNow();
            const double elapsed = std::max(0.0, monoNow - it->startMonoSec);
            const uint64_t seq = it->traceSeq;
            const uint32_t sid = it->soundEntryId;
            const std::string sname = it->soundEntryName;
            const std::string spath = it->path;
            ma_sound_stop(it->sound);
            ma_sound_uninit(it->sound);
            std::free(it->sound);
            ma_audio_buffer* buffer = static_cast<ma_audio_buffer*>(it->buffer);
            ma_audio_buffer_uninit(buffer);
            std::free(buffer);
            activeSounds_.erase(it);
            {
                std::lock_guard<std::mutex> lock(traceMutex_);
                if (traceFile_) {
                    std::ostringstream os;
                    os << std::fixed << std::setprecision(6);
                    os << "{\"event\":\"stop\",\"client\":\"wowee\""
                       << ",\"seq\":" << seq
                       << ",\"handle_id\":" << id
                       << ",\"mono_s\":" << monoNow
                       << ",\"ts_ns\":" << realtimeNsNow()
                       << ",\"sound_id\":" << sid
                       << ",\"sound_name\":\"" << jsonEscape(sname) << "\""
                       << ",\"path\":\"" << jsonEscape(spath) << "\""
                       << ",\"reason\":\"explicit_stop\""
                       << ",\"elapsed_s\":" << elapsed
                       << "}";
                    writeTraceLineUnlocked(os.str());
                }
            }
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
    return startSound2DInternal(data, volume, pitch, false, SfxPlayOptions{}, mpqPath) != 0;
}

uint32_t AudioEngine::startSound3DInternal(const std::vector<uint8_t>& wavData, const glm::vec3& position,
                                           float volume, float pitch, float maxDistance,
                                           float minDistance, bool stoppable,
                                           const SfxPlayOptions& options,
                                           const std::string& debugPath) {
    const float clampedPitch = glm::clamp(pitch, 0.25f, 4.0f);
    const float safeMinDist = std::max(0.1f, minDistance);
    const float safeMaxDist = std::max(safeMinDist, maxDistance);
    const bool loopEnabled = stoppable && options.loop;
    uint32_t resolvedId = options.soundEntryId;
    std::string resolvedName;
    resolveSoundMetadata(resolvedId, resolvedName, debugPath);

    auto notify = [&](bool started, uint32_t handleId, uint64_t traceSeq,
                      double monoNow, const DecodedWavCacheEntry& dec) {
        if (playbackObserver_) {
            PlaybackEvent ev;
            ev.soundEntryId = resolvedId;
            ev.soundEntryName = resolvedName;
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
        }
        std::lock_guard<std::mutex> lock(traceMutex_);
        if (!traceFile_) return;
        const double dur = (dec.sampleRate > 0)
            ? (static_cast<double>(dec.frames) / static_cast<double>(dec.sampleRate)) / static_cast<double>(clampedPitch)
            : 0.0;
        std::ostringstream os;
        os << std::fixed << std::setprecision(6);
        os << "{\"event\":\"start\",\"client\":\"wowee\""
           << ",\"seq\":" << traceSeq
           << ",\"handle_id\":" << handleId
           << ",\"mono_s\":" << monoNow
           << ",\"ts_ns\":" << realtimeNsNow()
           << ",\"sound_id\":" << resolvedId
           << ",\"sound_name\":\"" << jsonEscape(resolvedName) << "\""
           << ",\"path\":\"" << jsonEscape(debugPath) << "\""
           << ",\"spatial\":true,\"loop\":" << (loopEnabled ? "true" : "false") << ",\"music\":false"
           << ",\"volume\":" << safeJsonFloat(volume)
           << ",\"master_volume\":" << safeJsonFloat(masterVolume_)
           << ",\"suspended\":" << (suspended_ ? "true" : "false")
           << ",\"pitch\":" << safeJsonFloat(clampedPitch)
           << ",\"min_dist\":" << safeJsonFloat(safeMinDist)
           << ",\"max_dist\":" << safeJsonFloat(safeMaxDist)
           << ",\"source_pos\":[" << safeJsonFloat(position.x) << ","
           << safeJsonFloat(position.y) << "," << safeJsonFloat(position.z) << "]"
           << ",\"listener_pos\":[" << safeJsonFloat(listenerPosition_.x) << ","
           << safeJsonFloat(listenerPosition_.y) << "," << safeJsonFloat(listenerPosition_.z) << "]"
           << ",\"listener_fwd\":[" << safeJsonFloat(listenerForward_.x) << ","
           << safeJsonFloat(listenerForward_.y) << "," << safeJsonFloat(listenerForward_.z) << "]"
           << ",\"sample_rate\":" << dec.sampleRate
           << ",\"decoded_frames\":" << dec.frames
           << ",\"duration_s\":" << dur
           << ",\"started\":" << (started ? "true" : "false")
           << "}";
        writeTraceLineUnlocked(os.str());
    };

    DecodedWavCacheEntry emptyDec;
    if (!initialized_ || !engine_ || wavData.empty() || masterVolume_ <= 0.0f || volume <= 0.0f) {
        notify(false, 0, nextTraceSeq_++, monotonicSecondsNow(), emptyDec);
        return 0;
    }

    reapFinishedSounds();

    // Wow.exe 0x4621ef -> 0x4664b0: suppress fire-and-forget one-shots while muted in background
    if (isMutedInBackground() && !stoppable && !options.loop) {
        notify(false, 0, nextTraceSeq_++, monotonicSecondsNow(), emptyDec);
        return 0;
    }

    // Wow.exe 0x4620c0..0x4620da: per-category concurrency cap (0x897018)
    if (!canStartCategory(options.category)) {
        notify(false, 0, nextTraceSeq_++, monotonicSecondsNow(), emptyDec);
        return 0;
    }

    // Flags & 0x020 (NO_DUPLICATES)
    if ((options.soundEntryFlags & SoundEntryFlags::kNoDuplicates) != 0u &&
        options.soundEntryId != 0 && isSoundEntryPlaying(options.soundEntryId)) {
        notify(false, 0, nextTraceSeq_++, monotonicSecondsNow(), emptyDec);
        return 0;
    }

    const double preStartMono = monotonicSecondsNow();
    if (exceedsPerEntryOverlapLimit(resolvedId, stoppable, loopEnabled, preStartMono) ||
        !ensureChannelSlotAvailable(stoppable, loopEnabled)) {
        notify(false, 0, nextTraceSeq_++, preStartMono, emptyDec);
        return 0;
    }

    // Wow.exe 0x461c10 -> 0x461af0: pre-spawn DistanceCutoff^2 cull before decoding/allocating channel
    const glm::vec3 rel = position - listenerPosition_;
    const float distSq = glm::dot(rel, rel);
    if (distSq >= safeMaxDist * safeMaxDist) {
        notify(false, 0, nextTraceSeq_++, monotonicSecondsNow(), emptyDec);
        return 0;
    }
    const float dist = std::sqrt(distSq);
    const float atten = computeDistanceAttenuation(safeMinDist, safeMaxDist, dist);
    if (atten <= 0.0f) {
        notify(false, 0, nextTraceSeq_++, monotonicSecondsNow(), emptyDec);
        return 0;
    }

    DecodedWavCacheEntry decoded;
    if (!decodeWavCached(wavData, decoded) || !decoded.pcmData || decoded.frames == 0) {
        notify(false, 0, nextTraceSeq_++, monotonicSecondsNow(), emptyDec);
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
        notify(false, 0, nextTraceSeq_++, monotonicSecondsNow(), decoded);
        return 0;
    }
    audioBuffer.markInitialised();

    ma_sound* targetGroup = selectTargetGroup(resolvedId, /*spatial=*/true, options);
    SoundStorage sound;
    if (!sound ||
        ma_sound_init_from_data_source(engine_, audioBuffer.get(),
                                       MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_ASYNC,
                                       targetGroup, sound.get()) != MA_SUCCESS) {
        notify(false, 0, nextTraceSeq_++, monotonicSecondsNow(), decoded);
        return 0;
    }
    sound.markInitialised();

    // Keep ma_attenuation_model_inverse with rolloff = 0.0f so miniaudio computes full 3D stereo
    // directional panning while AudioEngine applies exact Wow.exe 0x756bd0 distance attenuation.
    // Match Native Wow.exe 8606 (FMOD 4.09.07) +-9.54 dB stereo pan width at +-90 deg (far-ear
    // gain ratio 1/3) instead of miniaudio's default 1.0f factor (+-13.98 dB, -6 dB front dip).
    constexpr float kFmodStereoPanDirectionalFactor = 2.0f / 3.0f;
    ma_sound_set_position(sound.get(), position.x, position.y, position.z);
    ma_sound_set_volume(sound.get(), volume * atten);
    ma_sound_set_pitch(sound.get(), clampedPitch);
    ma_sound_set_attenuation_model(sound.get(), ma_attenuation_model_inverse);
    ma_sound_set_min_gain(sound.get(), 0.0f);
    ma_sound_set_max_gain(sound.get(), 1.0f);
    ma_sound_set_min_distance(sound.get(), safeMinDist);
    ma_sound_set_max_distance(sound.get(), safeMaxDist);
    ma_sound_set_rolloff(sound.get(), 0.0f);
    ma_sound_set_directional_attenuation_factor(sound.get(), kFmodStereoPanDirectionalFactor);

    if (loopEnabled) {
        ma_sound_set_looping(sound.get(), MA_TRUE);
    }

    if (ma_sound_start(sound.get()) != MA_SUCCESS) {
        notify(false, 0, nextTraceSeq_++, monotonicSecondsNow(), decoded);
        return 0;
    }

    uint32_t id = 0;
    if (stoppable) {
        id = nextSoundId_++;
        if (nextSoundId_ == 0) nextSoundId_ = 1;
    }
    const uint64_t traceSeq = nextTraceSeq_++;
    const double monoNow = monotonicSecondsNow();
    ActiveSound active;
    active.sound = sound.release();
    active.buffer = audioBuffer.release();
    active.pcmDataRef = decoded.pcmData;
    active.id = id;
    active.soundEntryId = resolvedId;
    active.category = options.category;
    active.looping = loopEnabled;
    active.spatial = true;
    active.baseVolume = volume;
    active.position = position;
    active.minDistance = safeMinDist;
    active.maxDistance = safeMaxDist;
    active.traceSeq = traceSeq;
    active.soundEntryName = resolvedName;
    active.path = debugPath;
    active.startMonoSec = monoNow;
    activeSounds_.push_back(std::move(active));
    notify(true, id, traceSeq, monoNow, decoded);
    return stoppable ? id : 1u;
}

bool AudioEngine::playSound3D(const std::vector<uint8_t>& wavData, const glm::vec3& position,
                              float volume, float pitch, float maxDistance, float minDistance,
                              const std::string& debugPath, uint32_t soundEntryId) {
    SfxPlayOptions opts;
    opts.soundEntryId = soundEntryId;
    return startSound3DInternal(wavData, position, volume, pitch, maxDistance, minDistance,
                                false, opts, debugPath) != 0;
}

bool AudioEngine::playSound3D(const std::vector<uint8_t>& wavData, const glm::vec3& position,
                              float volume, float pitch, float maxDistance, float minDistance,
                              const SfxPlayOptions& options, const std::string& debugPath) {
    return startSound3DInternal(wavData, position, volume, pitch, maxDistance, minDistance,
                                false, options, debugPath) != 0;
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
    return startSound3DInternal(data, position, volume, pitch, maxDistance, minDistance,
                                false, SfxPlayOptions{}, mpqPath) != 0;
}

uint32_t AudioEngine::playSound3DStoppable(const std::vector<uint8_t>& wavData, const glm::vec3& position,
                                           float volume, float pitch, float maxDistance, float minDistance,
                                           const std::string& debugPath, uint32_t soundEntryId) {
    SfxPlayOptions opts;
    opts.soundEntryId = soundEntryId;
    return startSound3DInternal(wavData, position, volume, pitch, maxDistance, minDistance,
                                true, opts, debugPath);
}

uint32_t AudioEngine::playSound3DStoppable(const std::vector<uint8_t>& wavData, const glm::vec3& position,
                                           float volume, float pitch, float maxDistance, float minDistance,
                                           const SfxPlayOptions& options, const std::string& debugPath) {
    return startSound3DInternal(wavData, position, volume, pitch, maxDistance, minDistance,
                                true, options, debugPath);
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
    return startSound3DInternal(data, position, volume, pitch, maxDistance, minDistance,
                                true, SfxPlayOptions{}, mpqPath);
}

void AudioEngine::stopMusicSlot(MusicStreamSlot& slot, const char* reason) {
    const bool wasPlaying = (slot.sound != nullptr);
    const uint64_t seq = slot.traceSeq;
    const uint32_t seId = slot.soundEntryId;
    const std::string seName = slot.soundEntryName;
    const std::string spath = slot.tracePath;
    const double startMono = slot.startMonoSec;

    if (slot.sound) {
        ma_sound_uninit(slot.sound);
        std::free(slot.sound);
        slot.sound = nullptr;
    }
    if (slot.decoder) {
        ma_decoder* decoder = static_cast<ma_decoder*>(slot.decoder);
        ma_decoder_uninit(decoder);
        delete decoder;
        slot.decoder = nullptr;
    }
    slot.data.reset();
    slot.volume = 0.0f;
    slot.soundEntryId = 0;
    slot.soundEntryName.clear();
    slot.traceSeq = 0;
    slot.tracePath.clear();
    slot.startMonoSec = 0.0;

    if (wasPlaying) {
        const double monoNow = monotonicSecondsNow();
        std::lock_guard<std::mutex> lock(traceMutex_);
        if (traceFile_) {
            std::ostringstream os;
            os << std::fixed << std::setprecision(6);
            os << "{\"event\":\"stop\",\"client\":\"wowee\""
               << ",\"seq\":" << seq
               << ",\"handle_id\":0"
               << ",\"mono_s\":" << monoNow
               << ",\"ts_ns\":" << realtimeNsNow()
               << ",\"sound_id\":" << seId
               << ",\"sound_name\":\"" << jsonEscape(seName) << "\""
               << ",\"path\":\"" << jsonEscape(spath) << "\""
               << ",\"reason\":\"" << jsonEscape(reason ? reason : "explicit_stop") << "\""
               << ",\"elapsed_s\":" << std::max(0.0, monoNow - startMono)
               << "}";
            writeTraceLineUnlocked(os.str());
        }
    }
}

bool AudioEngine::playMusic(std::shared_ptr<const std::vector<uint8_t>> musicData,
                            float volume, bool loop, const std::string& debugPath,
                            bool keepOutgoingForCrossfade, uint32_t soundEntryId) {
    if (!initialized_ || !engine_ || !musicData || musicData->empty()) {
        return false;
    }

    const float clampedVol = glm::clamp(volume, 0.0f, 1.0f);
    LOG_INFO("AudioEngine::playMusic - data size: ", musicData->size(), " bytes, volume: ", clampedVol,
             ", crossfade: ", keepOutgoingForCrossfade);

    ma_decoder* decoder = new ma_decoder();
    ma_decoder_config decoderConfig = ma_decoder_config_init_default();
    ma_result result = ma_decoder_init_memory(
        musicData->data(),
        musicData->size(),
        &decoderConfig,
        decoder
    );

    if (result != MA_SUCCESS) {
        LOG_ERROR("Failed to create music decoder: ", result);
        delete decoder;
        return false;
    }

    ma_sound* sound = static_cast<ma_sound*>(std::malloc(sizeof(ma_sound)));
    if (!sound) {
        ma_decoder_uninit(decoder);
        delete decoder;
        return false;
    }

    result = ma_sound_init_from_data_source(
        engine_,
        decoder,
        MA_SOUND_FLAG_STREAM | MA_SOUND_FLAG_NO_PITCH | MA_SOUND_FLAG_NO_SPATIALIZATION,
        nullptr,
        sound
    );

    if (result != MA_SUCCESS) {
        LOG_ERROR("Failed to create music sound: ", result);
        ma_decoder_uninit(decoder);
        delete decoder;
        std::free(sound);
        return false;
    }

    ma_sound_set_volume(sound, clampedVol);
    ma_sound_set_looping(sound, loop ? MA_TRUE : MA_FALSE);

    result = ma_sound_start(sound);
    if (result != MA_SUCCESS) {
        LOG_ERROR("Failed to start music playback: ", result);
        ma_sound_uninit(sound);
        std::free(sound);
        ma_decoder_uninit(decoder);
        delete decoder;
        return false;
    }

    // Transition outgoing stream cleanly: either move activeMusic_ into fadingOutMusic_
    // for simultaneous dual-decoder crossfade, or stop active/fading streams immediately.
    if (keepOutgoingForCrossfade && activeMusic_.sound != nullptr &&
        ma_sound_is_playing(activeMusic_.sound) == MA_TRUE && activeMusic_.volume > 0.0f) {
        stopMusicSlot(fadingOutMusic_, "crossfade_superseded");
        fadingOutMusic_ = std::move(activeMusic_);
        activeMusic_ = MusicStreamSlot{};
    } else {
        stopMusicSlot(fadingOutMusic_, "explicit_stop");
        stopMusicSlot(activeMusic_, "explicit_stop");
    }

    uint32_t resolvedId = soundEntryId;
    std::string resolvedName;
    const auto& table = SoundEntryTable::instance();
    if (resolvedId != 0) {
        if (const SoundEntryRecord* rec = table.findLoadedById(resolvedId)) {
            resolvedName = rec->name;
        }
    } else if (!debugPath.empty()) {
        if (const SoundEntryRecord* rec = table.findLoadedByPath(debugPath)) {
            resolvedId = rec->id;
            resolvedName = rec->name;
        }
    }

    activeMusic_.sound = sound;
    activeMusic_.decoder = decoder;
    activeMusic_.data = std::move(musicData);
    activeMusic_.volume = clampedVol;
    activeMusic_.soundEntryId = resolvedId;
    activeMusic_.soundEntryName = resolvedName;
    activeMusic_.traceSeq = nextTraceSeq_++;
    activeMusic_.tracePath = debugPath;
    activeMusic_.startMonoSec = monotonicSecondsNow();

    LOG_INFO("Music playback started successfully - volume: ", clampedVol,
             ", loop: ", loop,
             ", is_playing: ", ma_sound_is_playing(activeMusic_.sound));

    if (playbackObserver_) {
        PlaybackEvent ev;
        ev.soundEntryId = resolvedId;
        ev.soundEntryName = resolvedName;
        ev.path = debugPath;
        ev.byteSize = activeMusic_.data ? activeMusic_.data->size() : 0;
        ev.sampleRate = decoder->outputSampleRate;
        ev.volume = clampedVol;
        ev.music = true;
        ev.started = true;
        playbackObserver_(ev);
    }
    {
        std::lock_guard<std::mutex> lock(traceMutex_);
        if (traceFile_) {
            std::ostringstream os;
            os << std::fixed << std::setprecision(6);
            os << "{\"event\":\"start\",\"client\":\"wowee\""
               << ",\"seq\":" << activeMusic_.traceSeq
               << ",\"handle_id\":0"
               << ",\"mono_s\":" << activeMusic_.startMonoSec
               << ",\"ts_ns\":" << realtimeNsNow()
               << ",\"sound_id\":" << resolvedId
               << ",\"sound_name\":\"" << jsonEscape(resolvedName) << "\""
               << ",\"path\":\"" << jsonEscape(debugPath) << "\""
               << ",\"spatial\":false,\"loop\":" << (loop ? "true" : "false") << ",\"music\":true"
               << ",\"volume\":" << safeJsonFloat(clampedVol)
               << ",\"master_volume\":" << safeJsonFloat(masterVolume_)
               << ",\"suspended\":" << (suspended_ ? "true" : "false")
               << ",\"pitch\":1.0,\"min_dist\":0.0,\"max_dist\":0.0"
               << ",\"source_pos\":[0.0,0.0,0.0]"
               << ",\"listener_pos\":[" << safeJsonFloat(listenerPosition_.x) << ","
               << safeJsonFloat(listenerPosition_.y) << "," << safeJsonFloat(listenerPosition_.z) << "]"
               << ",\"listener_fwd\":[" << safeJsonFloat(listenerForward_.x) << ","
               << safeJsonFloat(listenerForward_.y) << "," << safeJsonFloat(listenerForward_.z) << "]"
               << ",\"sample_rate\":" << decoder->outputSampleRate
               << ",\"decoded_frames\":0,\"duration_s\":0.0,\"started\":true}";
            writeTraceLineUnlocked(os.str());
        }
    }

    return true;
}

void AudioEngine::stopMusic() {
    stopMusicSlot(fadingOutMusic_, "explicit_stop");
    stopMusicSlot(activeMusic_, "explicit_stop");
}

void AudioEngine::stopCrossfadeOut() {
    stopMusicSlot(fadingOutMusic_, "crossfade_end");
}

bool AudioEngine::isMusicPlaying() const {
    if (!activeMusic_.sound) {
        return false;
    }
    return ma_sound_is_playing(activeMusic_.sound) == MA_TRUE;
}

bool AudioEngine::isCrossfadeOutPlaying() const {
    if (!fadingOutMusic_.sound) {
        return false;
    }
    return ma_sound_is_playing(fadingOutMusic_.sound) == MA_TRUE;
}

void AudioEngine::setMusicVolume(float volume) {
    activeMusic_.volume = glm::clamp(volume, 0.0f, 1.0f);
    if (activeMusic_.sound) {
        ma_sound_set_volume(activeMusic_.sound, activeMusic_.volume);
    }
}

void AudioEngine::setCrossfadeOutVolume(float volume) {
    fadingOutMusic_.volume = glm::clamp(volume, 0.0f, 1.0f);
    if (fadingOutMusic_.sound) {
        ma_sound_set_volume(fadingOutMusic_.sound, fadingOutMusic_.volume);
    }
}

void AudioEngine::update(float deltaTime) {
    (void)deltaTime;

    if (!initialized_ || !engine_) {
        return;
    }

    reapFinishedSounds();
    refreshSpatialVolumes();
}

void AudioEngine::deviceDataCallback(ma_device* device, void* out, const void* in, uint32_t frames) {
    (void)in;
    auto* engine = static_cast<ma_engine*>(device->pUserData);
    ma_engine_read_pcm_frames(engine, out, frames, nullptr);

    AudioEngine& self = instance();
    if (!self.capturing_.load(std::memory_order_acquire)) return;
    auto* ring = static_cast<ma_pcm_rb*>(self.captureRing_);
    const ma_uint32 channels = ma_engine_get_channels(engine);
    const auto* src = static_cast<const float*>(out);
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
        auto* ring = new ma_pcm_rb();
        if (ma_pcm_rb_init(ma_format_f32, ma_engine_get_channels(engine_),
                           ma_engine_get_sample_rate(engine_), nullptr, nullptr, ring) != MA_SUCCESS) {
            delete ring;
            LOG_WARNING("AudioEngine: could not make the capture buffer - recording without sound");
            return false;
        }
        captureRing_ = ring;
    }
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
    constexpr uint32_t kPeriodFrames = 256u;
    uint32_t offset = 0u;
    while (offset < frameCount) {
        const uint32_t chunk = std::min(kPeriodFrames, frameCount - offset);
        ma_uint64 framesRead = 0;
        const ma_result res = ma_engine_read_pcm_frames(
            engine_, dstInterleaved + static_cast<size_t>(offset) * ch, chunk, &framesRead);
        if (res != MA_SUCCESS && res != MA_AT_END) {
            return offset;
        }
        offset += chunk;
    }
    update(static_cast<float>(frameCount) / static_cast<float>(std::max(1u, ma_engine_get_sample_rate(engine_))));
    return frameCount;
}

} // namespace audio
} // namespace wowee
