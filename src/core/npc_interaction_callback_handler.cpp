#include "core/npc_interaction_callback_handler.hpp"
#include "core/entity_spawner.hpp"
#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include "game/game_handler.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/npc_voice_manager.hpp"

namespace wowee { namespace core {

NPCInteractionCallbackHandler::NPCInteractionCallbackHandler(
    EntitySpawner& entitySpawner,
    game::GameHandler& gameHandler,
    audio::AudioCoordinator* audioCoordinator)
    : entitySpawner_(entitySpawner)
    , gameHandler_(gameHandler)
    , audioCoordinator_(audioCoordinator)
{
}

audio::VoiceType NPCInteractionCallbackHandler::resolveNpcVoiceType(uint64_t guid) const {
    audio::VoiceType voiceType = audio::VoiceType::GENERIC;
    auto entity = gameHandler_.getEntityManager().getEntity(guid);
    if (entity && entity->getType() == game::ObjectType::UNIT) {
        auto unit = std::static_pointer_cast<game::Unit>(entity);
        uint32_t displayId = unit->getDisplayId();
        voiceType = entitySpawner_.detectVoiceTypeFromDisplayId(displayId);
    }
    return voiceType;
}

void NPCInteractionCallbackHandler::setupCallbacks() {
    auto displayIdOf = [this](uint64_t guid) -> uint32_t {
        auto entity = gameHandler_.getEntityManager().getEntity(guid);
        if (entity && entity->getType() == game::ObjectType::UNIT)
            return std::static_pointer_cast<game::Unit>(entity)->getDisplayId();
        return 0;
    };

    // NPC greeting callback - play voice line
    gameHandler_.setNpcGreetingCallback([this, displayIdOf](uint64_t guid, const glm::vec3& position) {
        // Greeting audio does not imply a body emote. Server emote packets
        // own NPC animations; replaying a welcome here resets every click.
        if (audioCoordinator_ && audioCoordinator_->getNpcVoiceManager()) {
            // Convert canonical to render coords for 3D audio
            glm::vec3 renderPos = core::coords::canonicalToRender(position);
            audio::VoiceType voiceType = resolveNpcVoiceType(guid);
            audioCoordinator_->getNpcVoiceManager()->playGreeting(guid, voiceType, renderPos, displayIdOf(guid));
        }
    });

    // NPC farewell callback - play farewell voice line
    gameHandler_.setNpcFarewellCallback([this, displayIdOf](uint64_t guid, const glm::vec3& position) {
        if (audioCoordinator_ && audioCoordinator_->getNpcVoiceManager()) {
            glm::vec3 renderPos = core::coords::canonicalToRender(position);
            audio::VoiceType voiceType = resolveNpcVoiceType(guid);
            audioCoordinator_->getNpcVoiceManager()->playFarewell(guid, voiceType, renderPos, displayIdOf(guid));
        }
    });

    // NPC vendor callback - play vendor voice line
    gameHandler_.setNpcVendorCallback([this, displayIdOf](uint64_t guid, const glm::vec3& position) {
        if (audioCoordinator_ && audioCoordinator_->getNpcVoiceManager()) {
            glm::vec3 renderPos = core::coords::canonicalToRender(position);
            audio::VoiceType voiceType = resolveNpcVoiceType(guid);
            audioCoordinator_->getNpcVoiceManager()->playVendor(guid, voiceType, renderPos, displayIdOf(guid));
        }
    });

    // NPC aggro callback - play combat start voice line
    gameHandler_.setNpcAggroCallback([this, displayIdOf](uint64_t guid, const glm::vec3& position) {
        if (audioCoordinator_ && audioCoordinator_->getNpcVoiceManager()) {
            glm::vec3 renderPos = core::coords::canonicalToRender(position);
            audio::VoiceType voiceType = resolveNpcVoiceType(guid);
            audioCoordinator_->getNpcVoiceManager()->playAggro(
                guid, displayIdOf(guid), voiceType, renderPos);
        }
    });
}

}} // namespace wowee::core
