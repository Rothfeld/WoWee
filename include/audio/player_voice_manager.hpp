#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <glm/glm.hpp>

namespace wowee {
namespace pipeline { class AssetManager; }

namespace audio {

// Player character error speech lines ("I can't do that", "Not enough mana", ...)
// Mapped directly to TBC 2.4.3 VocalUISounds.dbc VocalUIEnum (0..67).
enum class PlayerErrorSpeech : uint8_t {
    NO_MANA,
    NO_RAGE,
    NO_ENERGY,
    OUT_OF_RANGE,
    GENERIC_NO_TARGET,
    INVALID_ATTACK_TARGET,
    SPELL_COOLDOWN,
    ABILITY_COOLDOWN,
    ITEM_COOLDOWN,
    POTION_COOLDOWN,
    NO_AMMO,
    AMMO_ONLY,
    CHEST_IN_USE,
    INVENTORY_FULL,
    BAG_FULL,
    ITEM_MAX_COUNT,
    NOT_A_BAG,
    CANT_USE_ITEM,
    ITEM_LOCKED,
    MUST_EQUIP_ITEM,
    NOT_EQUIPPABLE,
    CANT_EQUIP_LEVEL,
    CANT_EQUIP_SKILL,
    CANT_EQUIP_EVER,
    CANT_DROP_SOULBOUND,
    CANT_TRADE_SOULBOUND,
    NOT_ENOUGH_MONEY,
    CANT_LEARN_SPELL,
    LOOT_TOO_FAR,
    LOOT_DIDNT_KILL,
    CANT_LOOT,
    ALREADY_IN_GROUP,
    PARTY_FULL,
    GUILD_PERMISSIONS,
    CANT_CREATE_HERE,
    FOOD_COOLDOWN,
    BANKSLOT_INSUFFICIENT_FUNDS,
    BAG_IN_BAG,
    WRONG_SLOT,
    LOOT_BAD_FACING,
    LOOT_LOCKED,
    NO_ATTACK_TARGET,
    TWO_HANDED_EQUIPPED,
    TWO_HAND_SKILL_NOT_FOUND,
    SPELL_OUT_OF_RANGE,
    PROFICIENCY_NEEDED,
    TAXINOTENOUGHMONEY,
    USE_TOO_FAR,
};

/**
 * Plays player vocal UI error lines from VocalUISounds.dbc (641 rows in TBC 2.4.3),
 * text-emote voices from EmotesTextSound.dbc (522 rows), and animation-keyframe
 * emote sounds ($ESD) from Emotes.dbc col 6.
 *
 * Error speech playback mirrors Wow.exe PlayVocalUISound (0x7b8100): a new error
 * voice line is gated by whether the previous error speech handle is still playing
 * (SESound::IsPlaying(&s_hErrorSpeechSound)), plus the Character Speech toggle.
 */
class PlayerVoiceManager {
public:
    PlayerVoiceManager() = default;
    ~PlayerVoiceManager() = default;

    bool initialize(pipeline::AssetManager* assets);
    void shutdown();

    void setEnabled(bool enabled) { enabled_ = enabled; }
    [[nodiscard]] bool isEnabled() const { return enabled_; }

    void setVolumeScale(float scale);
    [[nodiscard]] float getVolumeScale() const { return volumeScale_; }

    /// Map a PlayerErrorSpeech enum value to its VocalUISounds.dbc VocalUIEnum (0..67).
    [[nodiscard]] static uint32_t vocalUIEnumForErrorSpeech(PlayerErrorSpeech type);

    /// Map any of the 58 vocal GameError names from Wow.exe (0xb98770) to VocalUIEnum (0..67),
    /// or std::nullopt when the GameError has VocalUIEnum == 68 (no vocal line).
    [[nodiscard]] static std::optional<uint32_t> vocalUIEnumForGameError(std::string_view gameErrorName);

    /// Total rows loaded from VocalUISounds.dbc (641 in TBC 2.4.3).
    [[nodiscard]] size_t vocalUIRowCount();

    /// Resolve the SoundEntries.dbc ID for (vocalUiEnum, raceId, gender, pissed).
    [[nodiscard]] uint32_t getVocalUISoundId(uint32_t vocalUiEnum, uint8_t raceId,
                                             uint8_t gender, bool pissed = false);

    /// True while the most recently started error speech clip is still actively playing.
    [[nodiscard]] bool isErrorSpeechPlaying() const;

    /// Immediately stop any active error speech handle.
    void stopErrorSpeech();

    /// Play a VocalUISounds.dbc entry (SoundType = 7 / ErrorSpeech) gated by isErrorSpeechPlaying().
    bool playVocalUI(uint32_t vocalUiEnum, uint8_t raceId, uint8_t gender, bool pissed = false);

    /// Play a vocal GameError by GlobalStrings identifier (e.g. "ERR_2HSKILLNOTFOUND").
    bool playGameError(std::string_view gameErrorName, uint8_t raceId, uint8_t gender, bool pissed = false);

    /// Play a spoken error response for the player's race/gender.
    /// raceId: game Race enum value (1=Human .. 11=Draenei); gender: 0=male, 1=female
    void playError(PlayerErrorSpeech type, uint8_t raceId, uint8_t gender);

    /// Total rows loaded from EmotesTextSound.dbc (522 in TBC 2.4.3).
    [[nodiscard]] size_t emoteTextSoundCount();

    /// Resolve EmotesTextSound.dbc SoundEntries ID for (emotesTextId, raceId, gender).
    [[nodiscard]] uint32_t getEmoteTextSoundId(uint32_t emotesTextId, uint8_t raceId, uint8_t gender);

    /// Play a vocal emote from EmotesTextSound.dbc (2D local or 3D remote).
    bool playEmote(uint32_t emotesTextId, uint8_t raceId, uint8_t gender);
    bool playEmote3D(uint32_t emotesTextId, uint8_t raceId, uint8_t gender, const glm::vec3& position);

    /// Deduplication helper so local DoEmote() / ChatPanel /emote does not double-play
    /// when the server echoes SMSG_TEXT_EMOTE for the local player moments later.
    [[nodiscard]] bool wasLocalEmoteRecentlyPlayed(uint32_t emotesTextId, float windowSeconds = 0.5f) const;

    /// Resolve Emotes.dbc col 6 (EmoteSoundID) for M2 $ESD animation events.
    [[nodiscard]] uint32_t getEmoteSoundId(uint32_t emoteId);
    [[nodiscard]] uint32_t getEmoteSoundIdForAnim(uint32_t animId);
    bool playEmoteAnimSound(uint32_t emoteId);
    bool playEmoteAnimSound3D(uint32_t emoteId, const glm::vec3& position);
    bool playEmoteAnimSoundForEvent(uint32_t eventData, uint32_t npcEmoteState,
                                    uint32_t animId, bool isLocalPlayer,
                                    const glm::vec3& position);

private:
    struct VocalUIRecord {
        uint32_t normalMaleSoundId = 0;
        uint32_t normalFemaleSoundId = 0;
        uint32_t pissedMaleSoundId = 0;
        uint32_t pissedFemaleSoundId = 0;
    };

    void ensureVocalUISoundsLoaded();
    void ensureEmoteSoundsLoaded();
    void ensureEmoteAnimSoundsLoaded();

    pipeline::AssetManager* assetManager_ = nullptr;
    bool enabled_ = true;
    bool vocalUISoundsLoaded_ = false;
    bool emoteSoundsLoaded_ = false;
    bool emoteAnimSoundsLoaded_ = false;
    float volumeScale_ = 1.0f;

    size_t vocalUIRowCount_ = 0;
    // Key: (uint32_t(raceId) << 16) | (vocalUiEnum & 0xFFFF)
    std::unordered_map<uint32_t, VocalUIRecord> vocalUISounds_;

    // EmotesTextSound.dbc: (emotesTextId << 16 | raceId << 8 | sexId) -> SoundEntries ID
    std::unordered_map<uint32_t, uint32_t> emoteSounds_;

    // Emotes.dbc col 6: emoteId -> EmoteSoundID, and col 2 AnimID -> EmoteSoundID for $ESD
    std::unordered_map<uint32_t, uint32_t> emoteAnimSounds_;
    std::unordered_map<uint32_t, uint32_t> emoteAnimSoundByAnimId_;

    // Active error speech handle tracking (mirrors Wow.exe s_hErrorSpeechSound at 0xe2a710)
    mutable uint32_t errorSpeechHandle_ = 0;
    std::chrono::steady_clock::time_point errorSpeechStartTime_{};
    double errorSpeechDurationSec_ = 0.0;
    size_t errorSpeechActiveCountAtStart_ = 0;

    // Last local emote played via playEmote(), used to suppress duplicate server SMSG_TEXT_EMOTE echo
    uint32_t lastLocalEmoteId_ = 0;
    std::chrono::steady_clock::time_point lastLocalEmoteTime_{};
};

} // namespace audio
} // namespace wowee
