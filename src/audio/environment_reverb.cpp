#include "audio/environment_reverb.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "core/logger.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace wowee {
namespace audio {

namespace {

constexpr std::array<uint32_t, 8> kBaseCombDelays48k = {
    1277u, 1493u, 1747u, 2029u, // Left tank lines (mutually prime)
    1319u, 1543u, 1789u, 2081u  // Right tank lines (decorrelated stereo primes)
};

constexpr std::array<std::array<uint32_t, 2>, 2> kBaseAllpassDelays48k = {{
    {241u, 557u}, // Left cascaded Schroeder diffusers
    {263u, 587u}  // Right decorrelated Schroeder diffusers
}};

float millibelsToLinear(float mb) {
    if (mb <= -9999.0f) return 0.0f;
    return std::pow(10.0f, mb / 2000.0f);
}

} // namespace

// ---------------------------------------------------------------------------
// EnvironmentReverbTable
// ---------------------------------------------------------------------------

void EnvironmentReverbTable::reset() {
    loaded_ = false;
    providerPrefs_.clear();
    samplePrefs_.clear();
    wmoPrefs_.clear();
    areaPrefs_.clear();
    areaParents_.clear();
}

bool EnvironmentReverbTable::initialize(pipeline::AssetManager* assets) {
    reset();
    if (!assets || !assets->isInitialized()) return false;

    auto spDbc = assets->loadDBC("SoundProviderPreferences.dbc");
    if (!spDbc || !spDbc->isLoaded() || spDbc->getFieldCount() < 24) {
        LOG_WARNING("EnvironmentReverbTable: SoundProviderPreferences.dbc missing or invalid");
        return false;
    }

    providerPrefs_.reserve(spDbc->getRecordCount());
    for (uint32_t r = 0; r < spDbc->getRecordCount(); ++r) {
        const uint32_t id = spDbc->getUInt32(r, 0);
        if (id == 0) continue;
        SoundProviderPrefRecord rec;
        rec.id = id;
        rec.description = spDbc->getString(r, 1);
        rec.flags = spDbc->getUInt32(r, 2);
        rec.eaxEnvironmentSelection = spDbc->getInt32(r, 3);
        rec.decayTime = spDbc->getFloat(r, 4);
        rec.environmentSize = spDbc->getFloat(r, 5);
        rec.environmentDiffusion = spDbc->getFloat(r, 6);
        rec.eaxRoom = spDbc->getInt32(r, 7);
        rec.eaxRoomHf = spDbc->getInt32(r, 8);
        rec.eaxDecayHfRatio = spDbc->getFloat(r, 9);
        rec.eaxReflections = spDbc->getInt32(r, 10);
        rec.eaxReflectionsDelay = spDbc->getFloat(r, 11);
        rec.eaxReverb = spDbc->getInt32(r, 12);
        rec.eaxReverbDelay = spDbc->getFloat(r, 13);
        rec.eaxRoomRolloffFactor = spDbc->getFloat(r, 14);
        rec.eaxAirAbsorptionHf = spDbc->getFloat(r, 15);
        rec.eaxRoomLf = spDbc->getInt32(r, 16);
        rec.eaxDecayLfRatio = spDbc->getFloat(r, 17);
        rec.eaxEchoTime = spDbc->getFloat(r, 18);
        rec.eaxEchoDepth = spDbc->getFloat(r, 19);
        rec.eaxModulationTime = spDbc->getFloat(r, 20);
        rec.eaxModulationDepth = spDbc->getFloat(r, 21);
        rec.eaxHfReference = spDbc->getFloat(r, 22);
        rec.eaxLfReference = spDbc->getFloat(r, 23);
        providerPrefs_.emplace(id, std::move(rec));
    }

    if (auto ssDbc = assets->loadDBC("SoundSamplePreferences.dbc");
        ssDbc && ssDbc->isLoaded() && ssDbc->getFieldCount() >= 17) {
        samplePrefs_.reserve(ssDbc->getRecordCount());
        for (uint32_t r = 0; r < ssDbc->getRecordCount(); ++r) {
            const uint32_t id = ssDbc->getUInt32(r, 0);
            if (id == 0) continue;
            SoundSamplePrefRecord rec;
            rec.id = id;
            rec.direct = ssDbc->getInt32(r, 1);
            rec.directHf = ssDbc->getInt32(r, 2);
            rec.room = ssDbc->getInt32(r, 3);
            rec.roomHf = ssDbc->getInt32(r, 4);
            rec.obstruction = ssDbc->getInt32(r, 5);
            rec.obstructionLfRatio = ssDbc->getFloat(r, 6);
            rec.occlusion = ssDbc->getInt32(r, 7);
            rec.occlusionLfRatio = ssDbc->getFloat(r, 8);
            rec.occlusionRoomRatio = ssDbc->getFloat(r, 9);
            rec.occlusionDirectRatio = ssDbc->getFloat(r, 10);
            rec.exclusionLfRatio = ssDbc->getFloat(r, 11);
            rec.outsideVolumeHf = ssDbc->getInt32(r, 12);
            rec.roomRolloffFactor = ssDbc->getFloat(r, 13);
            rec.exclusion = ssDbc->getFloat(r, 14);
            rec.airAbsorptionFactor = ssDbc->getFloat(r, 15);
            rec.flags = ssDbc->getUInt32(r, 16);
            samplePrefs_.emplace(id, rec);
        }
    }

    if (auto wmoDbc = assets->loadDBC("WMOAreaTable.dbc");
        wmoDbc && wmoDbc->isLoaded() && wmoDbc->getFieldCount() >= 6) {
        wmoPrefs_.reserve(wmoDbc->getRecordCount());
        for (uint32_t r = 0; r < wmoDbc->getRecordCount(); ++r) {
            const WmoKey key{
                .rootId = wmoDbc->getUInt32(r, 1),
                .nameSet = wmoDbc->getUInt32(r, 2),
                .groupId = wmoDbc->getInt32(r, 3),
            };
            const PrefPair pair{
                .pref = wmoDbc->getInt32(r, 4),
                .underwaterPref = wmoDbc->getInt32(r, 5),
            };
            if (pair.pref != 0 || pair.underwaterPref != 0) {
                wmoPrefs_[key] = pair;
            }
        }
    }

    if (auto areaDbc = assets->loadDBC("AreaTable.dbc");
        areaDbc && areaDbc->isLoaded() && areaDbc->getFieldCount() >= 7) {
        areaPrefs_.reserve(areaDbc->getRecordCount());
        areaParents_.reserve(areaDbc->getRecordCount());
        for (uint32_t r = 0; r < areaDbc->getRecordCount(); ++r) {
            const uint32_t areaId = areaDbc->getUInt32(r, 0);
            if (areaId == 0) continue;
            const uint32_t parentId = areaDbc->getUInt32(r, 2);
            if (parentId != 0) {
                areaParents_[areaId] = parentId;
            }
            const PrefPair pair{
                .pref = areaDbc->getInt32(r, 5),
                .underwaterPref = areaDbc->getInt32(r, 6),
            };
            if (pair.pref != 0 || pair.underwaterPref != 0) {
                areaPrefs_[areaId] = pair;
            }
        }
    }

    loaded_ = !providerPrefs_.empty();
    LOG_INFO("EnvironmentReverbTable: loaded ", providerPrefs_.size(), " provider presets, ",
             samplePrefs_.size(), " sample profiles, ", wmoPrefs_.size(), " WMO acoustic rows, ",
             areaPrefs_.size(), " AreaTable acoustic rows");
    return loaded_;
}

const SoundProviderPrefRecord* EnvironmentReverbTable::findProviderPref(uint32_t id) const {
    const auto it = providerPrefs_.find(id);
    return (it != providerPrefs_.end()) ? &it->second : nullptr;
}

const SoundSamplePrefRecord* EnvironmentReverbTable::findSamplePref(uint32_t id) const {
    const auto it = samplePrefs_.find(id);
    return (it != samplePrefs_.end()) ? &it->second : nullptr;
}

bool EnvironmentReverbTable::isActivePreset(const SoundProviderPrefRecord* rec) {
    if (!rec) return false;
    // Exclude unflagged debug test rows (ID 3..5, 19: flags == 0) and PRESET_OFF (ID 66: env == -1, room == -10000).
    return rec->flags != 0u && rec->eaxEnvironmentSelection >= 0 && rec->eaxRoom > -9000;
}

bool EnvironmentReverbTable::sampleSuppressesRoom(uint32_t eaxDef) const {
    if (eaxDef == 0u) return false;
    if (const SoundSamplePrefRecord* sample = findSamplePref(eaxDef)) {
        return sample->exclusion <= -9000.0f || sample->room <= -9000;
    }
    return eaxDef == 1u;
}

int32_t EnvironmentReverbTable::lookupWmoProviderPref(uint32_t rootId, uint32_t nameSet,
                                                      uint32_t groupId, bool underwater) const {
    if (rootId == 0 && groupId == 0 && nameSet == 0) return 0;
    const int32_t grp = static_cast<int32_t>(groupId);
    const std::array<WmoKey, 4> candidates = {{
        {rootId, nameSet, grp},
        {rootId, nameSet, -1},
        {rootId, 0u, grp},
        {rootId, 0u, -1},
    }};
    for (const WmoKey& key : candidates) {
        const auto it = wmoPrefs_.find(key);
        if (it == wmoPrefs_.end()) continue;
        const int32_t val = underwater ? it->second.underwaterPref : it->second.pref;
        if (val > 0 && providerPrefs_.contains(static_cast<uint32_t>(val))) {
            return val;
        }
    }
    return 0;
}

int32_t EnvironmentReverbTable::lookupAreaProviderPref(uint32_t areaId, bool underwater) const {
    uint32_t cur = areaId;
    for (int depth = 0; cur != 0 && depth < 16; ++depth) {
        if (const auto it = areaPrefs_.find(cur); it != areaPrefs_.end()) {
            const int32_t val = underwater ? it->second.underwaterPref : it->second.pref;
            if (val > 0 && providerPrefs_.contains(static_cast<uint32_t>(val))) {
                return val;
            }
        }
        const auto pit = areaParents_.find(cur);
        if (pit == areaParents_.end() || pit->second == cur) break;
        cur = pit->second;
    }
    return 0;
}

uint32_t EnvironmentReverbTable::resolvePresetId(const EnvironmentQuery& query) const {
    if (!loaded_ || !query.reverbEnabled) {
        return 0u;
    }

    if (query.submerged) {
        if (query.hasWmoLocation) {
            const int32_t wmoUw = lookupWmoProviderPref(
                query.wmoRootId, query.wmoNameSet, query.wmoGroupId, /*underwater=*/true);
            if (wmoUw > 0) {
                const auto* rec = findProviderPref(static_cast<uint32_t>(wmoUw));
                return isActivePreset(rec) ? rec->id : 0u;
            }
        }
        int32_t areaUw = lookupAreaProviderPref(query.areaId, /*underwater=*/true);
        if (areaUw <= 0 && query.zoneId != 0 && query.zoneId != query.areaId) {
            areaUw = lookupAreaProviderPref(query.zoneId, /*underwater=*/true);
        }
        if (areaUw > 0) {
            const auto* rec = findProviderPref(static_cast<uint32_t>(areaUw));
            return isActivePreset(rec) ? rec->id : 0u;
        }
        const auto* fallbackUw = findProviderPref(kDefaultUnderwaterPresetId);
        return isActivePreset(fallbackUw) ? fallbackUw->id : 0u;
    }

    if (query.hasWmoLocation) {
        const int32_t wmoPref = lookupWmoProviderPref(
            query.wmoRootId, query.wmoNameSet, query.wmoGroupId, /*underwater=*/false);
        if (wmoPref > 0) {
            const auto* rec = findProviderPref(static_cast<uint32_t>(wmoPref));
            return isActivePreset(rec) ? rec->id : 0u;
        }
    }

    int32_t areaPref = lookupAreaProviderPref(query.areaId, /*underwater=*/false);
    if (areaPref <= 0 && query.zoneId != 0 && query.zoneId != query.areaId) {
        areaPref = lookupAreaProviderPref(query.zoneId, /*underwater=*/false);
    }
    if (areaPref > 0) {
        const auto* rec = findProviderPref(static_cast<uint32_t>(areaPref));
        return isActivePreset(rec) ? rec->id : 0u;
    }
    return 0u;
}

const SoundProviderPrefRecord* EnvironmentReverbTable::resolvePreset(const EnvironmentQuery& query) const {
    const uint32_t id = resolvePresetId(query);
    return (id != 0u) ? findProviderPref(id) : nullptr;
}

// ---------------------------------------------------------------------------
// EnvironmentReverbProcessor
// ---------------------------------------------------------------------------

EnvironmentReverbProcessor::EnvironmentReverbProcessor() {
    configure(48000u, 2u);
}

void EnvironmentReverbProcessor::configure(uint32_t sampleRate, uint32_t channels) {
    sampleRate_ = std::clamp(sampleRate, 8000u, 192000u);
    channels_ = std::max(1u, channels);
    const double srScale = static_cast<double>(sampleRate_) / 48000.0;

    // 45 ms exponential smoothing time constant for click-free room/submerge transitions.
    smoothAlpha_ = static_cast<float>(1.0 - std::exp(-1.0 / (0.045 * static_cast<double>(sampleRate_))));

    // Pre-delay ring buffer capacity: 400 ms (covers ReflectionsDelay <= 0.3s + ReverbDelay <= 0.1s).
    const size_t predelayCap = std::max<size_t>(256u, static_cast<size_t>(std::ceil(0.42 * sampleRate_)));
    predelayL_.assign(predelayCap, 0.0f);
    predelayR_.assign(predelayCap, 0.0f);
    predelayWrite_ = 0u;

    const uint32_t modHeadroom = static_cast<uint32_t>(std::ceil(0.004 * sampleRate_)) + 8u;
    for (size_t i = 0; i < kTotalCombs; ++i) {
        const uint32_t nominal = std::max(64u, static_cast<uint32_t>(std::lround(kBaseCombDelays48k[i] * srScale)));
        combs_[i].nominalDelay = nominal;
        combs_[i].buffer.assign(static_cast<size_t>(nominal + modHeadroom * 2u), 0.0f);
        combs_[i].writePos = 0u;
        combs_[i].filterState = 0.0f;
        combs_[i].lfoPhase = static_cast<float>(i) * (0.25f * std::numbers::pi_v<float>);
    }

    for (size_t ch = 0; ch < 2; ++ch) {
        for (size_t a = 0; a < kAllpassesPerChannel; ++a) {
            const uint32_t apLen = std::max(32u, static_cast<uint32_t>(
                std::lround(kBaseAllpassDelays48k[ch][a] * srScale)));
            allpasses_[ch][a].buffer.assign(apLen, 0.0f);
            allpasses_[ch][a].writePos = 0u;
        }
    }

    reset();
}

void EnvironmentReverbProcessor::reset() {
    std::fill(predelayL_.begin(), predelayL_.end(), 0.0f);
    std::fill(predelayR_.begin(), predelayR_.end(), 0.0f);
    predelayWrite_ = 0u;
    lp1L_ = lp2L_ = lp1R_ = lp2R_ = 0.0f;
    for (auto& c : combs_) {
        std::fill(c.buffer.begin(), c.buffer.end(), 0.0f);
        c.writePos = 0u;
        c.filterState = 0.0f;
    }
    for (auto& chAp : allpasses_) {
        for (auto& ap : chAp) {
            std::fill(ap.buffer.begin(), ap.buffer.end(), 0.0f);
            ap.writePos = 0u;
        }
    }
    curInputSend_ = 0.0f;
    curOutputEnable_ = 1.0f;
    curLpB_ = 1.0f;
    curReflGain_ = 0.0f;
    curRevGain_ = 0.0f;
    curReflDelay_ = 336.0f;
    curTankDelay_ = 960.0f;
    curDamp_ = 0.0f;
    curAllpassFb_ = 0.5f;
    curModExcursion_ = 0.0f;
    curCombFeedback_.fill(0.0f);
    curCombNorm_.fill(0.0f);
    runningTankEnergy_ = 0.0f;

    std::lock_guard<std::mutex> lock(targetMutex_);
    target_ = TargetState{};
}

void EnvironmentReverbProcessor::clearTargetPreset() {
    setTargetPreset(nullptr, true);
}

void EnvironmentReverbProcessor::setTargetPreset(const SoundProviderPrefRecord* preset, bool enabled) {
    TargetState next;
    next.enabled = enabled;
    next.outputEnable = enabled ? 1.0f : 0.0f;

    if (!enabled || !EnvironmentReverbTable::isActivePreset(preset)) {
        next.presetId = 0u;
        next.active = false;
        next.inputSend = 0.0f;
        std::lock_guard<std::mutex> lock(targetMutex_);
        next.lpB = target_.lpB;
        next.reflGain = target_.active ? target_.reflGain : 0.0f;
        next.revGain = target_.active ? target_.revGain : 0.0f;
        next.reflDelaySamples = target_.reflDelaySamples;
        next.tankDelaySamples = target_.tankDelaySamples;
        next.damp = target_.damp;
        next.combFeedback = target_.combFeedback;
        next.combInputNorm = target_.combInputNorm;
        next.allpassFeedback = target_.allpassFeedback;
        next.lfoIncRad = target_.lfoIncRad;
        next.modExcursionSamples = 0.0f;
        target_ = next;
        return;
    }

    next.presetId = preset->id;
    next.active = true;
    next.inputSend = 1.0f;

    // 1. Analytical 2-stage 1-pole RoomHF low-pass filter cutoff from (eaxRoomHf, eaxHfReference).
    const float hfRef = std::clamp(preset->eaxHfReference, 1000.0f, 20000.0f);
    const float gHf = millibelsToLinear(static_cast<float>(preset->eaxRoomHf));
    float fc = 18000.0f;
    if (gHf < 0.999f) {
        const float invRatio = std::max(1.0f / std::max(gHf, 1e-6f) - 1.0f, 1e-4f);
        fc = hfRef / std::sqrt(invRatio);
    }
    fc = std::clamp(fc, 280.0f, std::min(18000.0f, 0.45f * static_cast<float>(sampleRate_)));
    const float aLp = std::exp(-2.0f * std::numbers::pi_v<float> * fc / static_cast<float>(sampleRate_));
    next.lpB = std::clamp(1.0f - aLp, 0.005f, 1.0f);

    // 2. Early reflections & late reverb I3DL2 linear gains and delays.
    next.reflGain = std::clamp(
        millibelsToLinear(static_cast<float>(preset->eaxRoom + preset->eaxReflections)), 0.0f, 4.0f);
    next.revGain = std::clamp(
        millibelsToLinear(static_cast<float>(preset->eaxRoom + preset->eaxReverb)), 0.0f, 4.0f);

    const uint32_t maxDelay = static_cast<uint32_t>(predelayL_.size() > 8 ? predelayL_.size() - 8 : 1);
    const float reflSec = std::clamp(preset->eaxReflectionsDelay, 0.001f, 0.300f);
    const float revSec = std::clamp(preset->eaxReverbDelay, 0.001f, 0.100f);
    next.reflDelaySamples = std::clamp(
        static_cast<uint32_t>(std::lround(reflSec * static_cast<float>(sampleRate_))), 1u, maxDelay / 3u);
    next.tankDelaySamples = std::clamp(
        static_cast<uint32_t>(std::lround((reflSec + revSec) * static_cast<float>(sampleRate_))),
        2u, maxDelay);

    // 3. Feedback comb T60 and HF loop damping.
    const float t60Lf = std::clamp(preset->decayTime, 0.10f, 20.0f);
    const float hfRatio = std::clamp(preset->eaxDecayHfRatio, 0.10f, 2.0f);
    const float t60Hf = std::max(0.05f, t60Lf * hfRatio);
    next.damp = (t60Hf < t60Lf) ? std::clamp(1.0f - (t60Hf / t60Lf), 0.0f, 0.85f) : 0.0f;

    for (size_t i = 0; i < kTotalCombs; ++i) {
        const float delaySec = static_cast<float>(combs_[i].nominalDelay) / static_cast<float>(sampleRate_);
        const float gFb = std::clamp(std::pow(10.0f, -3.0f * delaySec / t60Lf), 0.0f, 0.985f);
        next.combFeedback[i] = gFb;
        next.combInputNorm[i] = std::sqrt(std::max(0.0f, 1.0f - gFb * gFb) / static_cast<float>(kCombsPerChannel));
    }

    next.allpassFeedback = 0.50f * std::clamp(preset->environmentDiffusion, 0.0f, 1.0f);

    // 4. Chorus / underwater delay-line LFO modulation (ModulationTime, ModulationDepth).
    const float modTime = std::clamp(preset->eaxModulationTime, 0.10f, 10.0f);
    const float modDepth = std::clamp(preset->eaxModulationDepth, 0.0f, 1.0f);
    next.lfoIncRad = (2.0f * std::numbers::pi_v<float>) / (modTime * static_cast<float>(sampleRate_));
    next.modExcursionSamples = modDepth * (0.001f * static_cast<float>(sampleRate_));

    std::lock_guard<std::mutex> lock(targetMutex_);
    target_ = next;
}

uint32_t EnvironmentReverbProcessor::activePresetId() const {
    std::lock_guard<std::mutex> lock(targetMutex_);
    return target_.presetId;
}

bool EnvironmentReverbProcessor::isEnabled() const {
    std::lock_guard<std::mutex> lock(targetMutex_);
    return target_.enabled;
}

void EnvironmentReverbProcessor::processFrames(const float* framesIn, float* framesOut, uint32_t frameCount) {
    if (!framesOut || frameCount == 0u) return;

    TargetState tgt;
    {
        std::lock_guard<std::mutex> lock(targetMutex_);
        tgt = target_;
    }

    const uint32_t ch = channels_;
    if (ch < 2u) {
        if (framesIn) {
            std::copy_n(framesIn, frameCount * ch, framesOut);
        } else {
            std::fill_n(framesOut, frameCount * ch, 0.0f);
        }
        return;
    }

    if (!framesIn && !tgt.active && curInputSend_ < 1e-5f && runningTankEnergy_ < 1e-11f) {
        std::fill_n(framesOut, static_cast<size_t>(frameCount) * ch, 0.0f);
        return;
    }

    if (!tgt.active && curInputSend_ < 1e-5f && runningTankEnergy_ < 1e-11f) {
        if (framesIn != framesOut) {
            std::copy_n(framesIn, static_cast<size_t>(frameCount) * ch, framesOut);
        }
        return;
    }

    const uint32_t pdSize = static_cast<uint32_t>(predelayL_.size());
    const float pdMaxDelay = static_cast<float>(pdSize > 4u ? pdSize - 3u : 1u);
    const float targetReflDelay = std::clamp(static_cast<float>(tgt.reflDelaySamples), 1.0f, pdMaxDelay / 2.7f);
    const float targetTankDelay = std::clamp(static_cast<float>(tgt.tankDelaySamples), 2.0f, pdMaxDelay);

    const float alpha = smoothAlpha_;
    float blockWetSq = 0.0f;

    for (uint32_t f = 0; f < frameCount; ++f) {
        const size_t base = static_cast<size_t>(f) * ch;
        const float dryL = framesIn ? framesIn[base] : 0.0f;
        const float dryR = framesIn ? framesIn[base + 1u] : 0.0f;

        curInputSend_ += alpha * (tgt.inputSend - curInputSend_);
        curOutputEnable_ += alpha * (tgt.outputEnable - curOutputEnable_);
        curLpB_ += alpha * (tgt.lpB - curLpB_);
        curReflGain_ += alpha * (tgt.reflGain - curReflGain_);
        curRevGain_ += alpha * (tgt.revGain - curRevGain_);
        curReflDelay_ += alpha * (targetReflDelay - curReflDelay_);
        curTankDelay_ += alpha * (targetTankDelay - curTankDelay_);
        curDamp_ += alpha * (tgt.damp - curDamp_);
        curAllpassFb_ += alpha * (tgt.allpassFeedback - curAllpassFb_);
        curModExcursion_ += alpha * (tgt.modExcursionSamples - curModExcursion_);
        for (size_t i = 0; i < kTotalCombs; ++i) {
            curCombFeedback_[i] += alpha * (tgt.combFeedback[i] - curCombFeedback_[i]);
            curCombNorm_[i] += alpha * (tgt.combInputNorm[i] - curCombNorm_[i]);
        }

        const float sendL = dryL * curInputSend_;
        const float sendR = dryR * curInputSend_;

        const float lpA = 1.0f - curLpB_;
        lp1L_ = curLpB_ * sendL + lpA * lp1L_;
        lp2L_ = curLpB_ * lp1L_ + lpA * lp2L_;
        lp1R_ = curLpB_ * sendR + lpA * lp1R_;
        lp2R_ = curLpB_ * lp1R_ + lpA * lp2R_;

        predelayL_[predelayWrite_] = lp2L_;
        predelayR_[predelayWrite_] = lp2R_;

        auto readPdInterp = [&](const std::vector<float>& buf, float delaySamples) -> float {
            const float clamped = std::clamp(delaySamples, 1.0f, pdMaxDelay);
            float rPos = static_cast<float>(predelayWrite_) - clamped;
            while (rPos < 0.0f) rPos += static_cast<float>(pdSize);
            const uint32_t r0 = static_cast<uint32_t>(rPos) % pdSize;
            const float frac = rPos - std::floor(rPos);
            return buf[r0] * (1.0f - frac) + buf[(r0 + 1u) % pdSize] * frac;
        };

        const float d0 = curReflDelay_;
        const float d1 = curReflDelay_ * 1.43f;
        const float d2 = curReflDelay_ * 1.97f;
        const float d3 = curReflDelay_ * 2.61f;
        const float earlyL = curReflGain_ * (
            0.50f * readPdInterp(predelayL_, d0) + 0.35f * readPdInterp(predelayR_, d1) +
            0.25f * readPdInterp(predelayL_, d2) + 0.18f * readPdInterp(predelayR_, d3));
        const float earlyR = curReflGain_ * (
            0.50f * readPdInterp(predelayR_, d0) + 0.35f * readPdInterp(predelayL_, d1) +
            0.25f * readPdInterp(predelayR_, d2) + 0.18f * readPdInterp(predelayL_, d3));

        const float rawTankL = readPdInterp(predelayL_, curTankDelay_);
        const float rawTankR = readPdInterp(predelayR_, curTankDelay_);
        const float tankInL = 0.80f * rawTankL + 0.20f * rawTankR;
        const float tankInR = 0.80f * rawTankR + 0.20f * rawTankL;

        predelayWrite_ = (predelayWrite_ + 1u) % pdSize;

        float combSumL = 0.0f;
        float combSumR = 0.0f;
        const float oneMinusDamp = 1.0f - curDamp_;
        const bool modulate = curModExcursion_ > 0.005f;

        for (size_t i = 0; i < kTotalCombs; ++i) {
            CombLine& c = combs_[i];
            const uint32_t cCap = static_cast<uint32_t>(c.buffer.size());
            float y = 0.0f;
            if (modulate) {
                const float dCur = static_cast<float>(c.nominalDelay) +
                                   curModExcursion_ * std::sin(c.lfoPhase);
                c.lfoPhase += tgt.lfoIncRad;
                if (c.lfoPhase > 2.0f * std::numbers::pi_v<float>) {
                    c.lfoPhase -= 2.0f * std::numbers::pi_v<float>;
                }
                float rPos = static_cast<float>(c.writePos) - dCur;
                while (rPos < 0.0f) rPos += static_cast<float>(cCap);
                const uint32_t r0 = static_cast<uint32_t>(rPos) % cCap;
                const float frac = rPos - std::floor(rPos);
                y = c.buffer[r0] * (1.0f - frac) + c.buffer[(r0 + 1u) % cCap] * frac;
            } else {
                y = c.buffer[(c.writePos + cCap - c.nominalDelay) % cCap];
            }

            c.filterState = oneMinusDamp * y + curDamp_ * c.filterState;
            if (std::abs(c.filterState) < 1e-20f) c.filterState = 0.0f;

            const float excitation = (i < kCombsPerChannel) ? tankInL : tankInR;
            c.buffer[c.writePos] = curCombNorm_[i] * excitation + curCombFeedback_[i] * c.filterState;
            c.writePos = (c.writePos + 1u) % cCap;

            if (i < kCombsPerChannel) combSumL += y;
            else combSumR += y;
        }

        const float apFb = curAllpassFb_;
        auto runAllpass = [&](AllpassLine& ap, float xIn) -> float {
            const uint32_t apCap = static_cast<uint32_t>(ap.buffer.size());
            const float bOut = ap.buffer[ap.writePos];
            float v = xIn + apFb * bOut;
            if (std::abs(v) < 1e-20f) v = 0.0f;
            ap.buffer[ap.writePos] = v;
            ap.writePos = (ap.writePos + 1u) % apCap;
            return bOut - apFb * v;
        };

        const float diffL = runAllpass(allpasses_[0][1], runAllpass(allpasses_[0][0], combSumL));
        const float diffR = runAllpass(allpasses_[1][1], runAllpass(allpasses_[1][0], combSumR));

        auto softLimitWet = [](float w) -> float {
            const float a = std::abs(w);
            if (a <= 0.5f) return w;
            return std::copysign(0.5f + 0.5f * std::tanh((a - 0.5f) * 2.0f), w);
        };

        const float wetL = softLimitWet(curOutputEnable_ * (earlyL + curRevGain_ * diffL));
        const float wetR = softLimitWet(curOutputEnable_ * (earlyR + curRevGain_ * diffR));
        blockWetSq += 0.5f * (wetL * wetL + wetR * wetR);

        framesOut[base] = dryL + wetL;
        framesOut[base + 1u] = dryR + wetR;
        for (uint32_t extra = 2u; extra < ch; ++extra) {
            framesOut[base + extra] = framesIn ? framesIn[base + extra] : 0.0f;
        }
    }

    runningTankEnergy_ = 0.85f * runningTankEnergy_ + 0.15f * (blockWetSq / static_cast<float>(frameCount));
    if (runningTankEnergy_ < 1e-20f) runningTankEnergy_ = 0.0f;
}

} // namespace audio
} // namespace wowee
