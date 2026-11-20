#include "core/audio_callback_handler.hpp"
#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include "rendering/renderer.hpp"
#include "rendering/animation_controller.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "game/game_handler.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/music_manager.hpp"
#include "audio/audio_engine.hpp"
#include "audio/sound_entries.hpp"
#include "ui/ui_manager.hpp"

namespace wowee { namespace core {

AudioCallbackHandler::AudioCallbackHandler(
    pipeline::AssetManager& assetManager,
    audio::AudioCoordinator* audioCoordinator,
    rendering::Renderer* renderer,
    ui::UIManager* uiManager,
    game::GameHandler& gameHandler)
    : assetManager_(assetManager)
    , audioCoordinator_(audioCoordinator)
    , renderer_(renderer)
    , uiManager_(uiManager)
    , gameHandler_(gameHandler)
{
}

void AudioCallbackHandler::setupCallbacks() {
    // Level-up callback - play sound, cheer emote, and trigger UI ding overlay + 3D effect
    gameHandler_.setLevelUpCallback([this](uint32_t newLevel) {
        if (uiManager_) {
            uiManager_->getGameScreen().toastManager().triggerDing(newLevel);
        }
        if (renderer_) {
            if (auto* ac = renderer_->getAnimationController()) ac->triggerLevelUpEffect(renderer_->getCharacterPosition());
        }
    });

    // Achievement earned callback - show toast banner
    gameHandler_.setAchievementEarnedCallback([this](uint32_t /*achievementId*/,
                                                    const std::string& /*name*/) {
        if (uiManager_) {
            uiManager_->getGameScreen().toastManager().playAchievementEarned();
        }
    });

    // Server-triggered music callback (SMSG_PLAY_MUSIC)
    // Resolves soundId -> SoundEntries.dbc -> MPQ path -> MusicManager.
    gameHandler_.setPlayMusicCallback([this](uint32_t soundId) {
        auto* music = audioCoordinator_ ? audioCoordinator_->getMusicManager() : nullptr;
        if (!music) return;

        const auto* rec = audio::SoundEntryTable::instance().findById(&assetManager_, soundId);
        if (!rec) return;
        const std::string path = audio::SoundEntryTable::instance().pickExistingPath(&assetManager_, *rec);
        if (!path.empty()) {
            music->playMusic(path, /*loop=*/false);
        }
    });

    // SMSG_PLAY_SOUND: look up SoundEntries.dbc and play 2-D sound effect
    gameHandler_.setPlaySoundCallback([this](uint32_t soundId) {
        const float sfxScale = audioCoordinator_ ? audioCoordinator_->getSfxVolumeScale() : 1.0f;
        audio::SoundEntryTable::instance().play2D(&assetManager_, soundId, sfxScale);
    });

    // SMSG_PLAY_OBJECT_SOUND / SMSG_PLAY_SPELL_IMPACT: play as 3D positional sound at source entity
    gameHandler_.setPlayPositionalSoundCallback([this](uint32_t soundId, uint64_t sourceGuid) {
        const float sfxScale = audioCoordinator_ ? audioCoordinator_->getSfxVolumeScale() : 1.0f;
        if (sfxScale <= 0.0f) return;

        // Play as 3D sound if source entity position is available and not the local player.
        auto entity = gameHandler_.getEntityManager().getEntity(sourceGuid);
        if (entity && sourceGuid != gameHandler_.getPlayerGuid()) {
            glm::vec3 canonical{entity->getLatestX(), entity->getLatestY(), entity->getLatestZ()};
            glm::vec3 pos = core::coords::canonicalToRender(canonical);
            audio::SoundEntryTable::instance().play3D(&assetManager_, soundId, pos, sfxScale);
        } else {
            audio::SoundEntryTable::instance().play2D(&assetManager_, soundId, sfxScale);
        }
    });

    // Other player level-up callback - trigger 3D effect + chat notification
    gameHandler_.setOtherPlayerLevelUpCallback([this](uint64_t guid, uint32_t newLevel) {
        if (!renderer_) return;

        // Trigger 3D effect at the other player's position
        auto entity = gameHandler_.getEntityManager().getEntity(guid);
        if (entity) {
            glm::vec3 canonical(entity->getX(), entity->getY(), entity->getZ());
            glm::vec3 renderPos = core::coords::canonicalToRender(canonical);
            if (auto* ac = renderer_->getAnimationController()) ac->triggerLevelUpEffect(renderPos);
        }

        // Show chat message if in group
        if (gameHandler_.isInGroup()) {
            std::string name = gameHandler_.getCachedPlayerName(guid);
            if (name.empty()) name = "A party member";
            game::MessageChatData msg;
            msg.type = game::ChatType::SYSTEM;
            msg.language = game::ChatLanguage::UNIVERSAL;
            msg.message = name + " has reached level " + std::to_string(newLevel) + "!";
            gameHandler_.addLocalChatMessage(msg);
        }
    });

    // Open dungeon finder callback - server sends SMSG_OPEN_LFG_DUNGEON_FINDER
    gameHandler_.setOpenLfgCallback([this]() {
        if (uiManager_) uiManager_->getGameScreen().openDungeonFinder(gameHandler_);
    });
}

}} // namespace wowee::core
