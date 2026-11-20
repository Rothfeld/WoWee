#pragma once

#include <unordered_map>
#include <vector>
#include <memory>
#include <string>
#include <cstdint>

namespace wowee {
namespace pipeline {
class AssetManager;
}

namespace audio {

class UiSoundManager {
public:
    UiSoundManager() = default;
    ~UiSoundManager() = default;

    // Initialization
    bool initialize(pipeline::AssetManager* assets);
    void shutdown();
    [[nodiscard]] bool isInitialized() const { return initialized_; }

    /// Play a sound by SoundEntries.dbc numeric ID (used by PlaySound(id) and ItemGroupSounds).
    bool playById(uint32_t soundId);

    /// Play an item pickup or putdown sound via ItemGroupSounds.dbc (id = ItemDisplayInfo.GroupSoundIndex).
    bool playItemGroupSound(uint32_t groupSoundIndex, bool pickup);

    /// Play an item pickup or putdown sound resolved from ItemDisplayInfo.dbc col 12 -> ItemGroupSounds.dbc.
    bool playItemDisplaySound(uint32_t displayInfoId, bool pickup);

    /// Play a sound by the name FrameXML asks for - PlaySound("igQuestFailed")
    /// and its sixty-seven siblings.
    ///
    /// Resolved through SoundEntries.dbc, which is where those names come
    /// from: the interface is naming a row, and the row names the files. That
    /// is the real sound rather than the nearest one this client had already
    /// loaded, and it covers every name rather than the forty-four somebody
    /// mapped by hand.
    ///
    /// False when the name is not in the table or its file will not load, so
    /// the caller can fall back rather than going silent.
    bool playByName(const std::string& soundName);

    /// Play a sound by path, which is what PlaySoundFile takes. Cached the
    /// same way and by the same store: a path and a name are both just keys,
    /// and nothing can be both.
    bool playFile(const std::string& path);

    // Volume control
    void setVolumeScale(float scale);
    [[nodiscard]] float getVolumeScale() const { return volumeScale_; }

    // Window sounds
    void playBagOpen();
    void playBagClose();
    void playQuestLogOpen();
    void playQuestLogClose();
    void playCharacterSheetOpen();
    void playCharacterSheetClose();
    void playGuildBankOpen();
    void playGuildBankClose();
    void playAuctionHouseOpen();
    void playAuctionHouseClose();

    // Button sounds
    void playButtonClick();
    void playMenuButtonClick();

    // Quest sounds
    void playQuestActivate();
    void playQuestComplete();
    void playQuestFailed();
    void playQuestUpdate();
    void playFishingBite();

    // Loot sounds
    void playLootCoinSmall();
    void playLootCoinLarge();
    void playLootItem();

    // Item sounds
    void playDropOnGround();
    void playPickupBag();
    /// Picking an item up sounds like the thing it is - cloth, food or a gem.
    /// Chosen by item class; anything else keeps the bag rustle.
    void playPickupBook();

    // Eating/drinking
    void playEating();
    void playDrinking();

    // Level up
    void playLevelUp();

    // Achievement
    void playAchievementAlert();

    // Error/feedback
    void playError();
    void playTargetSelect();
    void playTargetDeselect();

    // Chat notifications
    void playWhisperReceived();
    void playMailReceived();

    // Minimap ping
    void playMinimapPing();

private:
    struct UISample {
        std::string path;
        std::vector<uint8_t> data;
        bool loaded;
    };

    // Sound libraries
    std::vector<UISample> bagOpenSounds_;
    std::vector<UISample> bagCloseSounds_;
    std::vector<UISample> questLogOpenSounds_;
    std::vector<UISample> questLogCloseSounds_;
    std::vector<UISample> characterSheetOpenSounds_;
    std::vector<UISample> characterSheetCloseSounds_;
    std::vector<UISample> auctionOpenSounds_;
    std::vector<UISample> auctionCloseSounds_;
    std::vector<UISample> guildBankOpenSounds_;
    std::vector<UISample> guildBankCloseSounds_;

    std::vector<UISample> buttonClickSounds_;
    std::vector<UISample> menuButtonSounds_;

    std::vector<UISample> questActivateSounds_;
    std::vector<UISample> questCompleteSounds_;
    std::vector<UISample> questFailedSounds_;
    std::vector<UISample> questUpdateSounds_;
    std::vector<UISample> fishingBiteSounds_;

    std::vector<UISample> lootCoinSmallSounds_;
    std::vector<UISample> lootCoinLargeSounds_;
    std::vector<UISample> lootItemSounds_;

    std::vector<UISample> dropSounds_;
    std::vector<UISample> pickupBagSounds_;
    std::vector<UISample> pickupBookSounds_;

    std::vector<UISample> eatingSounds_;
    std::vector<UISample> drinkingSounds_;

    std::vector<UISample> levelUpSounds_;
    std::vector<UISample> achievementSounds_;

    std::vector<UISample> errorSounds_;
    std::vector<UISample> selectTargetSounds_;
    std::vector<UISample> deselectTargetSounds_;
    std::vector<UISample> whisperSounds_;
    std::vector<UISample> mailSounds_;
    std::vector<UISample> minimapPingSounds_;

    // State tracking
    float volumeScale_ = 1.0f;
    bool initialized_ = false;

    // Helper methods
    bool loadSound(const std::string& path, UISample& sample, pipeline::AssetManager* assets);
    void ensureItemGroupSoundsLoaded();
    bool itemGroupSoundsBuilt_ = false;
    struct ItemGroupSoundRow {
        uint32_t pickupSoundId = 0;
        uint32_t putdownSoundId = 0;
    };
    std::unordered_map<uint32_t, ItemGroupSoundRow> itemGroupSounds_;
    std::unordered_map<uint32_t, uint32_t> itemDisplayGroupSounds_;
    std::unordered_map<std::string, UISample> namedSamples_;
    pipeline::AssetManager* assets_ = nullptr;
    void playSound(const std::vector<UISample>& library);
};

} // namespace audio
} // namespace wowee
