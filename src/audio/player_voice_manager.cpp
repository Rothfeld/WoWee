#include "audio/player_voice_manager.hpp"
#include "audio/audio_engine.hpp"
#include "audio/sound_entries.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "core/logger.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace wowee {
namespace audio {

namespace {

uint32_t sanitizeDbcSoundId(uint32_t id) {
    return (id == 0xFFFFFFFFu) ? 0u : id;
}

uint32_t readLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8u) |
           (static_cast<uint32_t>(p[2]) << 16u) |
           (static_cast<uint32_t>(p[3]) << 24u);
}

// Extract exact PCM duration from RIFF/WAVE header bytes so live-device mode
// knows the precise clip duration without touching AudioEngine internals.
double parseWavDurationSeconds(const std::vector<uint8_t>& bytes) {
    if (bytes.size() < 44) return 1.0;
    if (std::memcmp(bytes.data(), "RIFF", 4) != 0 ||
        std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
        return 1.0;
    }
    uint32_t byteRate = 0;
    uint32_t dataSize = 0;
    size_t pos = 12;
    while (pos + 8 <= bytes.size()) {
        const uint32_t chunkSize = readLe32(bytes.data() + pos + 4);
        const size_t payloadOffset = pos + 8;
        if (payloadOffset > bytes.size()) break;
        const size_t avail = bytes.size() - payloadOffset;
        const size_t safeSize = std::min<size_t>(chunkSize, avail);
        if (std::memcmp(bytes.data() + pos, "fmt ", 4) == 0 && safeSize >= 12) {
            byteRate = readLe32(bytes.data() + payloadOffset + 8);
        } else if (std::memcmp(bytes.data() + pos, "data", 4) == 0) {
            dataSize = static_cast<uint32_t>(safeSize);
            break;
        }
        const size_t step = static_cast<size_t>(chunkSize) + (chunkSize & 1u);
        if (step > avail) break;
        pos = payloadOffset + step;
    }
    if (byteRate == 0 || dataSize == 0) return 1.0;
    return static_cast<double>(dataSize) / static_cast<double>(byteRate);
}

struct GameErrorVocalEntry {
    std::string_view name;
    uint32_t vocalUiEnum;
};

// Complete table of all 58 vocal GameError entries from TBC 2.4.3 Wow.exe (0xb98770),
// spanning all 46 triggered VocalUIEnum categories (0..64).
constexpr std::array<GameErrorVocalEntry, 58> kVocalGameErrors = {{
    {"ERR_INV_FULL",                    0},
    {"ERR_NOAMMO_S",                    1},
    {"ERR_CANT_EQUIP_LEVEL_I",          2},
    {"ERR_CANT_EQUIP_EVER",             3},
    {"ERR_DROP_BOUND_ITEM",             4},
    {"ERR_ITEM_COOLDOWN",               5},
    {"ERR_FOOD_COOLDOWN",               7},
    {"ERR_GROUP_FULL",                  8},
    {"ERR_OUT_OF_RANGE",               10},
    {"ERR_INVALID_ATTACK_TARGET",      11},
    {"ERR_SPELL_COOLDOWN",             12},
    {"ERR_SPELL_ALREADY_KNOWN_S",      13},
    {"ERR_OUT_OF_MANA",                15},
    {"ERR_ALREADY_IN_GROUP_S",         20},
    {"ERR_BANKSLOT_INSUFFICIENT_FUNDS",22},
    {"ERR_NOT_A_BAG",                  25},
    {"ERR_BAG_IN_BAG",                 26},
    {"ERR_WRONG_SLOT",                 27},
    {"ERR_AMMO_ONLY",                  28},
    {"ERR_WRONG_BAG_TYPE_SUBCLASS",    28},
    {"ERR_BAG_FULL",                   29},
    {"ERR_ITEM_MAX_COUNT",             30},
    {"ERR_LOOT_DIDNT_KILL",            31},
    {"ERR_LOOT_BAD_FACING",            32},
    {"ERR_LOOT_LOCKED",                33},
    {"ERR_LOOT_TOO_FAR",               35},
    {"ERR_NO_ATTACK_TARGET",           38},
    {"ERR_NOT_ENOUGH_MONEY",           40},
    {"ERR_GUILDEMBLEM_NOTENOUGHMONEY", 40},
    {"ERR_CANT_EQUIP_SKILL",           41},
    {"ERR_2HANDED_EQUIPPED",           42},
    {"ERR_2HSKILLNOTFOUND",            43},
    {"ERR_NOT_EQUIPPABLE",             44},
    {"ERR_GENERIC_NO_TARGET",          45},
    {"ERR_SPELL_OUT_OF_RANGE",         46},
    {"ERR_POTION_COOLDOWN",            47},
    {"ERR_PROFICIENCY_NEEDED",         48},
    {"ERR_MUST_EQUIP_ITEM",            49},
    {"ERR_ABILITY_COOLDOWN",           50},
    {"ERR_CANT_USE_ITEM",              51},
    {"ERR_CANT_USE_ITEM_IN_ARENA",     51},
    {"ERR_INVALID_ITEM_TARGET",        51},
    {"ERR_CHEST_IN_USE",               52},
    {"ERR_TAXINOTENOUGHMONEY",         54},
    {"ERR_USE_TOO_FAR",                57},
    {"ERR_TRADE_BOUND_ITEM",           59},
    {"ERR_TRADE_QUEST_ITEM",           59},
    {"ERR_MAIL_QUEST_ITEM",            59},
    {"ERR_MAIL_BOUND_ITEM",            59},
    {"ERR_AUCTION_QUEST_ITEM",         59},
    {"ERR_AUCTION_BOUND_ITEM",         59},
    {"ERR_ITEM_LOCKED",                61},
    {"ERR_USE_LOCKED",                 61},
    {"ERR_DOOR_LOCKED",                61},
    {"ERR_BUTTON_LOCKED",              61},
    {"ERR_GUILD_PERMISSIONS",          62},
    {"ERR_OUT_OF_RAGE",                63},
    {"ERR_OUT_OF_ENERGY",              64},
}};

} // namespace

bool PlayerVoiceManager::initialize(pipeline::AssetManager* assets) {
    shutdown();
    assetManager_ = assets;
    if (!assetManager_) return false;
    ensureVocalUISoundsLoaded();
    ensureEmoteSoundsLoaded();
    ensureEmoteAnimSoundsLoaded();
    return true;
}

void PlayerVoiceManager::shutdown() {
    stopErrorSpeech();
    vocalUISounds_.clear();
    emoteSounds_.clear();
    emoteAnimSounds_.clear();
    vocalUIRowCount_ = 0;
    vocalUISoundsLoaded_ = false;
    emoteSoundsLoaded_ = false;
    emoteAnimSoundsLoaded_ = false;
    lastLocalEmoteId_ = 0;
    lastLocalEmoteTime_ = {};
    assetManager_ = nullptr;
}

void PlayerVoiceManager::setVolumeScale(float scale) {
    volumeScale_ = std::clamp(scale, 0.0f, 1.0f);
}

uint32_t PlayerVoiceManager::vocalUIEnumForErrorSpeech(PlayerErrorSpeech type) {
    switch (type) {
        case PlayerErrorSpeech::INVENTORY_FULL:              return 0;
        case PlayerErrorSpeech::NO_AMMO:                     return 1;
        case PlayerErrorSpeech::CANT_EQUIP_LEVEL:            return 2;
        case PlayerErrorSpeech::CANT_EQUIP_EVER:             return 3;
        case PlayerErrorSpeech::CANT_DROP_SOULBOUND:         return 4;
        case PlayerErrorSpeech::ITEM_COOLDOWN:               return 5;
        case PlayerErrorSpeech::FOOD_COOLDOWN:               return 7;
        case PlayerErrorSpeech::PARTY_FULL:                  return 8;
        case PlayerErrorSpeech::OUT_OF_RANGE:                return 10;
        case PlayerErrorSpeech::INVALID_ATTACK_TARGET:       return 11;
        case PlayerErrorSpeech::SPELL_COOLDOWN:              return 12;
        case PlayerErrorSpeech::CANT_LEARN_SPELL:            return 13;
        case PlayerErrorSpeech::NO_MANA:                     return 15;
        case PlayerErrorSpeech::CANT_LOOT:                   return 17;
        case PlayerErrorSpeech::CANT_CREATE_HERE:            return 18;
        case PlayerErrorSpeech::ALREADY_IN_GROUP:            return 20;
        case PlayerErrorSpeech::BANKSLOT_INSUFFICIENT_FUNDS: return 22;
        case PlayerErrorSpeech::NOT_A_BAG:                   return 25;
        case PlayerErrorSpeech::BAG_IN_BAG:                  return 26;
        case PlayerErrorSpeech::WRONG_SLOT:                  return 27;
        case PlayerErrorSpeech::AMMO_ONLY:                   return 28;
        case PlayerErrorSpeech::BAG_FULL:                    return 29;
        case PlayerErrorSpeech::ITEM_MAX_COUNT:              return 30;
        case PlayerErrorSpeech::LOOT_DIDNT_KILL:             return 31;
        case PlayerErrorSpeech::LOOT_BAD_FACING:             return 32;
        case PlayerErrorSpeech::LOOT_LOCKED:                 return 33;
        case PlayerErrorSpeech::LOOT_TOO_FAR:                return 35;
        case PlayerErrorSpeech::NO_ATTACK_TARGET:            return 38;
        case PlayerErrorSpeech::NOT_ENOUGH_MONEY:            return 40;
        case PlayerErrorSpeech::CANT_EQUIP_SKILL:            return 41;
        case PlayerErrorSpeech::TWO_HANDED_EQUIPPED:         return 42;
        case PlayerErrorSpeech::TWO_HAND_SKILL_NOT_FOUND:    return 43;
        case PlayerErrorSpeech::NOT_EQUIPPABLE:              return 44;
        case PlayerErrorSpeech::GENERIC_NO_TARGET:           return 45;
        case PlayerErrorSpeech::SPELL_OUT_OF_RANGE:          return 46;
        case PlayerErrorSpeech::POTION_COOLDOWN:             return 47;
        case PlayerErrorSpeech::PROFICIENCY_NEEDED:          return 48;
        case PlayerErrorSpeech::MUST_EQUIP_ITEM:             return 49;
        case PlayerErrorSpeech::ABILITY_COOLDOWN:            return 50;
        case PlayerErrorSpeech::CANT_USE_ITEM:               return 51;
        case PlayerErrorSpeech::CHEST_IN_USE:                return 52;
        case PlayerErrorSpeech::TAXINOTENOUGHMONEY:          return 54;
        case PlayerErrorSpeech::USE_TOO_FAR:                 return 57;
        case PlayerErrorSpeech::CANT_TRADE_SOULBOUND:        return 59;
        case PlayerErrorSpeech::ITEM_LOCKED:                 return 61;
        case PlayerErrorSpeech::GUILD_PERMISSIONS:           return 62;
        case PlayerErrorSpeech::NO_RAGE:                     return 63;
        case PlayerErrorSpeech::NO_ENERGY:                   return 64;
    }
    return 68;
}

std::optional<uint32_t> PlayerVoiceManager::vocalUIEnumForGameError(std::string_view gameErrorName) {
    for (const auto& entry : kVocalGameErrors) {
        if (entry.name == gameErrorName) {
            return entry.vocalUiEnum;
        }
    }
    return std::nullopt;
}

void PlayerVoiceManager::ensureVocalUISoundsLoaded() {
    if (vocalUISoundsLoaded_ || !assetManager_ || !assetManager_->isInitialized()) return;
    auto dbc = assetManager_->loadDBC("VocalUISounds.dbc");
    if (!dbc || !dbc->isLoaded() || dbc->getFieldCount() < 7) return;

    vocalUISoundsLoaded_ = true;
    vocalUIRowCount_ = dbc->getRecordCount();
    vocalUISounds_.reserve(vocalUIRowCount_);

    for (uint32_t r = 0; r < dbc->getRecordCount(); ++r) {
        const uint32_t vocalUiEnum = dbc->getUInt32(r, 1);
        const uint32_t raceId      = dbc->getUInt32(r, 2);
        if (raceId == 0 || vocalUiEnum >= 68) continue;

        VocalUIRecord rec;
        rec.normalMaleSoundId   = sanitizeDbcSoundId(dbc->getUInt32(r, 3));
        rec.normalFemaleSoundId = sanitizeDbcSoundId(dbc->getUInt32(r, 4));
        rec.pissedMaleSoundId   = sanitizeDbcSoundId(dbc->getUInt32(r, 5));
        rec.pissedFemaleSoundId = sanitizeDbcSoundId(dbc->getUInt32(r, 6));

        const uint32_t key = ((raceId & 0xFFFFu) << 16u) | (vocalUiEnum & 0xFFFFu);
        vocalUISounds_[key] = rec;
    }

    LOG_INFO("PlayerVoiceManager: loaded ", vocalUIRowCount_, " VocalUISounds.dbc rows (",
             vocalUISounds_.size(), " keyed entries)");
}

size_t PlayerVoiceManager::vocalUIRowCount() {
    ensureVocalUISoundsLoaded();
    return vocalUIRowCount_;
}

uint32_t PlayerVoiceManager::getVocalUISoundId(uint32_t vocalUiEnum, uint8_t raceId,
                                               uint8_t gender, bool pissed) {
    if (raceId == 0 || vocalUiEnum >= 68) return 0;
    ensureVocalUISoundsLoaded();
    const uint32_t key = (static_cast<uint32_t>(raceId) << 16u) | (vocalUiEnum & 0xFFFFu);
    const auto it = vocalUISounds_.find(key);
    if (it == vocalUISounds_.end()) return 0;

    const bool female = (gender == 1);
    if (pissed) {
        const uint32_t pissedId = female ? it->second.pissedFemaleSoundId
                                         : it->second.pissedMaleSoundId;
        if (pissedId != 0) return pissedId;
    }
    return female ? it->second.normalFemaleSoundId : it->second.normalMaleSoundId;
}

bool PlayerVoiceManager::isErrorSpeechPlaying() const {
    if (errorSpeechHandle_ == 0) return false;
    auto& engine = AudioEngine::instance();
    if (!engine.isInitialized()) {
        errorSpeechHandle_ = 0;
        return false;
    }

    const size_t activeCount = engine.getActiveSoundCount();
    if (activeCount == 0) {
        errorSpeechHandle_ = 0;
        return false;
    }

    const bool headless = (engine.getOutputDeviceName() == "Headless Test Mixer");
    if (headless) {
        if (activeCount < errorSpeechActiveCountAtStart_) {
            errorSpeechHandle_ = 0;
            return false;
        }
        return true;
    }

    if (errorSpeechDurationSec_ > 0.0 && errorSpeechStartTime_.time_since_epoch().count() != 0) {
        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - errorSpeechStartTime_).count();
        if (elapsed >= errorSpeechDurationSec_) {
            engine.stopSound(errorSpeechHandle_);
            errorSpeechHandle_ = 0;
            return false;
        }
    }
    return true;
}

void PlayerVoiceManager::stopErrorSpeech() {
    if (errorSpeechHandle_ != 0) {
        if (AudioEngine::instance().isInitialized()) {
            AudioEngine::instance().stopSound(errorSpeechHandle_);
        }
        errorSpeechHandle_ = 0;
    }
    errorSpeechStartTime_ = {};
    errorSpeechDurationSec_ = 0.0;
    errorSpeechActiveCountAtStart_ = 0;
}

bool PlayerVoiceManager::playVocalUI(uint32_t vocalUiEnum, uint8_t raceId,
                                     uint8_t gender, bool pissed) {
    if (!enabled_ || !assetManager_ || volumeScale_ <= 0.0f) return false;
    auto& engine = AudioEngine::instance();
    if (!engine.isInitialized()) return false;

    // Wow.exe PlayVocalUISound (0x7b8100): block new error speech only while
    // s_hErrorSpeechSound is actively playing.
    if (isErrorSpeechPlaying()) return false;

    const uint32_t soundEntryId = getVocalUISoundId(vocalUiEnum, raceId, gender, pissed);
    if (soundEntryId == 0) return false;

    auto& table = SoundEntryTable::instance();
    const SoundEntryRecord* rec = table.findById(assetManager_, soundEntryId);
    if (!rec) return false;

    const std::string samplePath = table.pickExistingPath(assetManager_, *rec);
    if (samplePath.empty()) return false;

    double clipDurationSec = 1.0;
    if (auto bytes = assetManager_->readFile(samplePath); !bytes.empty()) {
        clipDurationSec = parseWavDurationSeconds(bytes);
    }

    const uint32_t handle = table.play2DStoppable(assetManager_, soundEntryId, volumeScale_);
    if (handle == 0) return false;

    errorSpeechHandle_ = handle;
    errorSpeechStartTime_ = std::chrono::steady_clock::now();
    errorSpeechDurationSec_ = clipDurationSec;
    errorSpeechActiveCountAtStart_ = std::max<size_t>(1u, engine.getActiveSoundCount());
    return true;
}

bool PlayerVoiceManager::playGameError(std::string_view gameErrorName, uint8_t raceId,
                                       uint8_t gender, bool pissed) {
    const auto vocalUiEnum = vocalUIEnumForGameError(gameErrorName);
    if (!vocalUiEnum) return false;
    return playVocalUI(*vocalUiEnum, raceId, gender, pissed);
}

void PlayerVoiceManager::playError(PlayerErrorSpeech type, uint8_t raceId, uint8_t gender) {
    const uint32_t vocalUiEnum = vocalUIEnumForErrorSpeech(type);
    if (vocalUiEnum >= 68) return;
    (void)playVocalUI(vocalUiEnum, raceId, gender, false);
}

void PlayerVoiceManager::ensureEmoteSoundsLoaded() {
    if (emoteSoundsLoaded_ || !assetManager_ || !assetManager_->isInitialized()) return;
    auto dbc = assetManager_->loadDBC("EmotesTextSound.dbc");
    if (!dbc || !dbc->isLoaded() || dbc->getFieldCount() < 5) return;
    emoteSoundsLoaded_ = true;
    emoteSounds_.reserve(dbc->getRecordCount());
    for (uint32_t r = 0; r < dbc->getRecordCount(); ++r) {
        const uint32_t textEmoteId = dbc->getUInt32(r, 1);
        const uint32_t raceId = dbc->getUInt32(r, 2);
        const uint32_t sexId = dbc->getUInt32(r, 3);
        const uint32_t soundId = sanitizeDbcSoundId(dbc->getUInt32(r, 4));
        if (textEmoteId == 0 || soundId == 0) continue;
        const uint32_t key = (textEmoteId << 16u) | ((raceId & 0xFFu) << 8u) | (sexId & 0xFFu);
        emoteSounds_[key] = soundId;
    }
}

size_t PlayerVoiceManager::emoteTextSoundCount() {
    ensureEmoteSoundsLoaded();
    return emoteSounds_.size();
}

uint32_t PlayerVoiceManager::getEmoteTextSoundId(uint32_t emotesTextId, uint8_t raceId, uint8_t gender) {
    if (emotesTextId == 0 || raceId == 0) return 0;
    ensureEmoteSoundsLoaded();
    const uint32_t key = (emotesTextId << 16u) | (static_cast<uint32_t>(raceId) << 8u) | (gender ? 1u : 0u);
    const auto it = emoteSounds_.find(key);
    return (it != emoteSounds_.end()) ? it->second : 0u;
}

bool PlayerVoiceManager::playEmote(uint32_t emotesTextId, uint8_t raceId, uint8_t gender) {
    if (!assetManager_ || emotesTextId == 0 || raceId == 0 || volumeScale_ <= 0.0f) return false;
    const uint32_t soundId = getEmoteTextSoundId(emotesTextId, raceId, gender);
    if (soundId == 0) return false;
    if (!SoundEntryTable::instance().play2D(
            assetManager_, soundId, volumeScale_ * SoundEntryTable::kLocalPlayer2DGain)) {
        return false;
    }
    lastLocalEmoteId_ = emotesTextId;
    lastLocalEmoteTime_ = std::chrono::steady_clock::now();
    return true;
}

bool PlayerVoiceManager::playLocalEmote(uint32_t emotesTextId, uint8_t raceId, uint8_t gender,
                                        const glm::vec3& unitBasePos, bool listenerAtCharacter) {
    if (!assetManager_ || emotesTextId == 0 || raceId == 0 || volumeScale_ <= 0.0f) return false;
    const uint32_t soundId = getEmoteTextSoundId(emotesTextId, raceId, gender);
    if (soundId == 0) return false;
    if (!SoundEntryTable::instance().playUnit(
            assetManager_, soundId, unitBasePos, /*isLocalPlayer=*/true,
            listenerAtCharacter, volumeScale_)) {
        return false;
    }
    lastLocalEmoteId_ = emotesTextId;
    lastLocalEmoteTime_ = std::chrono::steady_clock::now();
    return true;
}

bool PlayerVoiceManager::playEmote3D(uint32_t emotesTextId, uint8_t raceId, uint8_t gender,
                                     const glm::vec3& position) {
    if (!assetManager_ || emotesTextId == 0 || raceId == 0 || volumeScale_ <= 0.0f) return false;
    const uint32_t soundId = getEmoteTextSoundId(emotesTextId, raceId, gender);
    if (soundId == 0) return false;
    return SoundEntryTable::instance().playUnit(assetManager_, soundId, position,
                                                /*isLocalPlayer=*/false,
                                                /*listenerAtCharacter=*/false,
                                                volumeScale_);
}

bool PlayerVoiceManager::wasLocalEmoteRecentlyPlayed(uint32_t emotesTextId, float windowSeconds) const {
    if (emotesTextId == 0 || lastLocalEmoteId_ != emotesTextId) return false;
    if (lastLocalEmoteTime_.time_since_epoch().count() == 0) return false;
    const float elapsed =
        std::chrono::duration<float>(std::chrono::steady_clock::now() - lastLocalEmoteTime_).count();
    return elapsed >= 0.0f && elapsed < windowSeconds;
}

void PlayerVoiceManager::ensureEmoteAnimSoundsLoaded() {
    if (emoteAnimSoundsLoaded_ || !assetManager_ || !assetManager_->isInitialized()) return;
    auto dbc = assetManager_->loadDBC("Emotes.dbc");
    if (!dbc || !dbc->isLoaded() || dbc->getFieldCount() < 7) return;
    emoteAnimSoundsLoaded_ = true;
    for (uint32_t r = 0; r < dbc->getRecordCount(); ++r) {
        const uint32_t emoteId = dbc->getUInt32(r, 0);
        const uint32_t animId = dbc->getUInt32(r, 2);
        const uint32_t soundId = sanitizeDbcSoundId(dbc->getUInt32(r, 6));
        if (emoteId != 0 && soundId != 0) {
            emoteAnimSounds_[emoteId] = soundId;
            if (animId != 0) {
                emoteAnimSoundByAnimId_.emplace(animId, soundId);
            }
        }
    }
}

uint32_t PlayerVoiceManager::getEmoteSoundId(uint32_t emoteId) {
    if (emoteId == 0) return 0;
    ensureEmoteAnimSoundsLoaded();
    const auto it = emoteAnimSounds_.find(emoteId);
    return (it != emoteAnimSounds_.end()) ? it->second : 0u;
}

uint32_t PlayerVoiceManager::getEmoteSoundIdForAnim(uint32_t animId) {
    if (animId == 0) return 0;
    ensureEmoteAnimSoundsLoaded();
    const auto it = emoteAnimSoundByAnimId_.find(animId);
    return (it != emoteAnimSoundByAnimId_.end()) ? it->second : 0u;
}

bool PlayerVoiceManager::playEmoteAnimSound(uint32_t emoteId, bool listenerAtCharacter,
                                            const glm::vec3& unitBasePos) {
    if (!assetManager_ || volumeScale_ <= 0.0f) return false;
    const uint32_t soundId = getEmoteSoundId(emoteId);
    if (soundId == 0) return false;
    return SoundEntryTable::instance().playUnit(assetManager_, soundId, unitBasePos,
                                                /*isLocalPlayer=*/true, listenerAtCharacter,
                                                volumeScale_);
}

bool PlayerVoiceManager::playEmoteAnimSound3D(uint32_t emoteId, const glm::vec3& position) {
    if (!assetManager_ || volumeScale_ <= 0.0f) return false;
    const uint32_t soundId = getEmoteSoundId(emoteId);
    if (soundId == 0) return false;
    return SoundEntryTable::instance().playUnit(assetManager_, soundId, position,
                                                /*isLocalPlayer=*/false,
                                                /*listenerAtCharacter=*/false,
                                                volumeScale_);
}

bool PlayerVoiceManager::playEmoteAnimSoundForEvent(
        uint32_t eventData, uint32_t npcEmoteState, uint32_t animId,
        bool isLocalPlayer, const glm::vec3& position) {
    if (!assetManager_ || volumeScale_ <= 0.0f) return false;
    uint32_t soundId = getEmoteSoundId(eventData);
    if (soundId == 0u) soundId = getEmoteSoundId(npcEmoteState);
    if (soundId == 0u) soundId = getEmoteSoundIdForAnim(animId);
    if (soundId == 0u) return false;
    if (isLocalPlayer) {
        return SoundEntryTable::instance().play2D(
            assetManager_, soundId, volumeScale_ * SoundEntryTable::kLocalPlayer2DGain);
    }
    return SoundEntryTable::instance().play3D(assetManager_, soundId, position, volumeScale_);
}

} // namespace audio
} // namespace wowee
