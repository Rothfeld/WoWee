#include "core/audio_callback_handler.hpp"
#include "addons/lua_api_registrations.hpp"
#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include "rendering/renderer.hpp"
#include "rendering/animation_controller.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "game/game_handler.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/creature_sound_index.hpp"
#include "audio/footstep_manager.hpp"
#include "audio/music_manager.hpp"
#include "audio/audio_engine.hpp"
#include "audio/sound_entries.hpp"
#include "game/combat_handler.hpp"
#include "game/zone_manager.hpp"
#include "ui/ui_manager.hpp"

namespace wowee { namespace core {

namespace {

bool isPetSoundsEnabled() {
    return addons::storedCVarValue("Sound_EnablePetSounds", "1") != "0";
}

} // namespace

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

AudioCallbackHandler::~AudioCallbackHandler() = default;

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

    // Reputation standing tier-up callback - 3D M2 ReputationLevelUp.m2 plays 8473 via $SND;
    // fall back to 2D only when no renderer/animation controller is active.
    gameHandler_.setReputationLevelUpCallback([this]() {
        bool triggeredVisual = false;
        if (renderer_) {
            if (auto* ac = renderer_->getAnimationController()) {
                ac->triggerReputationLevelUpEffect(renderer_->getCharacterPosition());
                triggeredVisual = true;
            }
        }
        if (!triggeredVisual) {
            const float sfxScale = audioCoordinator_ ? audioCoordinator_->getSfxVolumeScale() : 1.0f;
            if (sfxScale > 0.0f) {
                if (const auto* entry = audio::SoundEntryTable::instance().findByName(&assetManager_, "ReputationLevelUp")) {
                    audio::SoundEntryTable::instance().play2D(&assetManager_, entry->id, sfxScale);
                }
            }
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
            music->playMusic(path, /*loop=*/false, /*fadeInMs=*/0.0f, rec->id);
        }
    });

    // WorldStateZoneSounds.dbc sync (SMSG_INIT_WORLD_STATES / SMSG_UPDATE_WORLD_STATE):
    // immediately update ZoneManager's world-state map and clear AudioCoordinator's
    // switch cooldown so the next frame's updateZoneAudio() performs a seamless
    // overlapping crossfade to the new ZoneMusic set without a silence gap.
    auto syncWorldStateAudio = [this]() {
        auto* zm = renderer_ ? renderer_->getZoneManager() : nullptr;
        if (!zm) return;
        const bool affectedChanged = zm->setWorldStates(gameHandler_.getWorldStates());
        if (!affectedChanged || !audioCoordinator_) return;

        auto* music = audioCoordinator_->getMusicManager();
        if (!music || (!music->isPlaying() && !music->isLoading())) {
            audioCoordinator_->notifyWorldStateMusicChanged();
            return;
        }
        const std::string& activeTrack = music->getActiveOrPendingTrack();
        if (activeTrack.empty() || activeTrack.rfind("file:", 0) == 0) return;

        uint32_t activeSet = 0;
        if (renderer_ && renderer_->getCurrentAreaId() != 0) {
            activeSet = zm->getAreaZoneMusicId(renderer_->getCurrentAreaId());
        }
        if (activeSet == 0 && renderer_ && renderer_->getCurrentZoneId() != 0) {
            activeSet = zm->getAreaZoneMusicId(renderer_->getCurrentZoneId());
        }
        if (activeSet == 0 && gameHandler_.getWorldStateAreaId() != 0) {
            activeSet = zm->getAreaZoneMusicId(gameHandler_.getWorldStateAreaId());
        }
        if (activeSet == 0 && audioCoordinator_->getCurrentZoneId() != 0) {
            activeSet = zm->getAreaZoneMusicId(audioCoordinator_->getCurrentZoneId());
        }
        if (activeSet != 0 && !zm->zoneMusicSetContainsTrack(activeSet, activeTrack)) {
            audioCoordinator_->notifyWorldStateMusicChanged();
        }
    };
    syncWorldStateAudio();
    gameHandler_.setWorldStateChangedCallback(syncWorldStateAudio);

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
            const bool listenerAtChar = !audioCoordinator_ || audioCoordinator_->isListenerAtCharacter();
            if (listenerAtChar) {
                audio::SoundEntryTable::instance().play2D(
                    &assetManager_, soundId, sfxScale * audio::FootstepManager::kLocalSelf2DGain);
            } else {
                glm::vec3 pos{0.0f};
                if (renderer_) {
                    pos = renderer_->getCharacterPosition();
                } else if (entity) {
                    pos = core::coords::canonicalToRender(
                        glm::vec3{entity->getLatestX(), entity->getLatestY(), entity->getLatestZ()});
                }
                audio::SoundEntryTable::instance().play3D(
                    &assetManager_, soundId,
                    pos + glm::vec3(0.0f, 0.0f, audio::FootstepManager::kLocalListenerZOffset),
                    sfxScale);
            }
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

    if (auto* ch = gameHandler_.getCombatHandler()) {
        auto ensureCreatureSounds = [this]() -> const audio::CreatureSoundIndex* {
            if (!creatureSoundsLoaded_) {
                creatureSoundsLoaded_ = true;
                auto idx = std::make_unique<audio::CreatureSoundIndex>();
                if (idx->load(assetManager_)) {
                    creatureSounds_ = std::move(idx);
                }
            }
            return creatureSounds_.get();
        };

        ch->setPetActionSoundCallback([this, ensureCreatureSounds](uint64_t petGuid, uint32_t petTalk) {
            if (!isPetSoundsEnabled()) return;
            const float sfxScale = audioCoordinator_ ? audioCoordinator_->getSfxVolumeScale() : 1.0f;
            if (sfxScale <= 0.0f) return;
            const auto* idx = ensureCreatureSounds();
            if (!idx) return;
            auto entity = gameHandler_.getEntityManager().getEntity(petGuid);
            auto unit = std::dynamic_pointer_cast<game::Unit>(entity);
            if (!unit || unit->getDisplayId() == 0) return;
            const uint32_t soundDataId = idx->soundDataForDisplay(unit->getDisplayId());
            const auto col = (petTalk == 1)
                ? audio::CreatureSoundColumn::PetAttack
                : audio::CreatureSoundColumn::PetOrder;
            const uint32_t soundId = idx->sound(soundDataId, col);
            if (soundId == 0) return;
            glm::vec3 canonical{unit->getLatestX(), unit->getLatestY(), unit->getLatestZ()};
            glm::vec3 pos = core::coords::canonicalToRender(canonical);
            audio::SoundEntryTable::instance().play3D(&assetManager_, soundId, pos, sfxScale);
        });

        ch->setPetDismissSoundCallback([this, ensureCreatureSounds](uint32_t creatureSoundDataId,
                                                                     const glm::vec3& serverPos) {
            if (!isPetSoundsEnabled()) return;
            const float sfxScale = audioCoordinator_ ? audioCoordinator_->getSfxVolumeScale() : 1.0f;
            if (sfxScale <= 0.0f) return;
            const auto* idx = ensureCreatureSounds();
            if (!idx) return;
            const uint32_t soundId = idx->sound(creatureSoundDataId, audio::CreatureSoundColumn::PetDismiss);
            if (soundId == 0) return;
            glm::vec3 canonical = core::coords::serverToCanonical(serverPos);
            glm::vec3 pos = core::coords::canonicalToRender(canonical);
            audio::SoundEntryTable::instance().play3D(&assetManager_, soundId, pos, sfxScale);
        });
    }
}

}} // namespace wowee::core
