#include "audio/footstep_manager.hpp"
#include "audio/audio_engine.hpp"
#include "audio/creature_sound_index.hpp"
#include "audio/sound_entries.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "rendering/footfall_window.hpp"
#include "core/logger.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace wowee {
namespace audio {

namespace {

bool isCharacterDispatchedEvent(pipeline::M2AudioEventType type) noexcept {
    using T = pipeline::M2AudioEventType;
    switch (type) {
        case T::Footstep:
        case T::DeathThud:
        case T::ArmorFoley:
        case T::CustomSound:
        case T::SpellCastDirected:
        case T::Submerge:
        case T::Submerged:
        case T::Sound:
        case T::DoodadSoundOneShot:
            return true;
        default:
            return false;
    }
}

bool isCharacterBridgeEvent(pipeline::M2AudioEventType type) noexcept {
    using T = pipeline::M2AudioEventType;
    switch (type) {
        case T::CombatSwing:
        case T::CombatAttackHit:
        case T::ArmorHit0:
        case T::ArmorHit1:
        case T::ArmorHit2:
        case T::ArmorHit3:
        case T::BowPull:
        case T::BowRelease:
        case T::SheatheLeft:
        case T::SheatheRight:
        case T::EmoteSound:
            return true;
        default:
            return false;
    }
}

} // namespace

FootstepManager::FootstepManager() : rng(std::random_device{}()) {}

FootstepManager::~FootstepManager() {
    shutdown();
}

bool FootstepManager::initialize(pipeline::AssetManager* assets) {
    shutdown();
    assetManager = assets;
    if (!assetManager) return false;
    sounds_ = std::make_unique<CreatureSoundIndex>();
    if (!sounds_->load(*assetManager)) {
        sounds_.reset();
        assetManager = nullptr;
        return false;
    }
    if (const auto dt = assetManager->loadDBC("DeathThudLookups.dbc"); dt && dt->isLoaded()) {
        sounds_->loadDeathThudLookups(*dt);
    }
    if (const auto mat = assetManager->loadDBC("Material.dbc"); mat && mat->isLoaded()) {
        sounds_->loadMaterials(*mat);
    }
    if (const auto godi = assetManager->loadDBC("GameObjectDisplayInfo.dbc"); godi && godi->isLoaded()) {
        sounds_->loadGameObjectDisplays(*godi);
    }
    if (const auto idi = assetManager->loadDBC("ItemDisplayInfo.dbc"); idi && idi->isLoaded()) {
        sounds_->loadItemDisplayArmorMaterials(*idi);
    }
    return true;
}

void FootstepManager::shutdown() {
    stopDrowningLoop();
    clearAllCreatureLoops();
    clearAllModelAudioInstances();
    clearAllCharacterAudioInstances();
    sounds_.reset();
    assetManager = nullptr;
}

void FootstepManager::update(float) {}

bool FootstepManager::playSoundSpatialOrSelf(uint32_t soundId, const glm::vec3& position,
                                             bool isLocalPlayer, bool listenerAtCharacter,
                                             uint32_t categoryOverride, bool forceOneShot) {
    if (soundId == 0 || !assetManager || volumeScale <= 0.0f || !AudioEngine::instance().isInitialized()) {
        return false;
    }
    const SoundLoopMode loopMode = forceOneShot ? SoundLoopMode::ForceOneShot : SoundLoopMode::FromEntryFlags;
    if (isLocalPlayer) {
        if (listenerAtCharacter) {
            return SoundEntryTable::instance().play2D(
                assetManager, soundId, volumeScale * kLocalSelf2DGain, 1.0f, categoryOverride, loopMode);
        }
        return SoundEntryTable::instance().play3D(
            assetManager, soundId, position + glm::vec3(0.0f, 0.0f, kLocalListenerZOffset),
            volumeScale, 1.0f, categoryOverride, loopMode);
    }
    return SoundEntryTable::instance().play3D(
        assetManager, soundId, position, volumeScale, 1.0f, categoryOverride, loopMode);
}

void FootstepManager::playFootstep(uint32_t displayId, uint32_t terrainTypeId, bool wading) {
    (void)playFootstepAt(displayId, terrainTypeId, wading, localPlayerBarefoot_,
                         glm::vec3(0.0f), /*isLocalPlayer=*/true, /*listenerAtCharacter=*/true);
}

bool FootstepManager::playFootstepAt(uint32_t displayId, uint32_t terrainTypeId, bool wading,
                                     bool bareFeet, const glm::vec3& position,
                                     bool isLocalPlayer, bool listenerAtCharacter) {
    if (!sounds_ || !assetManager || !AudioEngine::instance().isInitialized()) return false;
    const uint32_t soundId = sounds_->footstepSound(displayId, terrainTypeId, wading, bareFeet);
    if (soundId == 0) {
        static std::unordered_set<uint64_t> reported;
        if (reported.insert((uint64_t{displayId} << 32) | terrainTypeId).second) {
            LOG_DEBUG("Footstep: display ", displayId, " on terrain type ", terrainTypeId,
                      wading ? " (wading)" : "", " has no FootstepTerrainLookup sound");
        }
        return false;
    }
    // Wow.exe PlayFootstepSound (0x465ae0): local player uses category 0x11 (kFootstepSelf, cap 4),
    // remote units use category 0x0D (kFootstepOther, cap 6), culled at SoundEntries.DistanceCutoff.
    const uint32_t category = isLocalPlayer
        ? SESoundCategory::kFootstepSelf
        : SESoundCategory::kFootstepOther;
    return playSoundSpatialOrSelf(soundId, position, isLocalPlayer, listenerAtCharacter, category);
}

bool FootstepManager::playDeathThud(uint32_t displayId, uint32_t terrainTypeId, bool inWater,
                                    const glm::vec3& position,
                                    bool isLocalPlayer, bool listenerAtCharacter) {
    if (!sounds_ || !assetManager || !AudioEngine::instance().isInitialized()) return false;
    const uint32_t soundId = sounds_->deathThudSound(displayId, terrainTypeId, inWater);
    if (soundId == 0) return false;
    return playSoundSpatialOrSelf(soundId, position, isLocalPlayer, listenerAtCharacter);
}

bool FootstepManager::playArmorFoley(uint32_t materialId, const glm::vec3& position,
                                     bool isLocalPlayer, bool listenerAtCharacter) {
    if (!sounds_ || !assetManager || !AudioEngine::instance().isInitialized()) return false;
    if (isLocalPlayer ? !armorFoleySelfEnabled_ : !armorFoleyOthersEnabled_) return false;
    const uint32_t soundId = sounds_->armorFoleySound(materialId);
    if (soundId == 0) return false;
    return playSoundSpatialOrSelf(soundId, position, isLocalPlayer, listenerAtCharacter);
}

bool FootstepManager::playCreatureWingSound(uint32_t displayId, bool glide, const glm::vec3& position,
                                            bool isLocalPlayer, bool listenerAtCharacter) {
    if (!sounds_ || !assetManager || !AudioEngine::instance().isInitialized() || displayId == 0) return false;
    const uint32_t soundDataId = sounds_->soundDataForDisplay(displayId);
    if (soundDataId == 0) return false;
    const uint32_t soundId = sounds_->sound(
        soundDataId, glide ? CreatureSoundColumn::WingGlide : CreatureSoundColumn::WingFlap);
    if (soundId == 0) return false;
    return playSoundSpatialOrSelf(soundId, position, isLocalPlayer, listenerAtCharacter);
}

bool FootstepManager::playUnitM2Event(pipeline::M2AudioEventType type, uint32_t eventData,
                                      uint32_t displayId, uint32_t terrainTypeId, bool wadingOrInWater,
                                      bool bareFeet, uint32_t armorMaterialId, const glm::vec3& position,
                                      bool isLocalPlayer, bool listenerAtCharacter, bool skipFootstep) {
    if (!sounds_ || !assetManager || !AudioEngine::instance().isInitialized()) return false;
    using T = pipeline::M2AudioEventType;
    switch (type) {
        case T::Footstep:
            if (skipFootstep) return false;
            return playFootstepAt(displayId, terrainTypeId, wadingOrInWater, bareFeet,
                                  position, isLocalPlayer, listenerAtCharacter);
        case T::DeathThud:
            return playDeathThud(displayId, terrainTypeId, wadingOrInWater,
                                 position, isLocalPlayer, listenerAtCharacter);
        case T::ArmorFoley:
            return playArmorFoley(armorMaterialId, position, isLocalPlayer, listenerAtCharacter);
        case T::CustomSound: {
            const uint32_t soundId = sounds_->customAttackSound(displayId, eventData);
            return playSoundSpatialOrSelf(soundId, position, isLocalPlayer, listenerAtCharacter);
        }
        case T::SpellCastDirected: {
            const uint32_t soundId = sounds_->sound(
                sounds_->soundDataForDisplay(displayId), CreatureSoundColumn::SpellCastDirected);
            return playSoundSpatialOrSelf(soundId, position, isLocalPlayer, listenerAtCharacter);
        }
        case T::Submerge: {
            const uint32_t soundId = sounds_->sound(
                sounds_->soundDataForDisplay(displayId), CreatureSoundColumn::Submerge);
            return playSoundSpatialOrSelf(soundId, position, isLocalPlayer, listenerAtCharacter);
        }
        case T::Submerged: {
            const uint32_t soundId = sounds_->sound(
                sounds_->soundDataForDisplay(displayId), CreatureSoundColumn::Submerged);
            return playSoundSpatialOrSelf(soundId, position, isLocalPlayer, listenerAtCharacter);
        }
        case T::Sound:
            return playSoundSpatialOrSelf(eventData, position, isLocalPlayer, listenerAtCharacter);
        case T::DoodadSoundOneShot:
            return playSoundSpatialOrSelf(eventData, position, isLocalPlayer, listenerAtCharacter,
                                          SoundEntryTable::kAutoCategory, /*forceOneShot=*/true);
        default:
            return false;
    }
}

uint32_t FootstepManager::stepModelAudioEvents(
    uint32_t instanceId,
    const std::vector<pipeline::M2Model::AudioEvent>& events,
    int sequenceIndex,
    float animTimeMs,
    float animDurationMs,
    bool heldAtEnd,
    const std::function<glm::vec3(uint16_t bone, const glm::vec3& localOffset)>& boneWorldPos,
    const std::function<uint32_t(uint32_t goSlotIndex)>& goSlotSoundResolver) {
    if (instanceId == 0 || events.empty() || !assetManager) return 0;

    auto& cursor = modelAudioCursors_[instanceId];
    auto stopActiveLoop = [&]() {
        if (cursor.activeLoopHandle != 0) {
            if (AudioEngine::instance().isInitialized()) {
                AudioEngine::instance().stopSound(cursor.activeLoopHandle);
            }
            cursor.activeLoopHandle = 0;
            cursor.activeLoopSoundId = 0;
            cursor.activeLoopSequence = -1;
        }
    };

    if (sequenceIndex < 0) {
        stopActiveLoop();
        cursor.prevSequence = -1;
        return 0;
    }

    if (cursor.activeLoopHandle != 0) {
        if (cursor.activeLoopSequence != sequenceIndex) {
            stopActiveLoop();
        } else if (!AudioEngine::instance().isSoundPlaying(cursor.activeLoopHandle)) {
            cursor.activeLoopHandle = 0;
            cursor.activeLoopSoundId = 0;
            cursor.activeLoopSequence = -1;
        } else if (boneWorldPos) {
            AudioEngine::instance().setSoundPosition(
                cursor.activeLoopHandle,
                boneWorldPos(cursor.activeLoopBone, cursor.activeLoopLocalOffset));
        }
    }

    if (heldAtEnd && animDurationMs > 0.0f && animTimeMs >= animDurationMs) {
        cursor.prevSequence = sequenceIndex;
        cursor.prevTimeMs = animTimeMs;
        return 0;
    }

    float fromMs = -1.0f;
    if (cursor.prevSequence == sequenceIndex) {
        fromMs = cursor.prevTimeMs;
    } else if (animTimeMs > 250.0f) {
        // Instance entered view mid-sequence: sync cursor without retroactively firing past events.
        cursor.prevSequence = sequenceIndex;
        cursor.prevTimeMs = animTimeMs;
        return 0;
    }

    cursor.prevSequence = sequenceIndex;
    cursor.prevTimeMs = animTimeMs;
    if (fromMs == animTimeMs) return 0;

    if (volumeScale <= 0.0f || !AudioEngine::instance().isInitialized()) return 0;

    uint32_t firedCount = 0;
    using T = pipeline::M2AudioEventType;
    for (const auto& ev : events) {
        if (static_cast<size_t>(sequenceIndex) >= ev.times.size()) continue;
        const auto& stamps = ev.times[sequenceIndex];
        if (stamps.empty()) continue;

        bool crossed = false;
        for (const uint32_t t : stamps) {
            if (rendering::footfallPassed(t, fromMs, animTimeMs)) {
                crossed = true;
                break;
            }
        }
        if (!crossed) continue;

        const glm::vec3 worldPos = boneWorldPos ? boneWorldPos(ev.bone, ev.position) : ev.position;
        switch (ev.type) {
            case T::Sound:
                if (ev.data != 0 &&
                    SoundEntryTable::instance().play3D(assetManager, ev.data, worldPos, volumeScale)) {
                    ++firedCount;
                }
                break;
            case T::DoodadSoundOneShot:
                // $DSO explicitly plays a single one-shot per animation keyframe crossing, even when
                // the referenced SoundEntries.dbc record carries 0x0200 (e.g. SE:3431 DeadMineSteamWhistleon,
                // SE:9370 Cocoon_YellowDeath, SE:9116 hellfire_dw_converClosed.wav).
                if (ev.data != 0 &&
                    SoundEntryTable::instance().play3D(assetManager, ev.data, worldPos, volumeScale,
                                                       1.0f, SoundEntryTable::kAutoCategory,
                                                       SoundLoopMode::ForceOneShot)) {
                    ++firedCount;
                }
                break;
            case T::DoodadSoundLoop:
                // $DSL explicitly loops while the sequence remains active, including the 122 TBC M2
                // $DSL entries whose SoundEntries.dbc row omits 0x0200 (e.g. SE:3246 FlameCircleLoop).
                if (ev.data != 0) {
                    if (cursor.activeLoopHandle != 0 && cursor.activeLoopSoundId == ev.data) break;
                    stopActiveLoop();
                    cursor.activeLoopHandle = SoundEntryTable::instance().play3DStoppable(
                        assetManager, ev.data, worldPos, volumeScale, 1.0f,
                        SoundEntryTable::kAutoCategory, SoundLoopMode::ForceLoop);
                    if (cursor.activeLoopHandle != 0) {
                        cursor.activeLoopSoundId = ev.data;
                        cursor.activeLoopSequence = sequenceIndex;
                        cursor.activeLoopBone = ev.bone;
                        cursor.activeLoopLocalOffset = ev.position;
                        ++firedCount;
                    }
                }
                break;
            case T::GameObject0:
            case T::GameObject1:
            case T::GameObject3:
            case T::GameObject4:
            case T::GameObject5: {
                if (!goSlotSoundResolver) break;
                uint32_t slot = 0;
                if (ev.type == T::GameObject1) slot = 1;
                else if (ev.type == T::GameObject3) slot = 3;
                else if (ev.type == T::GameObject4) slot = 4;
                else if (ev.type == T::GameObject5) slot = 5;
                const uint32_t soundId = goSlotSoundResolver(slot);
                if (soundId != 0 &&
                    SoundEntryTable::instance().play3D(assetManager, soundId, worldPos, volumeScale,
                                                       1.0f, SoundEntryTable::kAutoCategory,
                                                       SoundLoopMode::ForceOneShot)) {
                    ++firedCount;
                }
                break;
            }
            case T::GameObject2: {
                // GameObjectDisplayInfo col 4 = Loop sound while in that state sequence.
                if (!goSlotSoundResolver) break;
                const uint32_t soundId = goSlotSoundResolver(2u);
                if (soundId != 0) {
                    if (cursor.activeLoopHandle != 0 && cursor.activeLoopSoundId == soundId) break;
                    stopActiveLoop();
                    cursor.activeLoopHandle = SoundEntryTable::instance().play3DStoppable(
                        assetManager, soundId, worldPos, volumeScale, 1.0f,
                        SoundEntryTable::kAutoCategory, SoundLoopMode::ForceLoop);
                    if (cursor.activeLoopHandle != 0) {
                        cursor.activeLoopSoundId = soundId;
                        cursor.activeLoopSequence = sequenceIndex;
                        cursor.activeLoopBone = ev.bone;
                        cursor.activeLoopLocalOffset = ev.position;
                        ++firedCount;
                    }
                }
                break;
            }
            case T::GameObjectCustom0:
            case T::GameObjectCustom1:
            case T::GameObjectCustom2:
            case T::GameObjectCustom3: {
                if (!goSlotSoundResolver) break;
                const uint32_t slot = 6u + (static_cast<uint32_t>(ev.type) -
                                            static_cast<uint32_t>(T::GameObjectCustom0));
                const uint32_t soundId = goSlotSoundResolver(slot);
                if (soundId != 0 &&
                    SoundEntryTable::instance().play3D(assetManager, soundId, worldPos, volumeScale)) {
                    ++firedCount;
                }
                break;
            }
            default:
                break;
        }
    }
    return firedCount;
}

void FootstepManager::stopModelAudioInstance(uint32_t instanceId) {
    const auto it = modelAudioCursors_.find(instanceId);
    if (it == modelAudioCursors_.end()) return;
    if (it->second.activeLoopHandle != 0 && AudioEngine::instance().isInitialized()) {
        AudioEngine::instance().stopSound(it->second.activeLoopHandle);
    }
    modelAudioCursors_.erase(it);
}

void FootstepManager::clearAllModelAudioInstances() {
    if (AudioEngine::instance().isInitialized()) {
        for (const auto& [_, cursor] : modelAudioCursors_) {
            if (cursor.activeLoopHandle != 0) {
                AudioEngine::instance().stopSound(cursor.activeLoopHandle);
            }
        }
    }
    modelAudioCursors_.clear();
}

bool FootstepManager::hasActiveModelLoop(uint32_t instanceId) const {
    const auto it = modelAudioCursors_.find(instanceId);
    return it != modelAudioCursors_.end() && it->second.activeLoopHandle != 0;
}

uint32_t FootstepManager::stepCharacterAudioEvents(
    uint32_t instanceId,
    const pipeline::M2Model& model,
    int baseSeqIndex,
    float baseTimeMs,
    bool playReversed,
    int upperSeqIndex,
    float upperTimeMs,
    const CharacterAudioStepContext& ctx,
    const std::function<glm::vec3(uint16_t bone, const glm::vec3& localOffset)>& boneWorldPos) {
    if (instanceId == 0 || model.audioEvents.empty()) return 0;

    auto& cursor = charAudioCursors_[instanceId];
    uint32_t firedCount = 0;

    auto stepLayer = [&](int seqIdx, float toMs, bool reversed, bool isUpperLayer,
                         int& prevSeq, float& prevTimeMs) {
        if (seqIdx < 0 || static_cast<size_t>(seqIdx) >= model.sequences.size()) {
            prevSeq = -1;
            return;
        }
        const float seqDur = static_cast<float>(model.sequences[seqIdx].duration);
        float fromMs = -1.0f;
        if (prevSeq == seqIdx) {
            fromMs = prevTimeMs;
        } else if (toMs > 250.0f && (!reversed || (seqDur - toMs) > 250.0f)) {
            prevSeq = seqIdx;
            prevTimeMs = toMs;
            return;
        }
        prevSeq = seqIdx;
        prevTimeMs = toMs;
        if (fromMs == toMs || ctx.muteEvents) return;

        auto mapEventTime = [&](uint32_t eventMs) -> uint32_t {
            if (!reversed || seqDur <= 0.0f) return eventMs;
            const float rev = std::max(0.0f, seqDur - static_cast<float>(eventMs));
            return static_cast<uint32_t>(std::lround(rev));
        };

        for (const auto& ev : model.audioEvents) {
            const bool bridgeEv = isCharacterBridgeEvent(ev.type);
            if (!bridgeEv && !isCharacterDispatchedEvent(ev.type)) continue;
            if (bridgeEv && !ctx.onCustomUnitEvent) continue;
            // Upper-body overlay (emotes/casts while running) must not duplicate footstep/tread events.
            if (isUpperLayer &&
                (ev.type == pipeline::M2AudioEventType::Footstep ||
                 ev.type == pipeline::M2AudioEventType::ArmorFoley)) {
                continue;
            }
            if (ev.type == pipeline::M2AudioEventType::Footstep && !ctx.hasTerrainType) {
                continue;
            }
            if (static_cast<size_t>(seqIdx) >= ev.times.size()) continue;
            const auto& stamps = ev.times[seqIdx];
            if (stamps.empty()) continue;

            bool crossed = false;
            for (const uint32_t rawT : stamps) {
                const uint32_t mappedT = mapEventTime(rawT);
                const bool hit = reversed
                    ? rendering::footfallPassed(mappedT,
                                                fromMs < 0.0f ? -1.0f : std::max(0.0f, seqDur - fromMs),
                                                std::max(0.0f, seqDur - toMs))
                    : rendering::footfallPassed(mappedT, fromMs, toMs);
                if (hit) {
                    crossed = true;
                    break;
                }
            }
            if (!crossed) continue;

            const glm::vec3 worldPos = boneWorldPos ? boneWorldPos(ev.bone, ev.position) : ev.position;
            if (bridgeEv) {
                if (ctx.onCustomUnitEvent(ev.type, ev.data, model.sequences[seqIdx].id, worldPos)) {
                    ++firedCount;
                }
                continue;
            }
            const bool waterState = (ev.type == pipeline::M2AudioEventType::DeathThud)
                ? ctx.inWater
                : ctx.wading;
            if (playUnitM2Event(ev.type, ev.data, ctx.displayId, ctx.terrainTypeId, waterState,
                                ctx.bareFeet, ctx.armorMaterialId, worldPos,
                                ctx.isLocalPlayer, ctx.listenerAtCharacter, ctx.skipFootstep)) {
                ++firedCount;
            }
        }
    };

    stepLayer(baseSeqIndex, baseTimeMs, playReversed, /*isUpperLayer=*/false,
              cursor.prevBaseSeq, cursor.prevBaseTimeMs);
    if (upperSeqIndex >= 0 && upperSeqIndex != baseSeqIndex) {
        stepLayer(upperSeqIndex, upperTimeMs, /*reversed=*/false, /*isUpperLayer=*/true,
                  cursor.prevUpperSeq, cursor.prevUpperTimeMs);
    } else {
        cursor.prevUpperSeq = -1;
    }

    return firedCount;
}

void FootstepManager::removeCharacterAudioInstance(uint32_t instanceId) {
    charAudioCursors_.erase(instanceId);
}

void FootstepManager::clearAllCharacterAudioInstances() {
    charAudioCursors_.clear();
}

void FootstepManager::updateDrowningState(bool isDrowning) {
    if (!isDrowning) {
        stopDrowningLoop();
        return;
    }
    if (!assetManager || volumeScale <= 0.0f || !AudioEngine::instance().isInitialized()) return;
    if (drowningLoopHandle_ == 0) {
        drowningLoopHandle_ = SoundEntryTable::instance().play2DStoppable(
            assetManager, kPlayerDrowningLoopSoundId, volumeScale * kLocalSelf2DGain,
            1.0f, SoundEntryTable::kAutoCategory, SoundLoopMode::ForceLoop);
    }
}

void FootstepManager::onDrowningDamageTick(uint32_t playerDisplayId, const glm::vec3& position,
                                           bool listenerAtCharacter) {
    if (!assetManager || volumeScale <= 0.0f || !AudioEngine::instance().isInitialized()) return;
    (void)playSoundSpatialOrSelf(kDrowningBubbleSoundId, position,
                                 /*isLocalPlayer=*/true, listenerAtCharacter);
    if (sounds_ && playerDisplayId != 0) {
        const uint32_t injurySoundId = sounds_->sound(
            sounds_->soundDataForDisplay(playerDisplayId), CreatureSoundColumn::Injury);
        if (injurySoundId != 0) {
            (void)playSoundSpatialOrSelf(injurySoundId, position,
                                         /*isLocalPlayer=*/true, listenerAtCharacter);
        }
    }
}

void FootstepManager::stopDrowningLoop() {
    if (drowningLoopHandle_ != 0) {
        if (AudioEngine::instance().isInitialized()) {
            AudioEngine::instance().stopSound(drowningLoopHandle_);
        }
        drowningLoopHandle_ = 0;
    }
}

void FootstepManager::syncCreatureLoopSound(uint64_t guid, uint32_t displayId,
                                            const glm::vec3& position, bool alive) {
    if (guid == 0) return;
    if (!alive || !sounds_ || !assetManager || volumeScale <= 0.0f ||
        !AudioEngine::instance().isInitialized()) {
        stopCreatureLoopSound(guid);
        return;
    }

    const uint32_t loopSoundId = sounds_->sound(
        sounds_->soundDataForDisplay(displayId), CreatureSoundColumn::LoopSound);
    if (loopSoundId == 0) {
        stopCreatureLoopSound(guid);
        return;
    }

    const auto it = creatureLoops_.find(guid);
    if (it != creatureLoops_.end()) {
        if (it->second.soundId == loopSoundId && it->second.handle != 0) {
            return; // Already looping this profile
        }
        AudioEngine::instance().stopSound(it->second.handle);
        creatureLoops_.erase(it);
    }

    const uint32_t handle = SoundEntryTable::instance().play3DStoppable(
        assetManager, loopSoundId, position, volumeScale,
        1.0f, SoundEntryTable::kAutoCategory, SoundLoopMode::ForceLoop);
    if (handle != 0) {
        creatureLoops_[guid] = CreatureLoopState{loopSoundId, handle};
    }
}

void FootstepManager::stopCreatureLoopSound(uint64_t guid) {
    const auto it = creatureLoops_.find(guid);
    if (it == creatureLoops_.end()) return;
    if (it->second.handle != 0 && AudioEngine::instance().isInitialized()) {
        AudioEngine::instance().stopSound(it->second.handle);
    }
    creatureLoops_.erase(it);
}

void FootstepManager::clearAllCreatureLoops() {
    if (AudioEngine::instance().isInitialized()) {
        for (const auto& [_, st] : creatureLoops_) {
            if (st.handle != 0) {
                AudioEngine::instance().stopSound(st.handle);
            }
        }
    }
    creatureLoops_.clear();
}

bool FootstepManager::hasActiveCreatureLoop(uint64_t guid) const {
    const auto it = creatureLoops_.find(guid);
    return it != creatureLoops_.end() && it->second.handle != 0;
}

} // namespace audio
} // namespace wowee
