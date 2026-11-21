#include "core/weapon_attachment.hpp"
#include "core/entity_spawner.hpp"
#include "rendering/character_scale.hpp"
#include "rendering/animation/rider_pose.hpp"
#include "rendering/camera_controller.hpp"
#include "rendering/unit_fade.hpp"
#include "rendering/m2_model_classifier.hpp"
#include "core/appearance_composer.hpp"
#include "pipeline/char_sections.hpp"
#include "core/geoset_rules.hpp"
#include "core/npc_equipment_geosets.hpp"
#include "pipeline/item_textures.hpp"
#include "core/helm_visual.hpp"

#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include "rendering/renderer.hpp"
#include "rendering/spell_visual_system.hpp"
#include "rendering/animation_controller.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "audio/npc_voice_manager.hpp"
#include "pipeline/m2_loader.hpp"
#include "pipeline/wmo_loader.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "rendering/animation/emote_registry.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"
#include "game/game_handler.hpp"
#include "game/game_services.hpp"
#include "game/transport_manager.hpp"

#include <cmath>
#include <bit>
#include <cassert>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <cstring>

#include <set>

namespace wowee {
namespace core {

// The geoset numbers come from appearance_composer.hpp, which this file used to
// keep its own copy of.
//
// They were identical when written and stopped being so the moment one of them
// was corrected: group 20 is the feet, an HD human female carries 2001 where an
// HD human male carries 2002, and the fix that names both went into the header's
// copy. This file kept asking for 2002 alone, so every character it draws - an
// NPC, and another player - lost their feet on exactly the models the header's
// copy had been taught about.
//
// One definition, so the next correction cannot land in only half the client.

// --- Constructor / Destructor ---

EntitySpawner::EntitySpawner(rendering::Renderer* renderer,
                             pipeline::AssetManager* assetManager,
                             game::GameHandler* gameHandler,
                             game::GameServices* gameServices)
    : renderer_(renderer)
    , assetManager_(assetManager)
    , gameHandler_(gameHandler)
    , gameServices_(gameServices)
{
}

EntitySpawner::~EntitySpawner() = default;
// --- Lifecycle ---

void EntitySpawner::initialize() {
    buildCharSectionsCache();
    buildCreatureDisplayLookups();
    buildGameObjectDisplayLookups();
}

void EntitySpawner::update() {
    processPlayerSpawnQueue();
    processCreatureSpawnQueue();
    processAsyncNpcCompositeResults();
    processDeferredEquipmentQueue();
    processGameObjectSpawnQueue();
    processPendingTransportRegistrations();
    processPendingTransportDoodads();
    processFadingGameObjects();
    processPendingMount();
    processPendingRemotePlayerMounts();
    syncCreatureVisualState();
    syncPlayerMorphs();
}

// A player's display that is not its native display is a creature model:
// Little Red Riding Hood (30753, SPELL_AURA_TRANSFORM to creature 17602,
// display 17629) turns the Big Bad Wolf's target into Red Riding Hood in the
// original, local player and other players alike, and back when the aura
// ends. The character instance keeps its position, facing, animation and
// attachments; the creature instance is drawn in its place and follows it.
namespace {
// OBJECT_FIELD_SCALE_X; 1 when the server has not sent it.
float unitObjectScale(const game::Unit& unit) {
    const auto field = unit.getFields().find(game::fieldIndex(game::UF::OBJECT_FIELD_SCALE_X));
    if (field == unit.getFields().end()) return 1.0f;
    const float scale = std::bit_cast<float>(field->second);
    return scale > 0.0f ? scale : 1.0f;
}
} // namespace

void EntitySpawner::syncPlayerMorphs() {
    if (!renderer_ || !gameHandler_) return;
    auto* cr = renderer_->getCharacterRenderer();
    if (!cr) return;
    const uint16_t nativeField = game::fieldIndex(game::UF::UNIT_FIELD_NATIVEDISPLAYID);
    if (nativeField == 0xFFFF) return;  // expansion layout without the field
    std::vector<uint64_t> players;
    players.reserve(playerInstances_.size() + 1);
    players.push_back(gameHandler_->getPlayerGuid());
    for (const auto& [guid, instance] : playerInstances_) players.push_back(guid);

    auto drop = [&](uint64_t guid) {
        const auto found = playerMorphs_.find(guid);
        if (found == playerMorphs_.end()) return;
        if (auto* visuals = renderer_->getSpellVisualSystem())
            visuals->transferUnitVisuals(found->second.instanceId, found->second.playerInstanceId);
        cr->removeInstance(found->second.instanceId);
        if (cr->getInstanceModelData(found->second.playerInstanceId))
            cr->setInstanceReplaced(found->second.playerInstanceId, false);
        LOG_INFO("Player morph ended: guid=0x", std::hex, guid, std::dec, " display=", found->second.displayId);
        playerMorphs_.erase(found);
    };
    std::erase_if(playerMorphs_, [&](auto& entry) {
        if (std::find(players.begin(), players.end(), entry.first) != players.end()) return false;
        cr->removeInstance(entry.second.instanceId);
        return true;
    });
    for (const uint64_t guid : players) {
        const auto unit = std::dynamic_pointer_cast<game::Unit>(gameHandler_->getEntityManager().getEntity(guid));
        const uint32_t playerInstance = characterInstanceIdForGuid(guid);
        if (!unit || !playerInstance) { drop(guid); continue; }
        const uint32_t display = unit->getDisplayId();
        const uint32_t native = unit->getField(nativeField);
        const bool morphed = native != 0 && display != 0 && display != native;
        // A character is drawn at its race display's CreatureDisplayInfo and
        // CreatureModelData scale times OBJECT_FIELD_SCALE_X, as a creature
        // is: build 8606 draws a tauren male (display 59, 1.35) and a gnome
        // (1563, 1.15) larger than a scale-1 model, its horse with it, and
        // .modify scale 1.5 grows character and horse alike
        // (_experiments/20261118T160000Z.player_scale).
        const float objectScale = unitObjectScale(*unit);
        if (guid == gameHandler_->getPlayerGuid()) {
            // The camera pivots on the model the player is drawn as, a
            // morph's or a form's included (CameraController::nativePivotHeight).
            const auto shown = displayDataMap_.find(display);
            const auto height = shown != displayDataMap_.end()
                ? modelIdToCollisionHeight_.find(shown->second.modelId) : modelIdToCollisionHeight_.end();
            if (auto* camera = renderer_->getCameraController())
                camera->setFollowedCollisionHeight(height != modelIdToCollisionHeight_.end() ? height->second : 0.0f);
        }
        if (native != 0) {
            const float characterScale = rendering::characterRenderScale(
                objectScale, creatureDisplayScale(native), creatureModelScale(native));
            if (cr->getInstanceScale(playerInstance) != characterScale) {
                cr->setInstanceScale(playerInstance, characterScale);
                LOG_INFO("Player scale: guid=0x", std::hex, guid, std::dec, " display ", native,
                         " scale ", characterScale);
            }
        }
        auto found = playerMorphs_.find(guid);
        if (found != playerMorphs_.end() &&
            (!morphed || found->second.displayId != display || found->second.playerInstanceId != playerInstance)) {
            drop(guid);
            found = playerMorphs_.end();
        }
        if (!morphed) continue;
        glm::vec3 position, rotation;
        if (!cr->getInstancePosition(playerInstance, position) || !cr->getInstanceRotation(playerInstance, rotation))
            continue;
        if (found == playerMorphs_.end()) {
            uint32_t modelId = 0;
            std::string modelPath;
            float unusedHeight = 0.0f;
            if (!loadRemoteMountModel(display, modelId, modelPath, unusedHeight)) {
                LOG_WARNING("Player morph: display ", display, " has no loadable model; guid=0x", std::hex, guid);
                playerMorphs_[guid] = {.displayId = display, .instanceId = 0, .playerInstanceId = playerInstance};
                continue;
            }
            const float scale = rendering::characterRenderScale(
                objectScale, creatureDisplayScale(display), creatureModelScale(display));
            const uint32_t instance = cr->createInstance(modelId, position, rotation, scale);
            if (!instance) continue;
            cr->setInstanceCreatureModel(instance, display, creatureModelDataIdForDisplay(display));
            applyCreatureInstanceAppearance(instance, modelId, display);
            // The display's CreatureModelAlpha, as for a creature: Ghost Wolf
            // (display 4613, alpha 102) is drawn see-through in 2.4.3.
            cr->setInstanceOpacity(instance, creatureDisplayAlpha(display));
            LOG_INFO("Player morph: guid=0x", std::hex, guid, std::dec, " display ", display, " native ", native,
                     " model=", modelPath, " scale=", scale);
            found = playerMorphs_.emplace(guid, PlayerMorph{.displayId = display, .instanceId = instance,
                                                              .playerInstanceId = playerInstance}).first;
            if (auto* visuals = renderer_->getSpellVisualSystem())
                visuals->transferUnitVisuals(playerInstance, instance);
        }
        auto& morph = found->second;
        if (!morph.instanceId) continue;
        cr->setInstanceScale(morph.instanceId, rendering::characterRenderScale(
            objectScale, creatureDisplayScale(display), creatureModelScale(display)));
        cr->setInstanceReplaced(playerInstance, true);
        // First-person hiding and camera fade belong to the character; the
        // creature drawn for it follows them.
        float cameraOpacity = 1.0f;
        cr->setInstanceVisible(morph.instanceId, cr->isInstanceDrawnVisible(playerInstance, cameraOpacity));
        cr->setInstanceCameraOpacity(morph.instanceId, cameraOpacity);
        cr->setInstancePosition(morph.instanceId, position);
        cr->setInstanceRotation(morph.instanceId, rotation);
        // A seated character's creature sits in the same seat.
        cr->setInstanceSeat(morph.instanceId, cr->instanceSeat(playerInstance));
        uint32_t animation = 0;
        float timeMs = 0.0f, durationMs = 0.0f;
        if (cr->getAnimationState(playerInstance, animation, timeMs, durationMs) && animation != morph.animation) {
            morph.animation = animation;
            // A corpse keeps its death pose on the new model: Death once while
            // the fall is still playing, Dead after (removeCreatureForNewDisplay).
            uint32_t play = animation;
            bool loop = true;
            if (animation == rendering::anim::DEATH) {
                if (timeMs >= durationMs) play = rendering::anim::DEAD;
                else loop = false;
            }
            cr->playAnimation(morph.instanceId,
                              cr->hasAnimation(morph.instanceId, play) || rendering::anim::isCorpsePose(play) ? play : 0u,
                              loop);
        }
    }
}

void EntitySpawner::syncCreatureVisualState() {
    if (!renderer_ || !gameHandler_) return;
    auto* characterRenderer = renderer_->getCharacterRenderer();
    if (!characterRenderer) return;

    // Scale and display alpha change rarely, but this scan visits every spawned creature.
    // Doing that each frame with the mutex-locked getEntity() plus a
    // dynamic_pointer_cast per creature was a measurable main-thread cost in
    // crowded areas - a few sweeps per second is visually indistinguishable.
    if (++stealthSyncFrameCounter_ % 15 != 0) return;

    // EntitySpawner::update() runs on the main thread, so the unlocked
    // getEntities() reference is safe and avoids a mutex acquire per creature.
    const auto& entities = gameHandler_->getEntityManager().getEntities();

    for (const auto& [guid, instanceId] : creatureInstances_) {
        auto entIt = entities.find(guid);
        if (entIt == entities.end() || !entIt->second) continue;
        const game::Entity* entity = entIt->second.get();
        if (entity->getType() != game::ObjectType::UNIT &&
            entity->getType() != game::ObjectType::PLAYER) continue;
        const auto* unit = static_cast<const game::Unit*>(entity);

        // Query metadata may arrive after the model. Re-evaluate family/level
        // and object scale here so packet arrival order cannot freeze its size.
        const auto field = unit->getFields().find(game::fieldIndex(game::UF::OBJECT_FIELD_SCALE_X));
        if (field != unit->getFields().end()) {
            // A changed OBJECT_FIELD_SCALE_X eases in; late family/level
            // metadata only corrects the size.
            const float rendered = creatureRenderScale(*unit, unit->getDisplayId(), std::bit_cast<float>(field->second));
            const auto [seen, first] = creatureServerScaleBits_.try_emplace(guid, field->second);
            if (!first && seen->second != field->second) {
                seen->second = field->second;
                characterRenderer->setInstanceTargetScale(instanceId, rendered);
            } else if (!characterRenderer->isInstanceScaleTransitioning(instanceId)) {
                characterRenderer->setInstanceScale(instanceId, rendered);
            }
        }

        // CreatureDisplayInfo's model alpha (the Boar Spirit's display 4716
        // is 128). Stealth fades a unit through its aura's SpellVisualKit
        // (CharProc 14), not through the CREEP visibility flag: in 2.4.3 a
        // creature under Stealth 31621 (CREEP, no kit) stays opaque, and one
        // under Stealth 1784 (kit 312, 0.3) is drawn at 0.3, not less.
        const float opacity = creatureDisplayAlpha(unit->getDisplayId());
        auto [it, inserted] = creatureAppliedOpacity_.try_emplace(guid, opacity);
        if (!inserted && it->second == opacity) continue;

        it->second = opacity;
        characterRenderer->setInstanceOpacity(instanceId, opacity);
    }
}

void EntitySpawner::shutdown() {
    clearAllQueues();
    // Clear all instances
    for (const auto& [guid, instId] : replacedCreatureInstances_) {
        if (renderer_ && renderer_->getCharacterRenderer() && instId != 0)
            renderer_->getCharacterRenderer()->removeInstance(instId);
    }
    replacedCreatureInstances_.clear();
    creatureInstances_.clear();
    creatureModelIds_.clear();
    creatureDisplayIds_.clear();
    requestedCreatureDisplayIds_.clear();
    creatureRenderPosCache_.clear();
    creatureWasMoving_.clear();
    creatureWasSwimming_.clear();
    creatureWasFlying_.clear();
    creatureWasWalking_.clear();
    remoteMovementFlags_.clear();
    remoteWasMovingBackward_.clear();
    remoteWasAirborne_.clear();
    remoteAirborneLegs_.clear();
    remoteLegYawOffsets_.clear();
    remoteTurns_.clear();
    hoverAirborne_.clear();
    liftOffEmoted_.clear();
    creatureSwimmingState_.clear();
    creatureWalkingState_.clear();
    creatureFlyingState_.clear();
    creatureActiveEmotes_.clear();
    creatureAppliedOpacity_.clear();
    creatureServerScaleBits_.clear();
    creatureWeaponStates_.clear();
    creatureWeaponAttachments_.clear();
    creatureWeaponsAttached_.clear();
    creatureWeaponAttachAttempts_.clear();
    playerInstances_.clear();
    onlinePlayerAppearance_.clear();
    remoteStandStates_.clear();
    unitTransformAuraVisuals_.clear();
    remotePlayerMounts_.clear();
    pendingRemotePlayerMounts_.clear();
    gameObjectInstances_.clear();
    fadingGameObjects_.clear();
}

void EntitySpawner::clearRemoteMotionState(uint64_t guid) {
    creatureWasMoving_.erase(guid);
    creatureWasSwimming_.erase(guid);
    creatureWasFlying_.erase(guid);
    creatureWasWalking_.erase(guid);
    remoteMovementFlags_.erase(guid);
    remoteWasMovingBackward_.erase(guid);
    remoteWasAirborne_.erase(guid);
    remoteAirborneLegs_.erase(guid);
    remoteLegYawOffsets_.erase(guid);
    remoteTurns_.erase(guid);
    hoverAirborne_.erase(guid);
    liftOffEmoted_.erase(guid);
}

void EntitySpawner::resetAllState() {
    // Wait for in-flight async loads before clearing state
    for (auto& load : asyncCreatureLoads_) {
        if (load.future.valid()) load.future.wait();
    }

    // Despawn all entities (renderer cleanup)
    despawnAllCreatures();
    despawnAllPlayers();
    despawnAllGameObjects();
    clearMountState();

    // Clear all queues and async loads
    clearAllQueues();

    // Clear all instance tracking
    creatureInstances_.clear();
    creatureModelIds_.clear();
    creatureDisplayIds_.clear();
    requestedCreatureDisplayIds_.clear();
    creatureRenderPosCache_.clear();
    playerInstances_.clear();
    onlinePlayerAppearance_.clear();
    remoteStandStates_.clear();
    unitTransformAuraVisuals_.clear();
    remotePlayerMounts_.clear();
    pendingRemotePlayerMounts_.clear();
    gameObjectInstances_.clear();
    fadingGameObjects_.clear();

    // Clear animation state maps
    creatureWasMoving_.clear();
    creatureWasSwimming_.clear();
    creatureWasFlying_.clear();
    creatureWasWalking_.clear();
    remoteMovementFlags_.clear();
    remoteWasMovingBackward_.clear();
    remoteWasAirborne_.clear();
    remoteAirborneLegs_.clear();
    remoteLegYawOffsets_.clear();
    remoteTurns_.clear();
    hoverAirborne_.clear();
    liftOffEmoted_.clear();
    creatureSwimmingState_.clear();
    creatureWalkingState_.clear();
    creatureFlyingState_.clear();
    creatureActiveEmotes_.clear();
    // Carried over, a stale entry matching the creature's current stealth state
    // suppresses the opacity call that would apply it, so a stealthed NPC came
    // back fully opaque after a relog. shutdown() already cleared this.
    creatureAppliedOpacity_.clear();
    creatureServerScaleBits_.clear();
    creatureWeaponStates_.clear();
    creatureWeaponAttachments_.clear();
    creatureWeaponsAttached_.clear();
    creatureWeaponAttachAttempts_.clear();
    modelIdIsWolfLike_.clear();

    // Clear display/spawn caches
    nonRenderableCreatureDisplayIds_.clear();
    displayIdModelCache_.clear();
    displayIdTexturesApplied_.clear();
    charSectionsCache_.clear();
    charSectionsCacheBuilt_ = false;

    // Clear GO display caches
    gameObjectDisplayIdModelCache_.clear();
    gameObjectDisplayIdWmoCache_.clear();
    gameObjectDisplayIdFailedCache_.clear();
    // Instance ids in here belong to a renderer that has just been cleared.
    gameObjectPendingAnimPolicy_.clear();
}

void EntitySpawner::rebuildLookups() {
    creatureLookupsBuilt_ = false;
    displayDataMap_.clear();
    humanoidExtraMap_.clear();
    creatureModelIds_.clear();
    creatureRenderPosCache_.clear();
    nonRenderableCreatureDisplayIds_.clear();
    initialize();
}

bool EntitySpawner::hasWorkPending() const {
    return !pendingCreatureSpawns_.empty() || !asyncCreatureLoads_.empty() ||
           !asyncNpcCompositeLoads_.empty() || !pendingPlayerSpawns_.empty() ||
           !asyncEquipmentLoads_.empty() || !deferredEquipmentQueue_.empty() ||
           !pendingGameObjectSpawns_.empty() || !asyncGameObjectLoads_.empty();
}

void EntitySpawner::clearMountState() {
    if (mountInstanceId_ != 0 && renderer_) {
        if (auto* charRenderer = renderer_->getCharacterRenderer()) {
            charRenderer->removeInstance(mountInstanceId_);
        }
    }
    mountInstanceId_ = 0;
    mountModelId_ = 0;
    mountDisplayId_ = 0;
    pendingMountDisplayId_ = 0;
}

void EntitySpawner::setRemotePlayerMountDisplayId(uint64_t guid, uint32_t displayId) {
    if (guid == 0) return;
    pendingRemotePlayerMounts_[guid] = displayId;
}

void EntitySpawner::removeRemotePlayerMount(uint64_t guid) {
    auto it = remotePlayerMounts_.find(guid);
    if (it == remotePlayerMounts_.end()) return;
    if (renderer_) {
        if (auto* cr = renderer_->getCharacterRenderer()) {
            if (it->second.instanceId != 0) cr->removeInstance(it->second.instanceId);
            const auto riderInstance = characterInstanceIdForGuid(guid);
            // WorldLoader::loadOnlineWorldTerrain clears the character renderer
            // and only then resetAllState() despawns the units it tracked, so the
            // rider id can be stale here (SIGSEGV in setInstanceRiding, inlined,
            // teleporting Goldshire -> Nagrand). CMaNGOS unmounts a creature as it
            // dies (Creature::SetDeathState JUST_DIED); the corpse keeps lying
            // where the rider fell, as natively (Wind Reaver .die).
            const auto dismount = rendering::riderDismount(
                riderInstance != 0 && cr->hasInstance(riderInstance),
                riderInstance != 0 && cr->isInstanceDead(riderInstance));
            if (dismount.unseat) cr->setInstanceRiding(riderInstance, false);
            if (dismount.playIdle) cr->playAnimation(riderInstance, remoteIdleAnimation(guid), true);
        }
    }
    remotePlayerMounts_.erase(it);
}

void EntitySpawner::queueTransportRegistration(uint64_t guid, uint32_t entry, uint32_t displayId,
                                                float x, float y, float z, float orientation) {
    pendingTransportRegistrations_.push_back({guid, entry, displayId, x, y, z, orientation});
}

void EntitySpawner::setTransportPendingMove(uint64_t guid, float x, float y, float z, float orientation) {
    pendingTransportMoves_[guid] = {x, y, z, orientation};
}

bool EntitySpawner::hasTransportRegistrationPending(uint64_t guid) const {
    return std::any_of(pendingTransportRegistrations_.begin(), pendingTransportRegistrations_.end(),
                       [guid](const PendingTransportRegistration& reg) { return reg.guid == guid; });
}

void EntitySpawner::updateTransportRegistration(uint64_t guid, uint32_t displayId,
                                                 float x, float y, float z, float orientation) {
    for (auto& reg : pendingTransportRegistrations_) {
        if (reg.guid == guid) {
            reg.displayId = displayId;
            reg.x = x; reg.y = y; reg.z = z; reg.orientation = orientation;
            return;
        }
    }
}

// --- Queue API ---

void EntitySpawner::queueCreatureSpawn(uint64_t guid, uint32_t displayId,
                                        float x, float y, float z, float orientation, float scale) {
    if (creatureInstances_.count(guid)) return;
    requestedCreatureDisplayIds_[guid] = displayId;
    if (pendingCreatureSpawnGuids_.count(guid)) {
        // Replace any not-yet-started request. An older async result can still
        // finish later; processAsyncCreatureResults rejects it against the map.
        pendingCreatureSpawns_.erase(
            std::remove_if(pendingCreatureSpawns_.begin(), pendingCreatureSpawns_.end(),
                           [guid](const PendingCreatureSpawn& spawn) {
                               return spawn.guid == guid;
                           }),
            pendingCreatureSpawns_.end());
    }
    pendingCreatureSpawns_.push_back({guid, displayId, x, y, z, orientation, scale});
    pendingCreatureSpawnGuids_.insert(guid);
}

void EntitySpawner::queuePlayerSpawn(uint64_t guid, uint8_t raceId, uint8_t genderId,
                                      uint32_t appearanceBytes, uint8_t facialFeatures,
                                      float x, float y, float z, float orientation) {
    if (playerInstances_.count(guid)) return;
    if (pendingPlayerSpawnGuids_.count(guid)) return;
    pendingPlayerSpawns_.push_back({guid, raceId, genderId, appearanceBytes, facialFeatures, x, y, z, orientation});
    pendingPlayerSpawnGuids_.insert(guid);
}

void EntitySpawner::queueGameObjectSpawn(uint64_t guid, uint32_t entry, uint32_t displayId,
                                          float x, float y, float z, float orientation, float scale,
                                          const glm::quat& rotation) {
    pendingGameObjectSpawns_.push_back({guid, entry, displayId, x, y, z, orientation, scale, rotation});
    serverGameObjectSpawns_[guid] = pendingGameObjectSpawns_.back();
}

void EntitySpawner::queuePlayerEquipment(uint64_t guid,
                                          const std::array<uint32_t, 19>& displayInfoIds,
                                          const std::array<uint8_t, 19>& inventoryTypes) {
    deferredEquipmentQueue_.push_back({guid, {displayInfoIds, inventoryTypes}});
}

// --- Immediate despawn wrappers ---

void EntitySpawner::clearAllQueues() {
    pendingCreatureSpawns_.clear();
    pendingCreatureSpawnGuids_.clear();
    requestedCreatureDisplayIds_.clear();
    creatureSpawnRetryDeadlines_.clear();
    creaturePermanentFailureGuids_.clear();
    deadCreatureGuids_.clear();
    dyingCreatureGuids_.clear();
    displayChangedCreatureGuids_.clear();
    for (const auto& [guid, instId] : replacedCreatureInstances_) {
        if (renderer_ && renderer_->getCharacterRenderer() && instId != 0)
            renderer_->getCharacterRenderer()->removeInstance(instId);
    }
    replacedCreatureInstances_.clear();
    pendingPlayerSpawns_.clear();
    pendingPlayerSpawnGuids_.clear();
    pendingOnlinePlayerEquipment_.clear();
    deferredEquipmentQueue_.clear();
    pendingGameObjectSpawns_.clear();
    serverGameObjectSpawns_.clear();
    // Including the one that is already partway onto the GPU. An incremental
    // upload survives a map change otherwise: processPendingWmoUploads() only
    // gives up when the renderer pointer is null, and a transition leaves the
    // WMORenderer alive with its contents cleared. The upload then finishes
    // against a renderer that no longer knows the model, calls finishWmoSpawn()
    // on the new map, and puts the old map's building - or a transport, which
    // then registers itself here - into a world it does not belong to.
    pendingWmoUploads_.clear();
    pendingTransportRegistrations_.clear();
    pendingTransportMoves_.clear();
    pendingTransportDoodadBatches_.clear();
    asyncCreatureLoads_.clear();
    asyncCreatureDisplayLoads_.clear();
    asyncEquipmentLoads_.clear();
    asyncNpcCompositeLoads_.clear();
    asyncGameObjectLoads_.clear();
}

void EntitySpawner::despawnAllCreatures() {
    std::vector<uint64_t> guids;
    guids.reserve(creatureInstances_.size());
    for (const auto& [g, _] : creatureInstances_) guids.push_back(g);
    for (auto g : guids) despawnCreature(g);
}

void EntitySpawner::despawnAllPlayers() {
    std::vector<uint64_t> guids;
    guids.reserve(playerInstances_.size());
    for (const auto& [g, _] : playerInstances_) guids.push_back(g);
    for (auto g : guids) despawnPlayer(g);
}

void EntitySpawner::despawnAllGameObjects() {
    std::vector<uint64_t> guids;
    guids.reserve(gameObjectInstances_.size());
    for (const auto& [g, _] : gameObjectInstances_) guids.push_back(g);
    for (auto g : guids) despawnGameObject(g);
    removeFadingGameObjects();
    serverGameObjectSpawns_.clear();
}

// --- Methods extracted from Application (with comments preserved) ---

bool EntitySpawner::tryAttachCreatureVirtualWeapons(uint64_t guid, uint32_t instanceId) {
    if (!renderer_ || !renderer_->getCharacterRenderer() || !assetManager_ || !gameHandler_) return false;
    auto* charRenderer = renderer_->getCharacterRenderer();
    if (!charRenderer) return false;

    auto entity = gameHandler_->getEntityManager().getEntity(guid);
    if (!entity || entity->getType() != game::ObjectType::UNIT) return false;
    auto unit = std::static_pointer_cast<game::Unit>(entity);
    if (!unit) return false;

    const uint16_t slots = game::fieldIndex(game::UF::UNIT_VIRTUAL_ITEM_SLOT_ID);
    const uint16_t info = game::fieldIndex(game::UF::UNIT_VIRTUAL_ITEM_INFO);
    assert(slots != 0xFFFF);
    const bool displayIds = info != 0xFFFF;
    const uint8_t sheathState = creatureRangedDrawn_.count(guid)
        ? 2 : unit->getField(game::fieldIndex(game::UF::UNIT_FIELD_BYTES_2)) & 0xFF;
    auto itemDisplayDbc = assetManager_->loadDBC("ItemDisplayInfo.dbc");
    if (!itemDisplayDbc) return false;
    auto itemDbc = displayIds ? nullptr : assetManager_->loadDBC("Item.dbc");
    if (!displayIds && !itemDbc) return false;

    for (const uint32_t attachment : creatureWeaponAttachments_[guid])
        charRenderer->detachWeapon(instanceId, attachment);
    creatureWeaponAttachments_[guid].clear();
    for (uint16_t slot = 0; slot < 3; ++slot) {
        uint32_t displayId = unit->getField(slots + slot);
        if (!displayId) continue;
        uint8_t inventoryType;
        uint8_t sheathType;
        if (displayIds) {
            const uint32_t info1 = unit->getField(info + slot * 2 + 1);
            inventoryType = info1 & 0xFF;
            sheathType = (info1 >> 8) & 0xFF;
        } else {
            const int32_t record = itemDbc->findRecordById(displayId);
            if (record < 0) {
                LOG_ERROR("NPC equipment item missing: guid=", guid, " item=", displayId);
                return false;
            }
            const auto* itemLayout = pipeline::getActiveDBCLayout()
                ? pipeline::getActiveDBCLayout()->getLayout("Item") : nullptr;
            assert(itemLayout);
            inventoryType = itemDbc->getUInt32(record, (*itemLayout)["InventoryType"]);
            sheathType = itemDbc->getUInt32(record, (*itemLayout)["SheatheType"]);
            displayId = itemDbc->getUInt32(record, (*itemLayout)["DisplayInfoID"]);
        }
        const auto equipSlot = slot == 0 ? game::EquipSlot::MAIN_HAND
            : slot == 1 ? game::EquipSlot::OFF_HAND : game::EquipSlot::RANGED;
        const uint32_t held = heldAttachment(equipSlot, inventoryType);
        uint32_t sheath = sheathAttachment(sheathType, equipSlot);
        // Some models do not author the point at all: QuillBoarWarrior.m2
        // (Razorfen Quilguard/Defender) has no attachment 0 for its shield.
        // That item has nowhere to hang; it is skipped, not retried as a
        // failed attach.
        static std::unordered_set<uint64_t> reported;
        const uint64_t modelKey = uint64_t{charRenderer->getInstanceModelId(instanceId)} << 32;
        if (!charRenderer->hasAttachmentPoint(instanceId, held)) {
            if (reported.insert(modelKey | held).second)
                LOG_INFO("NPC equipment: model of guid=", guid, " has no attachment ", held,
                         "; item display ", displayId, " is not drawn");
            continue;
        }
        if (sheath != UINT32_MAX && !charRenderer->hasAttachmentPoint(instanceId, sheath)) {
            if (reported.insert(modelKey | sheath).second)
                LOG_INFO("NPC equipment: model of guid=", guid, " has no sheath attachment ", sheath,
                         "; item display ", displayId, " is not drawn while sheathed");
            sheath = UINT32_MAX;
        }
        const int32_t record = itemDisplayDbc->findRecordById(displayId);
        if (record < 0) {
            LOG_ERROR("NPC equipment display missing: guid=", guid, " display=", displayId);
            return false;
        }
        const auto art = pipeline::readItemDisplayArt(*itemDisplayDbc, record);
        if (art.modelFile.empty()) continue; // This slot has no visible model.
        const std::string directory = inventoryType == game::InvType::SHIELD
            ? "Item\\ObjectComponents\\Shield\\" : "Item\\ObjectComponents\\Weapon\\";
        pipeline::M2Model model;
        if (!loadWeaponM2(directory + art.modelFile, model)) {
            LOG_ERROR("NPC equipment model failed: ", directory, art.modelFile);
            return false;
        }
        const std::string texture = art.textureName.empty() ? "" : directory + art.textureName + ".blp";
        if (!charRenderer->attachWeapon(instanceId, held, model, nextWeaponModelId_++, texture,
                glm::mat4(1.0f), static_cast<int8_t>(heldSheathState(equipSlot)), sheath)) {
            LOG_ERROR("NPC equipment attachment failed: guid=", guid, " display=", displayId,
                      " attachment=", held);
            return false;
        }
        creatureWeaponAttachments_[guid].push_back(held);
        // Creatures carry no enchantments: the display's own visual only.
        attachItemVisual(instanceId, held, static_cast<int8_t>(heldSheathState(equipSlot)),
                         equippedItemVisual(displayId, 0, 0));
        LOG_INFO("NPC equipment: guid=", guid, " slot=", slot, " display=", displayId,
                 " held=", held, " sheath type=", int(sheathType), " sheath attachment=", sheath,
                 " sheath state=", int(sheathState));
    }
    charRenderer->setSheathState(instanceId, sheathState);
    return true;
}

bool EntitySpawner::retryCreatureVirtualWeapons(uint64_t guid, uint32_t instanceId,
                                                uint8_t maxAttempts) {
    const auto entity = gameHandler_->getEntityManager().getEntity(guid);
    if (!entity) return false;
    const uint16_t slots = game::fieldIndex(game::UF::UNIT_VIRTUAL_ITEM_SLOT_ID);
    const uint16_t info = game::fieldIndex(game::UF::UNIT_VIRTUAL_ITEM_INFO);
    assert(slots != 0xFFFF);
    std::array<uint32_t, 10> state{};
    for (uint16_t i = 0; i < 3; ++i) state[i] = entity->getField(slots + i);
    if (info != 0xFFFF)
        for (uint16_t i = 0; i < 6; ++i) state[3 + i] = entity->getField(info + i);
    state[9] = entity->getField(game::fieldIndex(game::UF::UNIT_FIELD_BYTES_2)) & 0xFF;
    const auto previous = creatureWeaponStates_.find(guid);
    if (previous == creatureWeaponStates_.end() || previous->second != state) {
        creatureWeaponStates_[guid] = state;
        creatureWeaponsAttached_.erase(guid);
        creatureWeaponAttachAttempts_.erase(guid);
    }
    if (creatureWeaponsAttached_.count(guid)) return false;
    const auto attemptIt = creatureWeaponAttachAttempts_.find(guid);
    const uint8_t attempts = attemptIt != creatureWeaponAttachAttempts_.end()
        ? attemptIt->second : 0;
    if (attempts >= maxAttempts) return false;

    if (tryAttachCreatureVirtualWeapons(guid, instanceId)) {
        creatureWeaponsAttached_.insert(guid);
        creatureWeaponAttachAttempts_.erase(guid);
    } else {
        creatureWeaponAttachAttempts_[guid] = static_cast<uint8_t>(attempts + 1);
    }
    return true;
}


void EntitySpawner::buildCharSectionsCache() {
    if (charSectionsCacheBuilt_ || !assetManager_ || !assetManager_->isInitialized()) return;
    auto dbc = assetManager_->loadDBC("CharSections.dbc");
    if (!dbc) return;
    const auto* csL = pipeline::getActiveDBCLayout()
        ? pipeline::getActiveDBCLayout()->getLayout("CharSections") : nullptr;
    auto csF = pipeline::detectCharSectionsFields(dbc.get(), csL);
    for (uint32_t r = 0; r < dbc->getRecordCount(); r++) {
        uint32_t race = dbc->getUInt32(r, csF.raceId);
        uint32_t sex = dbc->getUInt32(r, csF.sexId);
        uint32_t section = dbc->getUInt32(r, csF.baseSection);
        uint32_t variation = dbc->getUInt32(r, csF.variationIndex);
        uint32_t color = dbc->getUInt32(r, csF.colorIndex);
        // We only cache sections 0 (skin), 1 (face), 3 (hair), 4 (underwear)
        if (section != 0 && section != 1 && section != 3 && section != 4) continue;
        for (int ti = 0; ti < 3; ti++) {
            std::string tex = dbc->getString(r, csF.texture1 + ti);
            if (tex.empty()) continue;
            charSectionsCache_.emplace(
                charSectionKey(static_cast<uint8_t>(race), static_cast<uint8_t>(sex),
                               static_cast<uint8_t>(section), static_cast<uint8_t>(variation),
                               static_cast<uint8_t>(color), ti),
                tex);
        }
    }
    charSectionsCacheBuilt_ = true;
    LOG_INFO("CharSections cache built: ", charSectionsCache_.size(), " entries");
}

std::string EntitySpawner::lookupCharSection(uint8_t race, uint8_t sex, uint8_t section,
                                           uint8_t variation, uint8_t color, int texIndex) const {
    auto it = charSectionsCache_.find(
        charSectionKey(race, sex, section, variation, color, texIndex));
    return (it != charSectionsCache_.end()) ? it->second : std::string();
}

void EntitySpawner::buildCreatureDisplayLookups() {
    if (creatureLookupsBuilt_ || !assetManager_ || !assetManager_->isInitialized()) return;

    LOG_INFO("Building creature display lookups from DBC files");

    // These first five columns are shared by Vanilla/TBC/Wrath.
    auto families = assetManager_->loadDBC("CreatureFamily.dbc");
    assert(families && families->isLoaded() && families->getFieldCount() >= 5);
    creatureFamilyScales_.clear();
    for (uint32_t i = 0; i < families->getRecordCount(); ++i) {
        CreatureFamilyScale family{families->getFloat(i, 1), families->getUInt32(i, 2),
                                   families->getFloat(i, 3), families->getUInt32(i, 4)};
        assert(family.maxLevel >= family.minLevel);
        creatureFamilyScales_.emplace(families->getUInt32(i, 0), family);
    }

    // CreatureDisplayInfo.dbc structure (3.3.5a):
    // Col 0: displayId
    // Col 1: modelId
    // Col 3: extendedDisplayInfoID (link to CreatureDisplayInfoExtra.dbc)
    // Col 6: Skin1 (texture name)
    // Col 7: Skin2
    // Col 8: Skin3
    if (auto cdi = assetManager_->loadDBC("CreatureDisplayInfo.dbc"); cdi && cdi->isLoaded()) {
        const auto* cdiL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("CreatureDisplayInfo") : nullptr;
        for (uint32_t i = 0; i < cdi->getRecordCount(); i++) {
            CreatureDisplayData data;
            data.modelId = cdi->getUInt32(i, cdiL ? (*cdiL)["ModelID"] : 1);
            data.extraDisplayId = cdi->getUInt32(i, cdiL ? (*cdiL)["ExtraDisplayId"] : 3);
            data.skin1 = cdi->getString(i, cdiL ? (*cdiL)["Skin1"] : 6);
            data.skin2 = cdi->getString(i, cdiL ? (*cdiL)["Skin2"] : 7);
            data.skin3 = cdi->getString(i, cdiL ? (*cdiL)["Skin3"] : 8);
            // How big this display draws its model. One model serves many
            // displays at many sizes - the boar is 0.6 for a piglet and 1.5
            // for a giant across ten of them - and reading none of it drew
            // every one of them at the model's own size. A helboar is one of
            // the reduced ones, which is where it was noticed.
            //
            // Zero occurs in the file (and in more rows on TBC and vanilla)
            // and means unset, not invisible.
            const uint32_t scaleField = cdiL ? (*cdiL)["CreatureModelScale"] : 4;
            if (scaleField != 0xFFFFFFFF && scaleField < cdi->getFieldCount()) {
                const float displayScale = cdi->getFloat(i, scaleField);
                if (displayScale > 0.0f) data.displayScale = displayScale;
            }
            // CreatureModelAlpha, 0-255. 16106 of 16880 TBC displays are 255;
            // spirits and ghosts are lower (Boar Spirit 128).
            const uint32_t alphaField = cdiL ? (*cdiL)["CreatureModelAlpha"] : 5;
            assert(alphaField < cdi->getFieldCount());
            const uint32_t alpha = cdi->getUInt32(i, alphaField);
            assert(alpha <= 255);
            data.modelAlpha = static_cast<float>(alpha) / 255.0f;
            displayDataMap_[cdi->getUInt32(i, cdiL ? (*cdiL)["ID"] : 0)] = data;
        }
        LOG_INFO("Loaded ", displayDataMap_.size(), " display→model mappings");
    }

    // CreatureDisplayInfoExtra.dbc structure (3.3.5a):
    // Col 0: ID
    // Col 1: DisplayRaceID
    // Col 2: DisplaySexID
    // Col 3: SkinID
    // Col 4: FaceID
    // Col 5: HairStyleID
    // Col 6: HairColorID
    // Col 7: FacialHairID
    // CreatureDisplayInfoExtra.dbc field layout depends on actual field count:
    //   19 fields: 10 equip slots (8-17), BakeName=18 (no Flags field)
    //   21 fields: 11 equip slots (8-18), Flags=19, BakeName=20
    if (auto cdie = assetManager_->loadDBC("CreatureDisplayInfoExtra.dbc"); cdie && cdie->isLoaded()) {
        const auto* cdieL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("CreatureDisplayInfoExtra") : nullptr;
        const uint32_t cdieEquip0 = cdieL ? (*cdieL)["EquipDisplay0"] : 8;
        // Detect actual field count to determine equip slot count and BakeName position
        const uint32_t dbcFieldCount = cdie->getFieldCount();
        int numEquipSlots;
        uint32_t bakeField;
        if (dbcFieldCount <= 19) {
            // 19 fields: 10 equip slots (8-17), BakeName at 18
            numEquipSlots = 10;
            bakeField = 18;
        } else {
            // 21 fields: 11 equip slots (8-18), Flags=19, BakeName=20
            numEquipSlots = 11;
            bakeField = cdieL ? (*cdieL)["BakeName"] : 20;
        }
        uint32_t withBakeName = 0;
        for (uint32_t i = 0; i < cdie->getRecordCount(); i++) {
            HumanoidDisplayExtra extra;
            extra.raceId = static_cast<uint8_t>(cdie->getUInt32(i, cdieL ? (*cdieL)["RaceID"] : 1));
            extra.sexId = static_cast<uint8_t>(cdie->getUInt32(i, cdieL ? (*cdieL)["SexID"] : 2));
            extra.skinId = static_cast<uint8_t>(cdie->getUInt32(i, cdieL ? (*cdieL)["SkinID"] : 3));
            extra.faceId = static_cast<uint8_t>(cdie->getUInt32(i, cdieL ? (*cdieL)["FaceID"] : 4));
            extra.hairStyleId = static_cast<uint8_t>(cdie->getUInt32(i, cdieL ? (*cdieL)["HairStyleID"] : 5));
            extra.hairColorId = static_cast<uint8_t>(cdie->getUInt32(i, cdieL ? (*cdieL)["HairColorID"] : 6));
            extra.facialHairId = static_cast<uint8_t>(cdie->getUInt32(i, cdieL ? (*cdieL)["FacialHairID"] : 7));
            for (int eq = 0; eq < numEquipSlots; eq++) {
                extra.equipDisplayId[eq] = cdie->getUInt32(i, cdieEquip0 + eq);
            }
            extra.bakeName = cdie->getString(i, bakeField);
            if (!extra.bakeName.empty()) withBakeName++;
            humanoidExtraMap_[cdie->getUInt32(i, cdieL ? (*cdieL)["ID"] : 0)] = extra;
        }
        LOG_DEBUG("Loaded ", humanoidExtraMap_.size(), " humanoid display extra entries (",
                 withBakeName, " with baked textures, ", numEquipSlots, " equip slots, ",
                 dbcFieldCount, " DBC fields, bakeField=", bakeField, ")");
    }

    // A character-style NPC (CreatureDisplayInfoExtra) is drawn at its own
    // display scale times the scale of its race and sex's player display
    // (ChrRaces columns 4/5, male/female CreatureDisplayInfo), as a player
    // of that race is. Build 8606 in the visual sweep scene: tauren male NPCs
    // 3793 and 1624 (display scale 1.0) stand 1.35 times WoWee's height, 1941
    // (1.2) 1.36 times WoWee's 1.2, so 1.62; a gnome male 1832 about 1.2
    // (player display 1563: 1.15); a tauren female 1905 and a human 4397 the
    // same (player displays 60 and 49: 1.0).
    // _experiments/20261119T082500Z.orient_shadow/ (sweep_fix, height_ratio.py).
    if (auto races = assetManager_->loadDBC("ChrRaces.dbc"); races && races->isLoaded()) {
        assert(races->getFieldCount() > 5);
        std::unordered_map<uint32_t, float> raceSexScale;  // race << 8 | sex
        for (uint32_t i = 0; i < races->getRecordCount(); ++i) {
            for (uint32_t sex = 0; sex < 2; ++sex) {
                const auto display = displayDataMap_.find(races->getUInt32(i, 4 + sex));
                if (display != displayDataMap_.end())
                    raceSexScale[races->getUInt32(i, 0) << 8 | sex] = display->second.displayScale;
            }
        }
        uint32_t scaled = 0;
        for (auto& [displayId, data] : displayDataMap_) {
            const auto extra = humanoidExtraMap_.find(data.extraDisplayId);
            if (data.extraDisplayId == 0 || extra == humanoidExtraMap_.end()) continue;
            const auto race = raceSexScale.find(uint32_t(extra->second.raceId) << 8 | extra->second.sexId);
            if (race == raceSexScale.end() || race->second == 1.0f) continue;
            data.displayScale *= race->second;
            ++scaled;
        }
        LOG_INFO("Scaled ", scaled, " character-style NPC displays by their race's player display");
    }

    // CreatureModelData.dbc: modelId (col 0) → modelPath (col 2, .mdx → .m2)
    if (auto cmd = assetManager_->loadDBC("CreatureModelData.dbc"); cmd && cmd->isLoaded()) {
        const auto* cmdL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("CreatureModelData") : nullptr;
        for (uint32_t i = 0; i < cmd->getRecordCount(); i++) {
            std::string mdx = cmd->getString(i, cmdL ? (*cmdL)["ModelPath"] : 2);
            if (mdx.empty()) continue;
            if (mdx.size() >= 4) {
                mdx = mdx.substr(0, mdx.size() - 4) + ".m2";
            }
            const uint32_t modelId = cmd->getUInt32(i, cmdL ? (*cmdL)["ID"] : 0);
            modelIdToPath_[modelId] = mdx;
            if (cmdL && cmdL->fields.count("Flags"))
                modelIdToFlags_[modelId] = cmd->getUInt32(i, (*cmdL)["Flags"]);
            if (cmdL) {
                const auto heightField = (*cmdL)["CollisionHeight"];
                if (heightField != 0xFFFFFFFF && heightField < cmd->getFieldCount())
                    modelIdToCollisionHeight_[modelId] = cmd->getFloat(i, heightField);
            }
            // GeoBox and MountHeight, the size build 8606 measures a unit by
            // for its selection circle (0x60ce50, 0x60e560). Only layouts that
            // name them carry them; see docs/unit-size-rules.md.
            if (cmdL) {
                const uint32_t box[6] = {(*cmdL)["GeoBoxMinX"], (*cmdL)["GeoBoxMinY"],
                                         (*cmdL)["GeoBoxMinZ"], (*cmdL)["GeoBoxMaxX"],
                                         (*cmdL)["GeoBoxMaxY"], (*cmdL)["GeoBoxMaxZ"]};
                const uint32_t mountField = (*cmdL)["MountHeight"];
                const bool named = std::all_of(std::begin(box), std::end(box),
                    [&](uint32_t f) { return f < cmd->getFieldCount(); }) &&
                    mountField < cmd->getFieldCount();
                if (named) {
                    ModelSize size;
                    size.geoBox.min = {cmd->getFloat(i, box[0]), cmd->getFloat(i, box[1]),
                                       cmd->getFloat(i, box[2])};
                    size.geoBox.max = {cmd->getFloat(i, box[3]), cmd->getFloat(i, box[4]),
                                       cmd->getFloat(i, box[5])};
                    size.mountHeight = cmd->getFloat(i, mountField);
                    modelIdToSize_[modelId] = size;
                }
            }
            // The other half of a creature's size. Zero appears in the file
            // and means unset rather than invisible, so it keeps 1.0.
            const uint32_t scaleField = cmdL ? (*cmdL)["ModelScale"] : 4;
            if (scaleField != 0xFFFFFFFF && scaleField < cmd->getFieldCount()) {
                const float modelScale = cmd->getFloat(i, scaleField);
                if (modelScale > 0.0f) modelIdToScale_[modelId] = modelScale;
            }
        }
        LOG_INFO("Loaded ", modelIdToPath_.size(), " model→path mappings, ",
                 modelIdToSize_.size(), " CreatureModelData GeoBoxes, ", modelIdToFlags_.size(), " model flags");
        if (modelIdToFlags_.empty())
            LOG_ERROR("dbc_layouts.json names no CreatureModelData.Flags: hit reactions ignore the model's "
                      "no-wound flag (deploy current expansion metadata)");
    }

    // Resolve gryphon/wyvern display IDs by exact model path so taxi mounts have textures.
    auto toLower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    auto normalizePath = [&](const std::string& p) {
        std::string s = p;
        for (char& c : s) if (c == '/') c = '\\';
        return toLower(s);
    };
    // A display with no skin of its own, on a model that asks for one, keeps
    // the slot white. CreatureDisplayInfo leaves Skin1 empty in 545 rows; 517
    // carry an ExtraDisplayId and are baked from CreatureDisplayInfoExtra, the
    // other 28 are ordinary creatures on a model with texture types 11-13.
    // Native 2.4.3 draws those slots untextured white: Riding Wyvern 20413
    // (display 19608, RidingWyvern.m2 types 11 and 12) shows a white body
    // with its red saddle (type 0). Borrowing another display's skin for the
    // same model file drew it as a textured brown wyvern.
    // Evidence: _experiments/20261118T195622Z.visual_sweep_pilot/cases/19608/
    // in the workspace.

    auto resolveDisplayIdForExactPath = [&](const std::string& exactPath) -> uint32_t {
        const std::string target = normalizePath(exactPath);
        // Collect ALL model IDs that map to this path (multiple model IDs can
        // share the same .m2 file, e.g. modelId 147 and 792 both → Gryphon.m2)
        std::vector<uint32_t> modelIds;
        for (const auto& [mid, path] : modelIdToPath_) {
            if (normalizePath(path) == target) {
                modelIds.push_back(mid);
            }
        }
        if (modelIds.empty()) return 0;
        uint32_t bestDisplayId = 0;
        int bestScore = -1;
        for (const auto& [dispId, data] : displayDataMap_) {
            bool matches = false;
            for (uint32_t mid : modelIds) {
                if (data.modelId == mid) { matches = true; break; }
            }
            if (!matches) continue;
            int score = 0;
            if (!data.skin1.empty()) score += 3;
            if (!data.skin2.empty()) score += 2;
            if (!data.skin3.empty()) score += 1;
            if (score > bestScore) {
                bestScore = score;
                bestDisplayId = dispId;
            }
        }
        return bestDisplayId;
    };

    gryphonDisplayId_ = resolveDisplayIdForExactPath("Creature\\Gryphon\\Gryphon.m2");
    wyvernDisplayId_  = resolveDisplayIdForExactPath("Creature\\Wyvern\\Wyvern.m2");
    gameServices_->gryphonDisplayId = gryphonDisplayId_;
    gameServices_->wyvernDisplayId  = wyvernDisplayId_;
    LOG_INFO("Taxi mount displayIds: gryphon=", gryphonDisplayId_, " wyvern=", wyvernDisplayId_);

    // CharHairGeosets.dbc: maps (race, sex, hairStyleId) → skinSectionId for hair mesh
    // Col 0: ID, Col 1: RaceID, Col 2: SexID, Col 3: VariationID, Col 4: GeosetID, Col 5: Showscalp
    if (auto chg = assetManager_->loadDBC("CharHairGeosets.dbc"); chg && chg->isLoaded()) {
        const auto* chgL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("CharHairGeosets") : nullptr;
        for (uint32_t i = 0; i < chg->getRecordCount(); i++) {
            uint32_t raceId = chg->getUInt32(i, chgL ? (*chgL)["RaceID"] : 1);
            uint32_t sexId = chg->getUInt32(i, chgL ? (*chgL)["SexID"] : 2);
            uint32_t variation = chg->getUInt32(i, chgL ? (*chgL)["Variation"] : 3);
            uint32_t geosetId = chg->getUInt32(i, chgL ? (*chgL)["GeosetID"] : 4);
            // Showscalp/Bald means this style uses the default scalp instead
            // of the extra hair-cap mesh. Ignoring it makes the cap physically
            // poke through hair authored to expose the normal scalp.
            const bool useDefaultScalp = chg->getFieldCount() > 5 && chg->getUInt32(i, 5) != 0;
            const uint32_t key = appearanceKey(static_cast<uint8_t>(raceId),
                                              static_cast<uint8_t>(sexId),
                                              static_cast<uint8_t>(variation));
            hairGeosetMap_[key] = hairGeosetFromDbcRow(geosetId, useDefaultScalp);
        }
        LOG_INFO("Loaded ", hairGeosetMap_.size(), " hair geoset mappings from CharHairGeosets.dbc");
    }

    // CharacterFacialHairStyles.dbc: maps (race, sex, facialHairId) → geoset IDs
    // No ID column: Col 0: RaceID, Col 1: SexID, Col 2: VariationID
    // Col 3: Geoset100, Col 4: Geoset300, Col 5: Geoset200
    if (auto cfh = assetManager_->loadDBC("CharacterFacialHairStyles.dbc"); cfh && cfh->isLoaded()) {
        const auto* cfhL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("CharacterFacialHairStyles") : nullptr;
        const auto fhF = pipeline::detectFacialHairFields(cfh.get(), cfhL);
        for (uint32_t i = 0; i < cfh->getRecordCount(); i++) {
            uint32_t raceId = cfh->getUInt32(i, cfhL ? (*cfhL)["RaceID"] : 0);
            uint32_t sexId = cfh->getUInt32(i, cfhL ? (*cfhL)["SexID"] : 1);
            uint32_t variation = cfh->getUInt32(i, cfhL ? (*cfhL)["Variation"] : 2);
            const uint32_t key = appearanceKey(static_cast<uint8_t>(raceId),
                                              static_cast<uint8_t>(sexId),
                                              static_cast<uint8_t>(variation));
            FacialHairGeosets fhg;
            // Which columns those are depends on the copy of the DBC. The
            // nine-column file keeps them at 6-8 with three unused columns
            // before them; the eight-column file - which is what ships here -
            // keeps them at 3-5 and fills 6 and 7 with zero or 0xCCCCCCCC.
            // detectFacialHairFields decides on the field count, since column 8
            // exists only in the longer one.
            fhg.geoset100 = static_cast<uint16_t>(cfh->getUInt32(i, fhF.geoset100));
            fhg.geoset300 = static_cast<uint16_t>(cfh->getUInt32(i, fhF.geoset300));
            fhg.geoset200 = static_cast<uint16_t>(cfh->getUInt32(i, fhF.geoset200));
            facialHairGeosetMap_[key] = fhg;
        }
        LOG_INFO("Loaded ", facialHairGeosetMap_.size(), " facial hair geoset mappings from CharacterFacialHairStyles.dbc");
    }

    creatureLookupsBuilt_ = true;
}

std::string EntitySpawner::getModelPathForDisplayId(uint32_t displayId) const {
    // WotLK 3.3.5a CreatureDisplayInfo tops out around ~32000; values far
    // beyond that are corrupted update-field data or packet parse errors.
    // Silently reject to avoid pointless DBC lookups and log spam.
    constexpr uint32_t kMaxReasonableDisplayId = 100000;
    if (displayId == 0 || displayId > kMaxReasonableDisplayId) {
        return "";
    }

    if (displayId == 30412) return "Creature\\Gryphon\\Gryphon.m2";
    if (displayId == 30413) return "Creature\\Wyvern\\Wyvern.m2";

    // WotLK servers can send display IDs that do not exist in older/local
    // CreatureDisplayInfo datasets. Keep those creatures visible by falling
    // back to a close base model instead of dropping spawn entirely.
    switch (displayId) {
        case 31048: // Diseased Young Wolf variants (AzerothCore WotLK)
        case 31049: // Diseased Wolf variants (AzerothCore WotLK)
            return "Creature\\Wolf\\Wolf.m2";
        default:
            break;
    }

    auto itData = displayDataMap_.find(displayId);
    if (itData == displayDataMap_.end()) {
        // Some sources (e.g., taxi nodes) may provide a modelId directly.
        auto itPath = modelIdToPath_.find(displayId);
        if (itPath != modelIdToPath_.end()) {
            return itPath->second;
        }
        if (displayId == 30412) return "Creature\\Gryphon\\Gryphon.m2";
        if (displayId == 30413) return "Creature\\Wyvern\\Wyvern.m2";
        if (warnedMissingDisplayDataIds_.insert(displayId).second) {
            LOG_WARNING("No display data for displayId ", displayId,
                        " (displayDataMap_ has ", displayDataMap_.size(), " entries)");
        }
        return "";
    }

    auto itPath = modelIdToPath_.find(itData->second.modelId);
    if (itPath == modelIdToPath_.end()) {
        if (warnedMissingModelPathIds_.insert(displayId).second) {
            LOG_WARNING("No model path for modelId ", itData->second.modelId,
                        " from displayId ", displayId,
                        " (modelIdToPath_ has ", modelIdToPath_.size(), " entries)");
        }
        return "";
    }

    // Which model a humanoid display actually resolved to, once. An asset
    // overlay can re-point a display id at a different model, and there is no
    // other way from outside to tell "the re-point never reached the client"
    // from "it reached it and the model looks the same". At debug, with the
    // other per-creature lines: WOWEE_LOG_LEVEL=debug when that is the question.
    if (itData->second.extraDisplayId != 0 &&
        humanoidDisplayCanaryCount_ < 5) {
        ++humanoidDisplayCanaryCount_;
        LOG_DEBUG("Humanoid display ", displayId, " -> model ",
                    itData->second.modelId, " -> ", itPath->second);
    }

    return itPath->second;
}

audio::VoiceType EntitySpawner::detectVoiceTypeFromDisplayId(uint32_t displayId) const {
    // Look up display data
    auto itDisplay = displayDataMap_.find(displayId);
    if (itDisplay == displayDataMap_.end() || itDisplay->second.extraDisplayId == 0) {
        LOG_INFO("Voice detection: displayId ", displayId, " -> GENERIC (no display data)");
        return audio::VoiceType::GENERIC;  // Not a humanoid or no extra data
    }

    // Look up humanoid extra data (race/sex info)
    auto itExtra = humanoidExtraMap_.find(itDisplay->second.extraDisplayId);
    if (itExtra == humanoidExtraMap_.end()) {
        LOG_INFO("Voice detection: displayId ", displayId, " -> GENERIC (no humanoid extra data)");
        return audio::VoiceType::GENERIC;
    }

    uint8_t raceId = itExtra->second.raceId;
    uint8_t sexId = itExtra->second.sexId;

    const char* raceName = "Unknown";
    const char* sexName = (sexId == 0) ? "Male" : "Female";

    // Map (raceId, sexId) to VoiceType
    // Race IDs: 1=Human, 2=Orc, 3=Dwarf, 4=NightElf, 5=Undead, 6=Tauren, 7=Gnome, 8=Troll
    // Sex IDs: 0=Male, 1=Female
    audio::VoiceType result;
    switch (raceId) {
        case 1: raceName = "Human"; result = (sexId == 0) ? audio::VoiceType::HUMAN_MALE : audio::VoiceType::HUMAN_FEMALE; break;
        case 2: raceName = "Orc"; result = (sexId == 0) ? audio::VoiceType::ORC_MALE : audio::VoiceType::ORC_FEMALE; break;
        case 3: raceName = "Dwarf"; result = (sexId == 0) ? audio::VoiceType::DWARF_MALE : audio::VoiceType::DWARF_FEMALE; break;
        case 4: raceName = "NightElf"; result = (sexId == 0) ? audio::VoiceType::NIGHTELF_MALE : audio::VoiceType::NIGHTELF_FEMALE; break;
        case 5: raceName = "Undead"; result = (sexId == 0) ? audio::VoiceType::UNDEAD_MALE : audio::VoiceType::UNDEAD_FEMALE; break;
        case 6: raceName = "Tauren"; result = (sexId == 0) ? audio::VoiceType::TAUREN_MALE : audio::VoiceType::TAUREN_FEMALE; break;
        case 7: raceName = "Gnome"; result = (sexId == 0) ? audio::VoiceType::GNOME_MALE : audio::VoiceType::GNOME_FEMALE; break;
        case 8: raceName = "Troll"; result = (sexId == 0) ? audio::VoiceType::TROLL_MALE : audio::VoiceType::TROLL_FEMALE; break;
        case 10: raceName = "BloodElf"; result = (sexId == 0) ? audio::VoiceType::BLOODELF_MALE : audio::VoiceType::BLOODELF_FEMALE; break;
        case 11: raceName = "Draenei"; result = (sexId == 0) ? audio::VoiceType::DRAENEI_MALE : audio::VoiceType::DRAENEI_FEMALE; break;
        default: result = audio::VoiceType::GENERIC; break;
    }

    LOG_INFO("Voice detection: displayId ", displayId, " -> ", raceName, " ", sexName, " (race=", static_cast<int>(raceId), ", sex=", static_cast<int>(sexId), ")");
    return result;
}

void EntitySpawner::buildGameObjectDisplayLookups() {
    if (gameObjectLookupsBuilt_ || !assetManager_ || !assetManager_->isInitialized()) return;

    LOG_INFO("Building gameobject display lookups from DBC files");

    // GameObjectDisplayInfo.dbc structure (3.3.5a):
    // Col 0: ID (displayId)
    // Col 1: ModelName
    if (auto godi = assetManager_->loadDBC("GameObjectDisplayInfo.dbc"); godi && godi->isLoaded()) {
        const auto* godiL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("GameObjectDisplayInfo") : nullptr;
        const uint32_t fc = godi->getFieldCount();
        for (uint32_t i = 0; i < godi->getRecordCount(); i++) {
            uint32_t displayId = godi->getUInt32(i, godiL ? (*godiL)["ID"] : 0);
            if (displayId != 0 && fc > 11) {
                GameObjectDisplaySounds snd;
                snd.openSoundId = godi->getUInt32(i, 3);
                snd.closeSoundId = godi->getUInt32(i, 5);
                snd.destroySoundId = godi->getUInt32(i, 6);
                for (uint32_t c = 0; c < 4; ++c) {
                    snd.customSoundIds[c] = godi->getUInt32(i, 8 + c);
                }
                if (snd.openSoundId || snd.closeSoundId || snd.destroySoundId ||
                    snd.customSoundIds[0] || snd.customSoundIds[1] ||
                    snd.customSoundIds[2] || snd.customSoundIds[3]) {
                    gameObjectDisplaySounds_[displayId] = snd;
                }
            }
            std::string modelName = godi->getString(i, godiL ? (*godiL)["ModelName"] : 1);
            if (modelName.empty()) continue;
            // GameObjectDisplayInfo names .mdx and .mdl alike; this knew only
            // the first, so a .mdl gameobject had no model path at all.
            modelName = pipeline::modelPathToM2(modelName);
            gameObjectDisplayIdToPath_[displayId] = modelName;
        }
        LOG_INFO("Loaded ", gameObjectDisplayIdToPath_.size(), " gameobject display mappings (",
                 gameObjectDisplaySounds_.size(), " with sounds)");
    } else {
        LOG_WARNING("GameObjectDisplayInfo.dbc failed to load - no GO display mappings available");
    }

    if (gameObjectDisplayIdToPath_.empty()) {
        LOG_WARNING("GO display mapping table is EMPTY - game objects will not render");
    }

    gameObjectLookupsBuilt_ = true;
}

std::string EntitySpawner::getGameObjectModelPathForDisplayId(uint32_t displayId) const {
    auto it = gameObjectDisplayIdToPath_.find(displayId);
    if (it == gameObjectDisplayIdToPath_.end()) return "";
    return it->second;
}


/// Which character instance draws `guid`, or 0 for one that is not drawn.
///
/// The player is asked for by name rather than looked up: the local character
/// is not in either map. Three callers - bounds, foot Z and position - each
/// had this, identically, and a fourth reader would have made it four.
uint32_t EntitySpawner::characterInstanceIdForGuid(uint64_t guid) const {
    if (!renderer_) return 0;
    if (gameHandler_ && guid == gameHandler_->getPlayerGuid()) {
        const uint32_t own = renderer_->getCharacterInstanceId();
        if (own != 0) return own;
    }
    auto pit = playerInstances_.find(guid);
    if (pit != playerInstances_.end()) return pit->second;
    auto cit = creatureInstances_.find(guid);
    if (cit != creatureInstances_.end()) return cit->second;
    return 0;
}

uint32_t EntitySpawner::drawnInstanceIdForGuid(uint64_t guid) const {
    if (const auto morph = playerMorphs_.find(guid); morph != playerMorphs_.end() && morph->second.instanceId)
        return morph->second.instanceId;
    if (const uint32_t inst = characterInstanceIdForGuid(guid); inst != 0)
        return inst;
    if (const auto replaced = replacedCreatureInstances_.find(guid); replaced != replacedCreatureInstances_.end())
        return replaced->second;
    return 0;
}

uint32_t EntitySpawner::groundInstanceIdForGuid(uint64_t guid) const {
    if (gameHandler_ && guid == gameHandler_->getPlayerGuid() && mountInstanceId_ != 0) return mountInstanceId_;
    if (const auto mount = remotePlayerMounts_.find(guid); mount != remotePlayerMounts_.end())
        return mount->second.instanceId;
    return characterInstanceIdForGuid(guid);
}

float EntitySpawner::getPlayerSwimmingDepth() const {
    return getUnitSwimmingDepth(gameHandler_->getPlayerGuid(), renderer_->getCharacterInstanceId());
}

float EntitySpawner::getUnitSwimmingDepth(uint64_t guid, uint32_t instanceId) const {
    const auto unit = std::dynamic_pointer_cast<game::Unit>(
        gameHandler_->getEntityManager().getEntity(guid));
    assert(unit);
    const auto& display = displayDataMap_.at(unit->getDisplayId());
    const float height = modelIdToCollisionHeight_.at(display.modelId);
    assert(height > 0.0f);
    const auto* cr = renderer_->getCharacterRenderer();
    return 0.75f * height * cr->getInstanceScale(instanceId);
}

bool EntitySpawner::getRenderBoundsForGuid(uint64_t guid, glm::vec3& outCenter, float& outRadius) const {
    if (!renderer_) return false;

    // M2 game objects (mailboxes, chests, nodes, etc.) render via the M2
    // renderer, not the character renderer, so their bounds come from a
    // different instance table. Their true world-space visual sphere makes
    // cursor picking track the actual model instead of a flat fallback.
    //
    // WMO game objects (buildings) are intentionally not resolved here: a
    // bounding sphere from a building's AABB diagonal is enormous and would
    // swallow nearby objects in the picker. They keep the conservative
    // fallback sphere used by the click handlers.
    auto goIt = gameObjectInstances_.find(guid);
    if (goIt != gameObjectInstances_.end()) {
        const auto& go = goIt->second;
        if (go.isWmo) return false;
        auto* m2 = renderer_->getM2Renderer();
        return m2 && m2->getInstanceBounds(go.instanceId, outCenter, outRadius);
    }

    if (!renderer_->getCharacterRenderer()) return false;
    const uint32_t instanceId = drawnInstanceIdForGuid(guid);
    if (instanceId == 0) return false;

    return renderer_->getCharacterRenderer()->getInstanceBounds(instanceId, outCenter, outRadius);
}

bool EntitySpawner::getSelectionRadiusForGuid(uint64_t guid, float& outRadius) const {
    ModelGeoBox box;
    float scale = 1.0f;
    if (!getUnitGeoBoxForGuid(guid, box, scale)) return false;
    outRadius = selectionCircleRadius(box, scale);
    return true;
}

bool EntitySpawner::getUnitGeoBoxForGuid(uint64_t guid, ModelGeoBox& outBox, float& outScale) const {
    if (!gameHandler_) return false;
    const auto unit = std::dynamic_pointer_cast<game::Unit>(
        gameHandler_->getEntityManager().getEntity(guid));
    if (!unit) return false;
    const auto sizeOf = [&](uint32_t displayId) -> const ModelSize* {
        const auto display = displayDataMap_.find(displayId);
        if (display == displayDataMap_.end()) return nullptr;
        const auto size = modelIdToSize_.find(display->second.modelId);
        return size != modelIdToSize_.end() ? &size->second : nullptr;
    };
    const uint32_t displayId = unit->getDisplayId();
    const ModelSize* rider = sizeOf(displayId);
    if (!rider) return false;

    // CGUnit_C vtable +0x58 (0x5ec9b0): OBJECT_FIELD_SCALE_X times the display
    // scale of 0x60bc70 (creatureRenderScale with a unit server scale), times
    // the mount display's CreatureModelScale while mounted (CGUnit_C+0xa4,
    // written by 0x6216b0 from the mount's CreatureDisplayInfo).
    float objectScale = 1.0f;
    const auto scaleField = unit->getFields().find(game::fieldIndex(game::UF::OBJECT_FIELD_SCALE_X));
    if (scaleField != unit->getFields().end()) {
        const float server = std::bit_cast<float>(scaleField->second);
        if (server > 0.0f) objectScale = server;
    }
    float scale = objectScale * creatureRenderScale(*unit, displayId, 1.0f);
    ModelGeoBox box = rider->geoBox;
    const uint32_t mountDisplay = unit->getMountDisplayId();
    if (mountDisplay != 0) {
        const ModelSize* mount = sizeOf(mountDisplay);
        if (!mount) return false;
        box = mountedGeoBox(box, mount->geoBox, mount->mountHeight);
        scale *= creatureDisplayScale(mountDisplay);
    }
    outBox = box;
    outScale = scale;
    return true;
}

std::vector<rendering::UnitShadowBlob> EntitySpawner::collectUnitShadows() const {
    std::vector<rendering::UnitShadowBlob> blobs;
    if (!gameHandler_ || !renderer_ || !renderer_->getCharacterRenderer()) return blobs;
    const auto* characters = renderer_->getCharacterRenderer();
    std::vector<uint64_t> guids;
    guids.reserve(creatureInstances_.size() + playerInstances_.size() + 1);
    for (const auto& [guid, instance] : creatureInstances_) guids.push_back(guid);
    for (const auto& [guid, instance] : playerInstances_) guids.push_back(guid);
    if (!playerInstances_.count(gameHandler_->getPlayerGuid())) guids.push_back(gameHandler_->getPlayerGuid());
    for (const uint64_t guid : guids) {
        // The drawn unit stands where its ground instance (its mount, when
        // riding) stands; a hidden or culled-by-room unit casts nothing.
        const uint32_t drawn = drawnInstanceIdForGuid(guid);
        const uint32_t ground = groundInstanceIdForGuid(guid);
        glm::vec3 drawnPosition;
        if (drawn == 0 || ground == 0 || !characters->getInstancePosition(drawn, drawnPosition)) continue;
        float opacity = 1.0f;
        if (!characters->isInstanceDrawnVisible(drawn, opacity) || characters->isInstanceRoomHidden(drawn)) continue;
        ModelGeoBox box;
        float scale = 1.0f;
        if (!getUnitGeoBoxForGuid(guid, box, scale)) continue;
        rendering::UnitShadowBlob blob;
        float yaw = 0.0f;
        if (!characters->getInstancePosition(ground, blob.position) || !characters->getInstanceYaw(ground, yaw))
            continue;
        blob.axisX = {std::cos(yaw), std::sin(yaw)};
        blob.halfExtent = glm::vec2(box.max - box.min) * (0.5f * scale);
        blob.reach = (box.max.z - box.min.z) * scale * rendering::kUnitShadowReachPerBoxHeight;
        if (blob.halfExtent.x <= 0.0f || blob.halfExtent.y <= 0.0f) continue;
        blobs.push_back(blob);
    }
    return blobs;
}

bool EntitySpawner::getRenderFootZForGuid(uint64_t guid, float& outFootZ) const {
    if (!renderer_ || !renderer_->getCharacterRenderer()) return false;
    const uint32_t instanceId = characterInstanceIdForGuid(guid);
    if (instanceId == 0) return false;

    return renderer_->getCharacterRenderer()->getInstanceFootZ(instanceId, outFootZ);
}

bool EntitySpawner::getRenderPositionForGuid(uint64_t guid, glm::vec3& outPos) const {
    if (!renderer_ || !renderer_->getCharacterRenderer()) return false;
    const uint32_t instanceId = characterInstanceIdForGuid(guid);
    if (instanceId == 0) return false;

    return renderer_->getCharacterRenderer()->getInstancePosition(instanceId, outPos);
}

EntitySpawner::CachedAttachmentModel
EntitySpawner::getOrLoadAttachmentModel(const std::vector<std::string>& candidatePaths,
                                        const std::string& texturePath) {
    // 1) Geometry: first candidate that yields a valid model wins, parsed at most once.
    std::shared_ptr<pipeline::M2Model> model;
    std::string resolvedPath;
    for (const auto& path : candidatePaths) {
        auto it = attachmentModelData_.find(path);
        if (it != attachmentModelData_.end()) {
            if (!it->second) continue;  // known missing - try the next candidate
            model = it->second;
            resolvedPath = path;
            break;
        }

        auto data = assetManager_->readFile(path);
        if (data.empty()) {
            attachmentModelData_[path] = nullptr;
            continue;
        }
        auto parsed = std::make_shared<pipeline::M2Model>(pipeline::M2Loader::load(data));
        if (parsed->name.empty()) parsed->name = path;
        // Skin is a sidecar file for WotLK M2s; vanilla embeds it.
        if (parsed->version >= 264) {
            std::string skinPath = pipeline::skinPathForM2(path);
            auto skinData = assetManager_->readFile(skinPath);
            if (!skinData.empty()) pipeline::M2Loader::loadSkin(skinData, *parsed);
        }
        if (!parsed->isValid()) {
            attachmentModelData_[path] = nullptr;
            continue;
        }
        attachmentModelData_[path] = parsed;
        model = std::move(parsed);
        resolvedPath = path;
        break;
    }
    if (!model) return {};

    // 2) Model id is per (geometry, texture) - see attachmentModelIds_.
    const std::string key = resolvedPath + '|' + texturePath;
    auto idIt = attachmentModelIds_.find(key);
    if (idIt == attachmentModelIds_.end()) {
        idIt = attachmentModelIds_.emplace(key, nextCreatureModelId_++).first;
    }
    return CachedAttachmentModel{.modelId = idIt->second, .model = std::move(model)};
}

std::string EntitySpawner::getHumanoidBakePath(uint32_t displayId) const {
    if (!assetManager_) return "";
    auto disp = displayDataMap_.find(displayId);
    if (disp == displayDataMap_.end() || disp->second.extraDisplayId == 0) return "";
    auto extra = humanoidExtraMap_.find(disp->second.extraDisplayId);
    if (extra == humanoidExtraMap_.end() || extra->second.bakeName.empty()) return "";

    // The bakes live in one directory and the dbc names only the file. It is
    // Textures\BakedNpcTextures, which is where the world path has always
    // looked; Creature\Baked is not a directory in any of this client's data,
    // so this returned nothing for every NPC there has ever been.
    const std::string path = "Textures\\BakedNpcTextures\\" + extra->second.bakeName;
    return assetManager_->fileExists(path) ? path : std::string();
}

bool EntitySpawner::getHumanoidAppearance(uint32_t displayId, uint8_t& race,
                                          uint8_t& sex, uint32_t& appearanceBytes,
                                          uint8_t& facialHair) const {
    auto disp = displayDataMap_.find(displayId);
    if (disp == displayDataMap_.end() || disp->second.extraDisplayId == 0) return false;
    auto extra = humanoidExtraMap_.find(disp->second.extraDisplayId);
    if (extra == humanoidExtraMap_.end()) return false;

    race = extra->second.raceId;
    sex = extra->second.sexId;
    facialHair = extra->second.facialHairId;
    appearanceBytes = static_cast<uint32_t>(extra->second.skinId)
                    | (static_cast<uint32_t>(extra->second.faceId) << 8)
                    | (static_cast<uint32_t>(extra->second.hairStyleId) << 16)
                    | (static_cast<uint32_t>(extra->second.hairColorId) << 24);
    // Only where there is a character model to load. This table gives naga,
    // broken, skeletons and a dozen other NPC-only races the same skin-and-face
    // columns a character has - about one row in fourteen - and the character
    // path answers HumanMale for every one of them. A human standing in the
    // target frame where a naga is standing is worse than the naga's own model
    // with no texture on it, which is what the creature path will give.
    return race != 0 && game::hasPlayerModel(static_cast<game::Race>(race));
}

std::vector<std::pair<uint32_t, uint8_t>>
EntitySpawner::getHumanoidEquipment(uint32_t displayId) const {
    std::vector<std::pair<uint32_t, uint8_t>> out;
    auto disp = displayDataMap_.find(displayId);
    if (disp == displayDataMap_.end() || disp->second.extraDisplayId == 0) return out;
    auto extra = humanoidExtraMap_.find(disp->second.extraDisplayId);
    if (extra == humanoidExtraMap_.end()) return out;

    // CreatureDisplayInfoExtra's slot order, against the inventory types
    // applyEquipment matches on. The order is the dbc's and the numbers are
    // WoW's INVTYPE_*, and the two have nothing to do with each other - which
    // is why this is written out rather than computed.
    static constexpr uint8_t kInvType[11] = {
        1,   // 0  helm      INVTYPE_HEAD
        3,   // 1  shoulder  INVTYPE_SHOULDER
        4,   // 2  shirt     INVTYPE_BODY
        5,   // 3  chest     INVTYPE_CHEST
        6,   // 4  belt      INVTYPE_WAIST
        7,   // 5  legs      INVTYPE_LEGS
        8,   // 6  feet      INVTYPE_FEET
        9,   // 7  wrist     INVTYPE_WRIST
        10,  // 8  hands     INVTYPE_HAND
        19,  // 9  tabard    INVTYPE_TABARD
        16,  // 10 cape      INVTYPE_CLOAK
    };
    for (int slot = 0; slot < 11; ++slot) {
        const uint32_t did = extra->second.equipDisplayId[slot];
        if (did != 0) out.emplace_back(did, kInvType[slot]);
    }
    return out;
}

std::vector<std::pair<uint32_t, std::string>>
EntitySpawner::getCreatureSkinPaths(uint32_t displayId,
                                    const std::string& modelPath) const {
    std::vector<std::pair<uint32_t, std::string>> out;
    if (!assetManager_) return out;
    auto it = displayDataMap_.find(displayId);
    if (it == displayDataMap_.end()) return out;

    std::string modelDir;
    if (const size_t slash = modelPath.rfind('\\'); slash != std::string::npos) {
        modelDir = modelPath.substr(0, slash + 1);
    }

    // Same resolution the spawner makes: the field may carry a directory or
    // not, and may carry the extension or not.
    auto resolve = [&](const std::string& skinField) -> std::string {
        if (skinField.empty()) return "";
        std::string raw = skinField;
        std::replace(raw.begin(), raw.end(), '/', '\\');
        auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
        raw.erase(raw.begin(), std::find_if(raw.begin(), raw.end(),
                  [&](unsigned char c) { return !isSpace(c); }));
        raw.erase(std::find_if(raw.rbegin(), raw.rend(),
                  [&](unsigned char c) { return !isSpace(c); }).base(), raw.end());
        if (raw.empty()) return "";

        std::string lower = raw;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const bool hasExt = lower.size() >= 4 &&
                            lower.compare(lower.size() - 4, 4, ".blp") == 0;
        const bool hasDir = raw.find('\\') != std::string::npos;

        std::vector<std::string> candidates;
        if (hasDir) {
            candidates.push_back(raw);
            if (!hasExt) candidates.push_back(raw + ".blp");
        } else {
            candidates.push_back(modelDir + raw);
            if (!hasExt) candidates.push_back(modelDir + raw + ".blp");
            candidates.push_back(raw);
            if (!hasExt) candidates.push_back(raw + ".blp");
        }
        for (const std::string& c : candidates) {
            if (assetManager_->fileExists(c)) return c;
        }
        return "";
    };

    // The bake first, where the display has one: it is the whole appearance
    // already composited, and the world puts it over these same slots. It is
    // what a goblin, a naga or a broken has instead of skin fields - those are
    // empty on a display whose appearance lives in CreatureDisplayInfoExtra,
    // and a creature loaded from them alone draws as a white silhouette.
    if (const std::string bake = getHumanoidBakePath(displayId); !bake.empty()) {
        for (uint32_t texType : {1u, 11u, 12u, 13u}) out.emplace_back(texType, bake);
    }

    const std::pair<uint32_t, const std::string*> kSkins[] = {
        {11u, &it->second.skin1}, {12u, &it->second.skin2}, {13u, &it->second.skin3},
    };
    for (const auto& [texType, field] : kSkins) {
        std::string path = resolve(*field);
        if (!path.empty()) out.emplace_back(texType, std::move(path));
    }
    return out;
}

// Apply the textures a creature display names: its own skin variations, and
// for a humanoid the composited body, face, hair and equipment. Once per
// display, since the model is shared by every creature that uses it.
//
// Lifted out of spawnOnlineCreature, which was 1477 lines and is now under a
// thousand. Nothing here changed; it moved.
// The animation a creature starts in.
//
// A creature is not always new when the client first draws it. The server may
// have told us it was dead, or working, or eating, before the spawn came off the
// queue - so the pose is chosen from what is already known about it, and only a
// creature with nothing known plays a birth animation. Fades it in either way.
// Native 2.4.3, Akama (21700) under Shape of the Bear (42377) killed with
// .die: the aura goes with the death, the bear turns back into Akama and that
// model plays the whole of Death, then lies. A corpse given another display
// later (.npc setmodel 21631) shows the new model lying at once
// (_experiments/20261118T1620Z.corpse_display_change, n1 and n2). WoWee
// rebuilt both standing. Nalorakk (23576) dying in bear form is the first.
void EntitySpawner::removeReplacedCreatureInstance(uint64_t guid) {
    const auto it = replacedCreatureInstances_.find(guid);
    if (it == replacedCreatureInstances_.end()) return;
    if (renderer_ && renderer_->getCharacterRenderer() && it->second != 0)
        renderer_->getCharacterRenderer()->removeInstance(it->second);
    replacedCreatureInstances_.erase(it);
}

void EntitySpawner::removeCreatureForNewDisplay(uint64_t guid) {
    uint32_t retainedEmote = 0;
    if (const auto emote = creatureActiveEmotes_.find(guid); emote != creatureActiveEmotes_.end())
        retainedEmote = emote->second;
    std::optional<std::vector<uint32_t>> retainedTransformVisuals;
    if (const auto tv = unitTransformAuraVisuals_.find(guid); tv != unitTransformAuraVisuals_.end())
        retainedTransformVisuals = std::move(tv->second);
    const bool corpse = deadCreatureGuids_.count(guid) != 0;
    bool dying = false;
    if (const auto instance = creatureInstances_.find(guid); corpse && instance != creatureInstances_.end()) {
        uint32_t animation = 0;
        float time = 0.0f, duration = 0.0f;
        dying = renderer_->getCharacterRenderer()->getAnimationState(instance->second, animation, time, duration) &&
                animation == rendering::anim::DEATH && time < duration;
    }
    despawnCreatureInternal(guid, /*holdInstanceForDisplaySwap=*/true);
    if (retainedEmote != 0) creatureActiveEmotes_[guid] = retainedEmote;
    if (retainedTransformVisuals)
        unitTransformAuraVisuals_.emplace(guid, std::move(*retainedTransformVisuals));
    if (corpse) markCreatureDead(guid);
    if (dying) dyingCreatureGuids_.insert(guid);
    displayChangedCreatureGuids_.insert(guid);
    LOG_DEBUG("Display change of 0x", std::hex, guid, std::dec, corpse ? (dying ? " (dying)" : " (corpse)") : "");
}

void EntitySpawner::playCreatureSpawnPose(uint64_t guid, uint32_t instanceId) {
    auto* charRenderer = renderer_->getCharacterRenderer();
    if (!charRenderer) return;
// Spawn in the correct pose. A creature the server marked dead before the
// queued spawn was processed is a corpse on arrival: Dead (6), not a replay
// of Death - unless its previous model was still falling
// (removeCreatureForNewDisplay), which goes on as Death on the new one.
// Consume displayChangedCreatureGuids_ unconditionally (dead or alive) so
// display swaps on dying/dead units neither leak the marker nor fade from 0.
const bool displayChanged = displayChangedCreatureGuids_.erase(guid) != 0;
if (deadCreatureGuids_.count(guid)) {
    if (dyingCreatureGuids_.erase(guid))
        charRenderer->playAnimation(instanceId, rendering::anim::DEATH, false);
    else
        charRenderer->playAnimation(instanceId, rendering::anim::DEAD, true);
} else {
    // Check if this NPC has a persistent emote state (e.g. working, eating, dancing)
    uint32_t npcEmote = 0;
    if (gameHandler_) {
        auto entity = gameHandler_->getEntityManager().getEntity(guid);
        if (entity && entity->getType() == game::ObjectType::UNIT) {
            npcEmote = std::static_pointer_cast<game::Unit>(entity)->getNpcEmoteState();
        }
    }
    uint32_t npcEmoteAnim = npcEmote != 0
        ? rendering::AnimationController::getEmoteAnimByEmotesId(npcEmote)
        : 0;
    if (npcEmoteAnim == 0) {
        auto activeIt = creatureActiveEmotes_.find(guid);
        if (activeIt != creatureActiveEmotes_.end()) {
            npcEmoteAnim = activeIt->second;
        }
    }
    if (npcEmoteAnim != 0 && charRenderer->hasAnimation(instanceId, npcEmoteAnim)) {
        creatureActiveEmotes_[guid] = npcEmoteAnim;
        charRenderer->playAnimation(instanceId, npcEmoteAnim, true);
    } else if (displayChanged) {
        // A new display on a creature already in the world is no new
        // creature: native 2.4.3 shows Tree Disguise (16977,
        // AshenvaleTreeFalling01.m2, Birth grows bone 1 from 0.09 over
        // 16.7 s) at full height right after .npc setmodel.
        charRenderer->playAnimation(instanceId, remoteIdlesInHover(guid)
            ? rendering::anim::FLY_IDLE : rendering::anim::STAND, true);
    } else if (charRenderer->hasAnimation(instanceId, rendering::anim::BIRTH)) {
        // Play birth animation (one-shot) - will return to STAND after
        charRenderer->playAnimation(instanceId, rendering::anim::BIRTH, false);
    } else if (charRenderer->hasAnimation(instanceId, rendering::anim::SPAWN)) {
        charRenderer->playAnimation(instanceId, rendering::anim::SPAWN, false);
    } else {
        // Already hovering or levitating when it came into view: Hover, not Stand.
        charRenderer->playAnimation(instanceId, remoteIdlesInHover(guid)
            ? rendering::anim::FLY_IDLE : rendering::anim::STAND, true);
    }
}
bool newlySpawned = false;
float targetOpacity = 1.0f;
if (gameHandler_) {
    if (auto entity = gameHandler_->getEntityManager().getEntity(guid);
        entity && entity->getType() == game::ObjectType::UNIT) {
        const auto* unit = static_cast<const game::Unit*>(entity.get());
        newlySpawned = !displayChanged && unit->isNewlySpawned();
        targetOpacity = creatureDisplayAlpha(unit->getDisplayId());
    }
}
// Seed applied display opacity before the periodic sync in EntitySpawner::update()
// runs; otherwise its first tick calls setInstanceOpacity() and cancels fade-in.
creatureAppliedOpacity_[guid] = targetOpacity;
const float fadeDuration = rendering::creatureSpawnFadeDuration(displayChanged, newlySpawned);
charRenderer->startFadeIn(instanceId, fadeDuration, targetOpacity);
}

// Choose one mesh per clothing group for a character-style NPC.
//
// These models carry every alternative the artists authored - six cloaks, a
// robe skirt and the trousers under it, several scalps - and a model drawn with
// all of them on shows a character wearing all of them at once. The player path
// avoids this by building a geoset set from the character's inventory; an NPC
// has no inventory to build one from, so its equipment comes from
// CreatureDisplayInfoExtra and only the clothing groups are touched. Everything
// else the model authored is left exactly as it is, because on a creature the
// same group numbers mean unrelated geometry.
void EntitySpawner::normalizeHumanoidClothingGeosets(uint32_t instanceId, uint32_t modelId,
                                                     uint32_t displayId) {
    auto* charRenderer = renderer_->getCharacterRenderer();
    if (!charRenderer) return;
    auto itDisplayData = displayDataMap_.find(displayId);
// With full humanoid overrides disabled, some character-style NPC models still render
// conflicting clothing geosets at once (global capes, robe skirts over trousers).
// Normalize only clothing groups while leaving all other model batches untouched.
if (const auto* md = charRenderer->getModelData(modelId)) {
    std::unordered_set<uint16_t> allGeosets;
    bool hasGroup3 = false;  // glove/forearm variants
    bool hasGroup4 = false;  // glove/forearm variants (some models)
    bool hasGroup5 = false;  // boot/shin variants
    bool hasGroup8 = false;  // sleeve/wrist variants
    bool hasGroup10 = false; // chest flap below the waist
    bool hasGroup12 = false; // tabard variants
    bool hasGroup13 = false; // trousers/robe skirt variants
    bool hasGroup15 = false; // cloak variants
    for (const auto& b : md->batches) {
        const uint16_t sid = b.submeshId;
        const uint16_t group = static_cast<uint16_t>(sid / 100);
        allGeosets.insert(sid);
        if (group == 3) hasGroup3 = true;
        if (group == 4) hasGroup4 = true;
        if (group == 5) hasGroup5 = true;
        if (group == 8) hasGroup8 = true;
        if (group == 10) hasGroup10 = true;
        if (group == 12) hasGroup12 = true;
        if (group == 13) hasGroup13 = true;
        if (group == 15) hasGroup15 = true;
    }

    // These numeric submesh groups only mean clothing on player-character
    // models. Creature models reuse the same IDs for unrelated authored
    // geometry (elementals use them for their built-in wrist pieces), so a
    // group-number heuristic alone can manufacture a second floating set of
    // "bracers". CreatureDisplayInfoExtra is the authoritative indication
    // that this display uses humanoid equipment geosets.
    const bool hasHumanoidDisplayExtra =
        itDisplayData != displayDataMap_.end() &&
        itDisplayData->second.extraDisplayId != 0 &&
        humanoidExtraMap_.find(itDisplayData->second.extraDisplayId) != humanoidExtraMap_.end();
    if (hasHumanoidDisplayExtra &&
        (hasGroup3 || hasGroup4 || hasGroup5 || hasGroup8 || hasGroup10 || hasGroup12 || hasGroup13 || hasGroup15)) {
        bool hasRenderableCape = false;
        std::string capeTexturePath;  // first found cape texture for override
        bool hasHumanoidExtra = false;
        uint8_t extraRaceId = 0;
        uint8_t extraSexId = 0;
        // 1 is the bald cap, and it is also what a missed lookup leaves here.
        //
        // That ambiguity was the standing suspect for "taking a helmet off does
        // not bring the hair back" - if the CharHairGeosets lookup always
        // missed, the selected scalp would always be bald, hair would be drawn
        // by texture alone, and the helm path's erase-group-0-and-insert-1
        // would be a visual no-op.
        //
        // Measured 2026-08-11 and it is not that. The DBC reads 339 records
        // across 334 distinct (race, sex, variation) keys and all 21 races; 22
        // set Showscalp and 25 more carry GeosetID 0, so 40 of 339 resolve to
        // the bald cap by design and the other 299 select a real geoset. The
        // lookup works. What is left of that bug is updateCharacterTextures,
        // which has not been traced.
        uint16_t selectedHairScalp = 1;
        uint16_t selectedFacial100 = 100;
        uint16_t selectedFacial200 = 200;
        uint16_t selectedFacial300 = 300;
        // Geoset groups the CreatureDisplayInfoExtra helm covers; applied last.
        std::vector<uint16_t> helmHiddenGroups;
        uint32_t equipChestGG = 0, equipLegsGG = 0, equipFeetGG = 0, equipGlovesGG = 0, equipCapeGG = 0;
        uint32_t equipRobeGG = 0;
        uint32_t equipChestFlapGG = 0;
        uint32_t equipKneepadsGG = 0;
        uint32_t equipTabardGG = 0;
        if (itDisplayData != displayDataMap_.end() &&
            itDisplayData->second.extraDisplayId != 0) {
            auto itExtra = humanoidExtraMap_.find(itDisplayData->second.extraDisplayId);
            if (itExtra != humanoidExtraMap_.end()) {
                hasHumanoidExtra = true;
                extraRaceId = itExtra->second.raceId;
                extraSexId = itExtra->second.sexId;
                const uint32_t hairKey = appearanceKey(
                    extraRaceId, extraSexId, itExtra->second.hairStyleId);
                auto itHairGeo = hairGeosetMap_.find(hairKey);
                if (itHairGeo != hairGeosetMap_.end() && itHairGeo->second > 0) {
                    selectedHairScalp = itHairGeo->second;
                }
                helmHiddenGroups = helmHiddenGeosetGroups(
                    *assetManager_, itExtra->second.equipDisplayId[0], extraRaceId, extraSexId);
                const uint32_t facialKey = appearanceKey(
                    extraRaceId, extraSexId, itExtra->second.facialHairId);
                auto itFacial = facialHairGeosetMap_.find(facialKey);
                if (itFacial != facialHairGeosetMap_.end()) {
                    // A zero variant means the character has none of that
                    // feature, and x00 is an id no model carries - which is
                    // what resolveGeoset reads as "none" further down.
                    selectedFacial100 = static_cast<uint16_t>(100 + itFacial->second.geoset100);
                    selectedFacial200 = static_cast<uint16_t>(200 + itFacial->second.geoset200);
                    selectedFacial300 = static_cast<uint16_t>(300 + itFacial->second.geoset300);
                }
                auto itemDisplayDbc = assetManager_->loadDBC("ItemDisplayInfo.dbc");
                const auto* idiL = pipeline::getActiveDBCLayout()
                    ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;

                uint32_t capeDisplayId = itExtra->second.equipDisplayId[10];
                if (capeDisplayId != 0 && itemDisplayDbc) {
                        int32_t recIdx = itemDisplayDbc->findRecordById(capeDisplayId);
                        if (recIdx >= 0) {
                            const uint32_t leftTexField = idiL ? (*idiL)["LeftModelTexture"] : 3u;
                            const uint32_t rightTexField = idiL ? (*idiL)["RightModelTexture"] : 4u;
                            std::vector<std::string> capeNames;
                            auto addName = [&](const std::string& n) {
                                if (!n.empty() &&
                                    std::find(capeNames.begin(), capeNames.end(), n) == capeNames.end()) {
                                    capeNames.push_back(n);
                                }
                            };
                            addName(itemDisplayDbc->getString(static_cast<uint32_t>(recIdx), leftTexField));
                            addName(itemDisplayDbc->getString(static_cast<uint32_t>(recIdx), rightTexField));

                            const bool npcIsFemale = (itExtra->second.sexId == 1);
                            // Same list, same order, one place:
                            // pipeline/item_textures.hpp.
                            std::vector<std::string> candidates;
                            for (const auto& raw : capeNames) {
                                for (auto& c : pipeline::capeTextureCandidates(raw, npcIsFemale)) {
                                    if (std::find(candidates.begin(), candidates.end(), c) ==
                                        candidates.end()) {
                                        candidates.push_back(std::move(c));
                                    }
                                }
                            }

                            for (const auto& p : candidates) {
                                if (assetManager_->fileExists(p)) {
                                    hasRenderableCape = true;
                                    capeTexturePath = p;
                                    break;
                                }
                            }
                        }
                }

                // Sleeves, gloves and boots use GeosetGroup1. Robes and
                // leg-slot kilts use GeosetGroup3, with chest taking priority.
                if (itemDisplayDbc) {
                    const uint32_t fGG1 = idiL ? (*idiL)["GeosetGroup1"] : 7;
                    const uint32_t fGG2 = fGG1 + 1; // Three contiguous GeosetGroup columns.
                    const uint32_t fGG3 = idiL ? (*idiL)["GeosetGroup3"] : 9;
                    auto readField = [&](uint32_t did, uint32_t field) -> uint32_t {
                        if (did == 0) return 0;
                        int32_t idx = itemDisplayDbc->findRecordById(did);
                        return (idx >= 0) ? itemDisplayDbc->getUInt32(static_cast<uint32_t>(idx), field) : 0;
                    };
                    auto readGG = [&](uint32_t did) { return readField(did, fGG1); };
                    equipChestGG = readGG(itExtra->second.equipDisplayId[3]);
                    if (equipChestGG == 0) equipChestGG = readGG(itExtra->second.equipDisplayId[2]); // shirt fallback
                    equipChestFlapGG = readField(itExtra->second.equipDisplayId[3], fGG2);
                    if (equipChestFlapGG == 0) equipChestFlapGG = readField(itExtra->second.equipDisplayId[2], fGG2);
                    equipLegsGG = readGG(itExtra->second.equipDisplayId[5]);
                    equipKneepadsGG = readField(itExtra->second.equipDisplayId[5], fGG2);
                    equipFeetGG = readGG(itExtra->second.equipDisplayId[6]);
                    equipGlovesGG = readGG(itExtra->second.equipDisplayId[8]);
                    equipCapeGG = readGG(itExtra->second.equipDisplayId[10]);
                    equipTabardGG = readGG(itExtra->second.equipDisplayId[9]);
                    equipRobeGG = readField(itExtra->second.equipDisplayId[3], fGG3);
                    if (equipRobeGG == 0)
                        equipRobeGG = readField(itExtra->second.equipDisplayId[5], fGG3);
                }
            }
        }

        std::unordered_set<uint16_t> normalizedGeosets;
        for (uint16_t sid : allGeosets) {
            const uint16_t group = static_cast<uint16_t>(sid / 100);
            if (group == 3 || group == 4 || group == 5 || group == 8 || group == 10 || group == 12 || group == 13 || group == 15) continue;
            // Group 17 = eye glow (DK/Night Elf "shining eyes" overlay), group 18 = related
            // glow geosets. NPCs are never DK/NE players opting into eye glow, so strip
            // these groups so creatures don't get unwanted glowing blue night-elf eyes.
            if (group == 17 || group == 18) continue;
            // Some humanoid models carry cloak cloth in group 16. Strip this too
            // when no cape is equipped to avoid "everyone has a cape".
            if (!hasRenderableCape && group == 16) continue;
            // Group 0 can contain multiple scalp/hair meshes. Keep only the selected
            // race/sex/style scalp to avoid overlapping broken hair.
            if (hasHumanoidExtra && sid < 100 && sid != 0 && sid != selectedHairScalp) {
                continue;
            }
            // Groups 1 and 2 are CharacterFacialHairStyles channels. A zero
            // variant is none and draws nothing: gnome male 5428 (facial 0)
            // is clean-shaven in native, gnome female 5435 (facial 0) wears no
            // earrings. Substituting the group's first member put a goatee
            // (102) and an earring (202) on them.
            if (hasHumanoidExtra && group == 1 && sid != resolveGeoset(selectedFacial100, allGeosets)) continue;
            if (hasHumanoidExtra && group == 2 && sid != resolveGeoset(selectedFacial200, allGeosets)) continue;
            normalizedGeosets.insert(sid);
        }

        // Intentionally do not add group 3 (glove/forearm accessory meshes).
        // Even "bare" variants can produce unwanted looped arm geometry on NPCs.

        // Group 4 is the forearms, so it is driven by the gloves. It used to be
        // driven by the boots - the feet value applied to the arm group, one
        // variant low - which the player and portrait paths never did.
        if (hasGroup4) {
            uint16_t wantForearms = (equipGlovesGG > 0)
                ? equippedGeoset(equipment::kGlovesBare, equipGlovesGG)
                : kGeosetBareForearms;
            uint16_t forearmSid = resolveGeoset(wantForearms, allGeosets);
            if (forearmSid != 0) normalizedGeosets.insert(forearmSid);
        }

        // Group 10 is the chest's flap below the waist, chosen by the chest's
        // GeosetGroup2 (1001 + value). It was never stripped, so every NPC wore
        // 1002, a skirt ring round the hips that native draws only for such a
        // chest (human female 11148 and gnome female 6714 have none).
        if (hasGroup10 && equipChestFlapGG > 0) {
            const uint16_t flapSid = resolveGeoset(
                equippedGeoset(equipment::kChestFlapBare, equipChestFlapGG), allGeosets);
            if (flapSid != 0) normalizedGeosets.insert(flapSid);
        }

        // Add sleeve/wrist meshes when chest armor calls for them.
        if (hasGroup8 && equipChestGG > 0) {
            uint16_t wantSleeves = equippedGeoset(equipment::kChestBare, equipChestGG);
            uint16_t sleeveSid = resolveGeoset(wantSleeves, allGeosets);
            if (sleeveSid != 0) normalizedGeosets.insert(sleeveSid);
        }

        // The tabard flap, selected by the tabard's GeosetGroup1 like every
        // other worn item: 1201 + value. Its print is in the bake (or the
        // torso regions); without the flap the gnome Scholomance Adept 11150
        // showed bare trousers where native hangs tabard 3454 to the knees.
        // A robe's skirt (group 13 from GeosetGroup3) takes its place: native
        // draws no flap on Father Gavin 1616 (legs robe), Scarlet Augur 2495
        // (chest robe) or Magus Zabraxis 16372, all wearing tabards.
        if (hasGroup12 && equipTabardGG > 0 && equipRobeGG == 0) {
            const uint16_t tabardSid = resolveGeoset(
                equippedGeoset(equipment::kTabardBare, equipTabardGG), allGeosets);
            if (tabardSid != 0) normalizedGeosets.insert(tabardSid);
        }

        // The third facial channel, group 3, under the same rule: zero is none.
        if (hasHumanoidExtra) {
            uint16_t facial300Sid = resolveGeoset(selectedFacial300, allGeosets);
            if (facial300Sid != 0) normalizedGeosets.insert(facial300Sid);
        }

        // Night Elf NPC eyes require the model's eye overlay. Continue to
        // strip it from other humanoids, but restore exactly one variant
        // for the race that actually uses it.
        if (hasHumanoidExtra && extraRaceId == 4) {
            uint16_t eyeGlowSid = resolveGeoset(kGeosetEyeGlow, allGeosets);
            if (eyeGlowSid != 0) normalizedGeosets.insert(eyeGlowSid);
        }

        // Legs' three authored fields control separate waist, knee and robe
        // groups. Keep one variant per group, including absent bare variants.
        selectNpcLowerBodyGeosets(normalizedGeosets, allGeosets,
            {equipFeetGG, equipLegsGG, equipKneepadsGG, equipRobeGG});

        // Group 15: cloak mesh. Use "with cape" when equipped, otherwise
        // use "no cape" back panel to cover the single-sided torso.
        if (hasGroup15) {
            if (hasRenderableCape) {
                uint16_t capeSid = resolveGeoset(cloakGeoset(equipCapeGG), allGeosets);
                if (capeSid != 0) normalizedGeosets.insert(capeSid);
            } else if (allGeosets.count(kGeosetNoCape) > 0) {
                // Only the real "no cape" panel, never a substitute. The
                // group's other members are cloaks, so falling back to the
                // first one hands a cape to a character wearing none - and
                // with no cloak texture bound, a white sheet. The HD models
                // have no 1501 at all, which is how every one of them came
                // to be wearing one.
                normalizedGeosets.insert(kGeosetNoCape);
            }
        }

        hideGeosetGroups(normalizedGeosets, helmHiddenGroups);

        if (!normalizedGeosets.empty()) {
            charRenderer->setActiveGeosets(instanceId, normalizedGeosets);
        }

        // Apply cape texture override to type-2 (Object Skin) slots only; group 15
        // also carries non-cape batches such as TaurenMale's tail (type 8) and
        // ScourgeMale's exposed spine (type 1).
        if (hasRenderableCape && !capeTexturePath.empty()) {
            rendering::VkTexture* capeTex = charRenderer->loadTexture(capeTexturePath);
            const rendering::VkTexture* whiteTex = charRenderer->loadTexture("");
            if (capeTex && capeTex != whiteTex) {
                if (const auto* md2 = charRenderer->getModelData(modelId)) {
                    for (size_t ti = 0; ti < md2->textures.size(); ti++) {
                        if (md2->textures[ti].type == 2) {
                            charRenderer->setTextureSlotOverride(instanceId, static_cast<uint16_t>(ti), capeTex);
                        }
                    }
                }
            }
        }
    }
}
}

// The per-instance colouring of a humanoid NPC: its hair, its skin, and the
// head-detail sheet an HD model draws its ears and eyes from.
//
// Per instance rather than per model, because two NPCs sharing one model still
// have their own hair colour - which is why these are texture slot overrides on
// the instance and not textures on the model.
void EntitySpawner::applyHumanoidInstanceOverrides(uint32_t instanceId, uint32_t modelId,
                                                   uint32_t displayId) {
    auto* charRenderer = renderer_->getCharacterRenderer();
    if (!charRenderer) return;
    if (!charSectionsCacheBuilt_) buildCharSectionsCache();
    auto itDD = displayDataMap_.find(displayId);
    if (itDD != displayDataMap_.end() && itDD->second.extraDisplayId != 0) {
        auto itExtra2 = humanoidExtraMap_.find(itDD->second.extraDisplayId);
        if (itExtra2 != humanoidExtraMap_.end()) {
            const auto& extra = itExtra2->second;
            const auto* md = charRenderer->getModelData(modelId);
            if (md) {
                    // Look up hair texture (section 3) via cache
                    rendering::VkTexture* whiteTex = charRenderer->loadTexture("");
                    std::string hairPath = lookupCharSection(
                        extra.raceId, extra.sexId, 3, extra.hairStyleId, extra.hairColorId, 0);
                    if (!hairPath.empty()) {
                        rendering::VkTexture* hairTex = charRenderer->loadTexture(hairPath);
                        if (hairTex && hairTex != whiteTex) {
                            for (size_t ti = 0; ti < md->textures.size(); ti++) {
                                if (md->textures[ti].type == 6) {
                                    charRenderer->setTextureSlotOverride(instanceId, static_cast<uint16_t>(ti), hairTex);
                                }
                            }
                        }
                    }

                    // The head detail sheet - eyes, mouth, ears, eyelashes -
                    // which an HD humanoid model asks for as texture type 8
                    // and the stock ones have no slot for. CharSections'
                    // second texture on the skin row is where it comes from,
                    // the same as for the player. Without it these slots
                    // keep whatever the model was authored with, and one of
                    // these models was authored with the word 'Ohren' - so
                    // the face detail fell back to the body art and every
                    // NPC wore its skin colour where its eyes should be.
                    {
                        std::string extraPath = lookupCharSection(
                            extra.raceId, extra.sexId, 0, 0, extra.skinId, 1);
                        int extraSlots = 0;
                        // Seven race and sex pairs name no extra art, and
                        // their models still carry 'Ohren' in the slot - a
                        // name that is not a file. The body skin is a poor
                        // substitute for an ear texture and a far better one
                        // than nothing, which is what those ears had.
                        if (extraPath.empty()) {
                            extraPath = lookupCharSection(
                                extra.raceId, extra.sexId, 0, 0, extra.skinId, 0);
                        }
                        rendering::VkTexture* extraTex =
                            extraPath.empty() ? nullptr : charRenderer->loadTexture(extraPath);
                        if (extraTex && extraTex != whiteTex) {
                            for (size_t ti = 0; ti < md->textures.size(); ti++) {
                                if (md->textures[ti].type == 8) {
                                    charRenderer->setTextureSlotOverride(
                                        instanceId, static_cast<uint16_t>(ti), extraTex);
                                    ++extraSlots;
                                }
                            }
                        }
                        // Three things decide whether an NPC's face is right,
                        // and a wrong face looks the same whichever failed:
                        // the table having the art, the art loading, and the
                        // model having a slot to put it in.
                        if (npcHeadDetailCanaryCount_ < 8) {
                            ++npcHeadDetailCanaryCount_;
                            // The face art this NPC was given, beside the
                            // face it was asked for. CharSections is keyed
                            // on (variation, colour) and a lookup that misses
                            // does not fail - it returns another row, and
                            // another row is another face.
                            const std::string faceLower = lookupCharSection(
                                extra.raceId, extra.sexId, 1, extra.faceId, extra.skinId, 0);
                            const std::string faceUpper = lookupCharSection(
                                extra.raceId, extra.sexId, 1, extra.faceId, extra.skinId, 1);
                            LOG_DEBUG("NPC head detail: displayId=", displayId,
                                        " race=", static_cast<int>(extra.raceId),
                                        " sex=", static_cast<int>(extra.sexId),
                                        " skin=", static_cast<int>(extra.skinId),
                                        " face=", static_cast<int>(extra.faceId),
                                        " extra='", extraPath,
                                        "' loaded=", (extraTex && extraTex != whiteTex ? "yes" : "NO"),
                                        " type8 slots=", extraSlots,
                                        " of ", md->textures.size(), " textures",
                                        " | faceLower='", faceLower,
                                        "' faceUpper='", faceUpper, "'");
                        }
                    }

                    // Look up skin texture (section 0) for per-instance skin color.
                    // Skip when the NPC has a baked texture or composited equipment -
                    // those already encode armor over skin and must not be replaced.
                    bool hasEquipOrBake = !extra.bakeName.empty();
                    if (!hasEquipOrBake) {
                        for (int s = 0; s < 11 && !hasEquipOrBake; s++)
                            if (extra.equipDisplayId[s] != 0) hasEquipOrBake = true;
                    }
                    if (!hasEquipOrBake) {
                        std::string skinPath = lookupCharSection(
                            extra.raceId, extra.sexId, 0, 0, extra.skinId, 0);
                        if (!skinPath.empty()) {
                            rendering::VkTexture* skinTex = charRenderer->loadTexture(skinPath);
                            if (skinTex) {
                                for (size_t ti = 0; ti < md->textures.size(); ti++) {
                                    uint32_t tt = md->textures[ti].type;
                                    if (tt == 1 || tt == 11) {
                                        charRenderer->setTextureSlotOverride(instanceId, static_cast<uint16_t>(ti), skinTex);
                                    }
                                }
                            }
                        }
                    }
            }
        }
    }
}

void EntitySpawner::applyCreatureDisplayTextures(uint32_t displayId, uint32_t modelId,
                                                 const CreatureDisplayData& dispData) {
    auto* charRenderer = renderer_->getCharacterRenderer();
    if (!charRenderer) return;
    auto texStart = std::chrono::steady_clock::now();
    displayIdTexturesApplied_.insert(displayId);

    // Use pre-decoded textures from async creature load (if available)
    auto itPreDec = displayIdPredecodedTextures_.find(displayId);
    bool hasPreDec = (itPreDec != displayIdPredecodedTextures_.end());
    if (hasPreDec) {
        charRenderer->setPredecodedBLPCache(&itPreDec->second);
    }

    // Creature skin names are relative to the model's own directory, so the
    // path is asked for here rather than passed in - the caller had it only
    // because it needed it for something else.
    const std::string m2Path = getModelPathForDisplayId(displayId);
    std::string modelDir;
    const size_t lastSlash = m2Path.find_last_of("\\/");
    if (lastSlash != std::string::npos) {
        modelDir = m2Path.substr(0, lastSlash + 1);
    }

    LOG_DEBUG("DisplayId ", displayId, " skins: '", dispData.skin1, "', '", dispData.skin2, "', '", dispData.skin3,
              "' extraDisplayId=", dispData.extraDisplayId);

    // Get model data from CharacterRenderer for texture iteration
    const auto* modelData = charRenderer->getModelData(modelId);
    if (!modelData) {
        LOG_WARNING("Model data not found for modelId ", modelId);
    }

    // Log texture types in the model
    if (modelData) {
    for (size_t ti = 0; ti < modelData->textures.size(); ti++) {
        LOG_DEBUG("  Model texture ", ti, ": type=", modelData->textures[ti].type, " filename='", modelData->textures[ti].filename, "'");
    }
    }

    // Check if this is a humanoid NPC with extra display info
    bool hasHumanoidTexture = false;
    if (dispData.extraDisplayId != 0) {
        auto itExtra = humanoidExtraMap_.find(dispData.extraDisplayId);
        if (itExtra != humanoidExtraMap_.end()) {
            const auto& extra = itExtra->second;
            LOG_DEBUG("  Found humanoid extra: raceId=", static_cast<int>(extra.raceId), " sexId=", static_cast<int>(extra.sexId),
                      " hairStyle=", static_cast<int>(extra.hairStyleId), " hairColor=", static_cast<int>(extra.hairColorId),
                      " bakeName='", extra.bakeName, "'");

            // Collect model texture slot info (type 1 = skin, type 6 = hair)
            std::vector<uint32_t> skinSlots, hairSlots;
            // Is this one of the replacement character models, rather than a
            // model the game shipped? The baked NPC textures were composited
            // for the shipped ones and are not interchangeable.
            //
            // Told by size, because size separates them cleanly and nothing
            // else does. The twenty character models the game ships run from
            // 3078 vertices to 8737; the twenty replacements run from 15051
            // to 87569. There is no overlap and the gap is wide.
            //
            // A Skin Extra slot was tried as the marker first and is not
            // one: only twelve of the twenty replacements carry it, and the
            // eight without - human male, dwarf, undead male, gnome, troll
            // male, draenei female - were exactly the ones left wrong.
            constexpr size_t kShippedCharacterVertexCeiling = 12000;
            const bool isHdCharacterModel =
                modelData && modelData->vertices.size() > kShippedCharacterVertexCeiling;
            if (modelData) {
                for (size_t ti = 0; ti < modelData->textures.size(); ti++) {
                    uint32_t texType = modelData->textures[ti].type;
                    if (texType == 1 || texType == 11 || texType == 12 || texType == 13)
                        skinSlots.push_back(static_cast<uint32_t>(ti));
                    if (texType == 6)
                        hairSlots.push_back(static_cast<uint32_t>(ti));
                }
            }

            // Copy extra data for the async task (avoid dangling reference)
            HumanoidDisplayExtra extraCopy = extra;

            // Launch async task: ALL DBC lookups, path resolution, and BLP pre-decode
            // happen on a background thread. Only GPU texture upload runs on main thread
            // (in processAsyncNpcCompositeResults).
            auto* am = assetManager_;
            AsyncNpcCompositeLoad load;
            load.future = std::async(std::launch::async,
                [am, extraCopy, skinSlots = std::move(skinSlots),
                 hairSlots = std::move(hairSlots), modelId, displayId, isHdCharacterModel]() mutable -> PreparedNpcComposite {
                    PreparedNpcComposite result;
                    DeferredNpcComposite& def = result.info;
                    def.modelId = modelId;
                    def.displayId = displayId;
                    def.skinTextureSlots = std::move(skinSlots);
                    def.hairTextureSlots = std::move(hairSlots);

                    std::vector<std::string> allPaths;  // paths to pre-decode

                    // --- Baked skin texture ---
                    //
                    // A bake is one image with the skin, the face and the
                    // armour already composited into it, made for a
                    // particular model. Taking it means CharSections is
                    // never read at all - which is the whole difference
                    // between this path and the portrait's, and why a
                    // portrait's face is right where the same NPC's is not.
                    //
                    // It is only right for the model it was baked for. These
                    // displays now point at the HD character models, whose
                    // faces the bakes know nothing about, so those composite
                    // from the table instead - the same way the portrait
                    // always has.
                    if (!extraCopy.bakeName.empty() && !isHdCharacterModel) {
                        def.bakedSkinPath = "Textures\\BakedNpcTextures\\" + extraCopy.bakeName;
                        def.hasBakedSkin = true;
                        allPaths.push_back(def.bakedSkinPath);
                    }

                    // --- CharSections fallback (skin/face/underwear) ---
                    if (!def.hasBakedSkin) {
                        auto csDbc = am->loadDBC("CharSections.dbc");
                        if (csDbc) {
                            const auto* csL = pipeline::getActiveDBCLayout()
                                ? pipeline::getActiveDBCLayout()->getLayout("CharSections") : nullptr;
                            auto csF = pipeline::detectCharSectionsFields(csDbc.get(), csL);
                            uint32_t npcRace = static_cast<uint32_t>(extraCopy.raceId);
                            uint32_t npcSex = static_cast<uint32_t>(extraCopy.sexId);
                            uint32_t npcSkin = static_cast<uint32_t>(extraCopy.skinId);
                            uint32_t npcFace = static_cast<uint32_t>(extraCopy.faceId);
                            std::vector<pipeline::SkinLayer> npcUnderwear;

                            // The one reader, in pipeline/char_sections.hpp.
                            //
                            // This copy had no fallback for a face the
                            // table does not carry, and no reading of the
                            // skin row's second texture. Both come with the
                            // conversion; the second is what an HD model
                            // draws its ears and eyelashes from.
                            pipeline::CharacterAppearance who;
                            who.raceId = npcRace;
                            who.sexId = npcSex;
                            who.skinId = static_cast<uint8_t>(npcSkin);
                            who.faceId = static_cast<uint8_t>(npcFace);
                            who.hairStyleId = extraCopy.hairStyleId;
                            who.hairColorId = extraCopy.hairColorId;
                            who.facialHairId = extraCopy.facialHairId;

                            const auto sections = pipeline::resolveCharacterSections(
                                csDbc.get(), csF, who,
                                [](const std::string& path, void* ctx) {
                                    return static_cast<pipeline::AssetManager*>(ctx)->fileExists(path);
                                },
                                am);

                            def.basePath = sections.bodySkin;
                            npcUnderwear = sections.underwear;

                            if (!def.basePath.empty()) {
                                allPaths.push_back(def.basePath);
                                std::vector<pipeline::SkinLayer> faceLayers;
                                pipeline::appendFaceOverlayLayers(faceLayers, sections);
                                for (const auto& fl : faceLayers) {
                                    def.overlayPaths.push_back(fl);
                                    allPaths.push_back(fl.path);
                                }
                                for (const auto& uw : npcUnderwear) { def.overlayPaths.push_back(uw); allPaths.push_back(uw.path); }
                            }
                        }
                    }

                    // --- Equipment region layers (ItemDisplayInfo DBC) ---
                    auto idiDbc = am->loadDBC("ItemDisplayInfo.dbc");
                    if (idiDbc) {
                        const auto* idiL = pipeline::getActiveDBCLayout()
                            ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;
                        uint32_t texRegionFields[8];
                        pipeline::getItemDisplayInfoTextureFields(*idiDbc, idiL, texRegionFields);
                        const bool npcIsFemale = (extraCopy.sexId == 1);
                        const bool npcHasArmArmor = (extraCopy.equipDisplayId[7] != 0 || extraCopy.equipDisplayId[8] != 0);

                        // Which regions of the skin atlas a piece of equipment paints.
                        //
                        // ItemDisplayInfo already answers that: a row names a
                        // texture for each region the item covers and leaves
                        // the rest empty. It is why the player path reads all
                        // eight and filters nothing at all - see the loop in
                        // entity_spawner_player.cpp, which the same items go
                        // through when a player wears them.
                        //
                        // The list here was narrower and dropped texture the
                        // items do name: across a 3.3.5 install's NPC displays,
                        // every belt's LegUpper (12790 of them), every boot's
                        // LegLower (10597), every glove's ArmLower (7908) and
                        // every bracer's ArmLower (3814). The bracers are where
                        // it was noticed - forearms wearing nothing on an NPC
                        // whose CreatureDisplayInfoExtra names a bracer and
                        // whose bracer names a texture.
                        //
                        // Helm, shoulder and cape stay out. Those are geometry
                        // rather than skin, they name no regions to begin with,
                        // and a helm painting a face would be worse than a helm
                        // that does not paint at all.
                        auto regionAllowedForNpcSlot = [](int eqSlot, int region) -> bool {
                            (void)region;
                            switch (eqSlot) {
                                case 0: case 1: case 10: return false;
                                default: return true;
                            }
                        };

                        for (int eqSlot = 0; eqSlot < 11; eqSlot++) {
                            uint32_t did = extraCopy.equipDisplayId[eqSlot];
                            if (did == 0) continue;
                            int32_t recIdx = idiDbc->findRecordById(did);
                            if (recIdx < 0) continue;

                            for (int region = 0; region < 8; region++) {
                                if (!regionAllowedForNpcSlot(eqSlot, region)) continue;
                                if (eqSlot == 2 && !npcHasArmArmor && !(region == 3 || region == 4)) continue;
                                std::string texName = idiDbc->getString(
                                    static_cast<uint32_t>(recIdx), texRegionFields[region]);
                                if (texName.empty()) continue;

                                std::string fullPath = pipeline::resolveItemRegionTexture(
                                    *am, region, texName, npcIsFemale);
                                if (fullPath.empty()) continue;

                                def.regionLayers.emplace_back(region, fullPath);
                                allPaths.push_back(fullPath);
                            }
                        }
                    }

                    // Determine compositing mode
                    if (!def.basePath.empty()) {
                        bool needsComposite = !def.overlayPaths.empty() || !def.regionLayers.empty();
                        if (needsComposite && !def.skinTextureSlots.empty()) {
                            def.hasComposite = true;
                        } else if (!def.skinTextureSlots.empty()) {
                            def.hasSimpleSkin = true;
                        }
                    }

                    // --- Hair texture from CharSections ---
                    // The one reader again, rather than a sixth scan of the
                    // table written out by hand. Only the hair is wanted here.
                    {
                        auto csDbc = am->loadDBC("CharSections.dbc");
                        if (csDbc) {
                            const auto* csL = pipeline::getActiveDBCLayout()
                                ? pipeline::getActiveDBCLayout()->getLayout("CharSections") : nullptr;
                            const auto csF = pipeline::detectCharSectionsFields(csDbc.get(), csL);
                            pipeline::CharacterAppearance who;
                            who.raceId = extraCopy.raceId;
                            who.sexId = extraCopy.sexId;
                            who.skinId = extraCopy.skinId;
                            who.faceId = extraCopy.faceId;
                            who.hairStyleId = extraCopy.hairStyleId;
                            who.hairColorId = extraCopy.hairColorId;
                            def.hairTexturePath =
                                pipeline::resolveCharacterSections(csDbc.get(), csF, who).hair;

                            if (!def.hairTexturePath.empty()) {
                                allPaths.push_back(def.hairTexturePath);
                            } else if (def.hasBakedSkin && !def.hairTextureSlots.empty()) {
                                def.useBakedForHair = true;
                                // bakedSkinPath already in allPaths
                            }
                        }
                    }

                    // --- Pre-decode all BLP textures on this background thread ---
                    for (const auto& path : allPaths) {
                        std::string key = path;
                        std::replace(key.begin(), key.end(), '/', '\\');
                        std::transform(key.begin(), key.end(), key.begin(),
                                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        if (result.predecodedTextures.count(key)) continue;
                        auto blp = am->loadTexture(key);
                        if (blp.isValid()) {
                            result.predecodedTextures[key] = std::move(blp);
                        }
                    }

                    return result;
                });
            asyncNpcCompositeLoads_.push_back(std::move(load));
            hasHumanoidTexture = true;  // skip non-humanoid skin block
        } else {
            LOG_WARNING("  extraDisplayId ", dispData.extraDisplayId, " not found in humanoidExtraMap");
        }
    }

    // Apply creature skin textures (for non-humanoid creatures)
    if (!hasHumanoidTexture && modelData) {
        auto resolveCreatureSkinPath = [&](const std::string& skinField) -> std::string {
            if (skinField.empty()) return "";

            std::string raw = skinField;
            std::replace(raw.begin(), raw.end(), '/', '\\');
            auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
            raw.erase(raw.begin(), std::find_if(raw.begin(), raw.end(), [&](unsigned char c) { return !isSpace(c); }));
            raw.erase(std::find_if(raw.rbegin(), raw.rend(), [&](unsigned char c) { return !isSpace(c); }).base(), raw.end());
            if (raw.empty()) return "";

            auto hasBlpExt = [](const std::string& p) {
                if (p.size() < 4) return false;
                std::string ext = p.substr(p.size() - 4);
                std::transform(ext.begin(), ext.end(), ext.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                return ext == ".blp";
            };
            auto addCandidate = [](std::vector<std::string>& out, const std::string& p) {
                if (p.empty()) return;
                if (std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
            };

            std::vector<std::string> candidates;
            const bool hasDir = (raw.find('\\') != std::string::npos || raw.find('/') != std::string::npos);
            const bool hasExt = hasBlpExt(raw);

            if (hasDir) {
                addCandidate(candidates, raw);
                if (!hasExt) addCandidate(candidates, raw + ".blp");
            } else {
                addCandidate(candidates, modelDir + raw);
                if (!hasExt) addCandidate(candidates, modelDir + raw + ".blp");
                addCandidate(candidates, raw);
                if (!hasExt) addCandidate(candidates, raw + ".blp");
            }

            for (const auto& c : candidates) {
                if (assetManager_->fileExists(c)) return c;
            }
            return "";
        };

        for (size_t ti = 0; ti < modelData->textures.size(); ti++) {
            const auto& tex = modelData->textures[ti];
            std::string skinPath;

            // Creature skin types: 11 = skin1, 12 = skin2, 13 = skin3
            if (tex.type == 11 && !dispData.skin1.empty()) {
                skinPath = resolveCreatureSkinPath(dispData.skin1);
            } else if (tex.type == 12 && !dispData.skin2.empty()) {
                skinPath = resolveCreatureSkinPath(dispData.skin2);
            } else if (tex.type == 13 && !dispData.skin3.empty()) {
                skinPath = resolveCreatureSkinPath(dispData.skin3);
            }

            if (!skinPath.empty()) {
                rendering::VkTexture* skinTex = charRenderer->loadTexture(skinPath);
                if (skinTex) {
                    charRenderer->setModelTexture(modelId, static_cast<uint32_t>(ti), skinTex);
                    LOG_DEBUG("Applied creature skin texture: ", skinPath, " to slot ", ti);
                } else {
                    // The row named a skin, the path was built, and the file
                    // behind it did not load. Said nothing before: the slot
                    // kept whatever it had and the creature drew wrong with
                    // no line anywhere saying which texture had not arrived.
                    LOG_WARNING("Creature skin did not load for displayId ", displayId,
                                " slot ", ti, " type ", tex.type, ": ", skinPath);
                }
            } else if ((tex.type == 11 && !dispData.skin1.empty()) ||
                       (tex.type == 12 && !dispData.skin2.empty()) ||
                       (tex.type == 13 && !dispData.skin3.empty())) {
                LOG_WARNING("Creature skin texture not found for displayId ", displayId,
                            " slot ", ti, " type ", tex.type,
                            " (skin fields: '", dispData.skin1, "', '",
                            dispData.skin2, "', '", dispData.skin3, "')");
            }
        }
    }

    // Clear pre-decoded cache after applying all display textures
    charRenderer->setPredecodedBLPCache(nullptr);
    displayIdPredecodedTextures_.erase(displayId);
    {
        auto texEnd = std::chrono::steady_clock::now();
        float texMs = std::chrono::duration<float, std::milli>(texEnd - texStart).count();
        if (texMs > 50.0f) {
            LOG_WARNING("spawnCreature texture setup took ", texMs, "ms displayId=", displayId,
                        " hasPreDec=", hasPreDec, " extra=", dispData.extraDisplayId);
        }
    }
}

float EntitySpawner::creatureModelScale(uint32_t displayId) const {
    auto disp = displayDataMap_.find(displayId);
    if (disp == displayDataMap_.end()) return 1.0f;
    auto it = modelIdToScale_.find(disp->second.modelId);
    return it != modelIdToScale_.end() ? it->second : 1.0f;
}

float EntitySpawner::creatureDisplayAlpha(uint32_t displayId) const {
    auto it = displayDataMap_.find(displayId);
    return it != displayDataMap_.end() ? it->second.modelAlpha : 1.0f;
}

float EntitySpawner::creatureDisplayScale(uint32_t displayId) const {
    auto it = displayDataMap_.find(displayId);
    return it != displayDataMap_.end() ? it->second.displayScale : 1.0f;
}

float EntitySpawner::creatureRenderScale(const game::Unit& unit, uint32_t displayId, float serverScale) const {
    const float base = creatureDisplayScale(displayId) * creatureModelScale(displayId);
    const CreatureFamilyScale* family = nullptr;
    const auto& cache = gameHandler_->getCreatureInfoCache();
    if (const auto info = cache.find(unit.getEntry()); info != cache.end()) {
        if (const auto found = creatureFamilyScales_.find(info->second.family); found != creatureFamilyScales_.end())
            family = &found->second;
    }
    const uint16_t petField = game::fieldIndex(game::UF::UNIT_FIELD_PETNUMBER);
    const bool pet = petField != 0xFFFF && unit.getField(petField) != 0;
    return serverScale * resolveCreatureFamilyScale(base, unit.getLevel(), pet, family);
}

// The per-instance part of a creature display's appearance: material rules,
// hair/skin/head overrides, one clothing mesh per group, and the shoulder and
// helmet models of a character-style display (CreatureDisplayInfoExtra).
void EntitySpawner::applyCreatureInstanceAppearance(uint32_t instanceId, uint32_t modelId, uint32_t displayId) {
    auto* charRenderer = renderer_->getCharacterRenderer();
    const auto itDisplayData = displayDataMap_.find(displayId);
    // CreatureDisplayInfoExtra identifies the character-style NPCs whose
    // character geosets and composite textures still need the legacy rules.
    // Other creature and vehicle atlases must obey their authored blend modes.
    const bool humanoidAppearance = itDisplayData != displayDataMap_.end() &&
        itDisplayData->second.extraDisplayId != 0 &&
        humanoidExtraMap_.find(itDisplayData->second.extraDisplayId) != humanoidExtraMap_.end();
    charRenderer->setInstanceCreatureMaterialRules(instanceId, !humanoidAppearance);

    // Per-instance hair, skin and head-detail overrides. These run for every
    // NPC, cached model or not, so two NPCs sharing a model still get their own
    // colouring.
    applyHumanoidInstanceOverrides(instanceId, modelId, displayId);
    // A humanoid NPC geoset mask used to be built here, behind
    // `static constexpr bool kEnableNpcSafeGeosetMask = false`. Same story as
    // the block below: disabled, unreachable, and edited tonight as though it
    // were live.

    // The humanoid geoset and equipment overrides that used to sit here are
    // gone. They were three hundred and ninety-five lines behind
    // `static constexpr bool kEnableNpcHumanoidOverrides = false`, and the
    // comment above them said why: too aggressive, made NPCs invisible.
    //
    // Code that cannot run is not a record of an idea, it is a place for
    // mistakes to hide. This block was edited twice tonight - once to stop a
    // geoset filter hiding a body, once to stop a zero facial-hair variant
    // becoming a beard - and neither edit could have done anything. Both were
    // real faults, and both had to be found again in the code that does run.
    //
    // git has it if the idea is wanted back.

    // Character-style NPC models can carry several conflicting clothing meshes
    // at once - a cape and no cape, a robe skirt over trousers. Pick one per
    // clothing group and leave every other batch of the model alone.
    normalizeHumanoidClothingGeosets(instanceId, modelId, displayId);

    if (humanoidAppearance) {
        const auto& extra = humanoidExtraMap_.at(itDisplayData->second.extraDisplayId);
        if (extra.equipDisplayId[1] != 0) {
            const auto dbc = assetManager_->loadDBC("ItemDisplayInfo.dbc");
            assert(dbc && dbc->isLoaded());
            const auto row = dbc->findRecordById(extra.equipDisplayId[1]);
            assert(row >= 0);
            const auto* layout = pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo");
            assert(layout);
            const std::string directory = "Item\\ObjectComponents\\Shoulder\\";
            const auto suffix = raceGenderSuffix(extra.raceId, extra.sexId);
            for (uint32_t side = 0; side < 2; ++side) {
                std::string modelName = dbc->getString(row,
                    (*layout)[side == 0 ? "LeftModel" : "RightModel"]);
                if (modelName.empty()) continue;
                if (const auto dot = modelName.rfind('.'); dot != std::string::npos)
                    modelName.resize(dot);
                const auto racialPath = directory + modelName + suffix + ".m2";
                const auto path = assetManager_->fileExists(racialPath)
                    ? racialPath : directory + modelName + ".m2";
                const auto textureName = dbc->getString(row,
                    (*layout)[side == 0 ? "LeftModelTexture" : "RightModelTexture"]);
                std::string texturePath;
                if (!textureName.empty()) {
                    const auto racialTexture = directory + textureName + suffix + ".blp";
                    texturePath = assetManager_->fileExists(racialTexture)
                        ? racialTexture : directory + textureName + ".blp";
                }
                pipeline::M2Model shoulder;
                if (!loadWeaponM2(path, shoulder) ||
                    !charRenderer->attachWeapon(instanceId, side == 0 ? 6 : 5, shoulder,
                                                 nextWeaponModelId_++, texturePath)) {
                    LOG_ERROR("NPC shoulder attachment failed: display=", displayId,
                              " side=", side, " path=", path);
                }
            }
        }
        const auto helm = resolveHelmVisual(*assetManager_, extra.equipDisplayId[0],
                                            extra.raceId, extra.sexId);
        if (helm.valid()) {
            const std::string path = !helm.racialModelPath.empty() &&
                assetManager_->fileExists(helm.racialModelPath)
                ? helm.racialModelPath : helm.baseModelPath;
            pipeline::M2Model model;
            if (!loadWeaponM2(path, model) ||
                !charRenderer->attachWeapon(instanceId, kAttachHelm, model,
                                             nextWeaponModelId_++, helm.texturePath)) {
                LOG_ERROR("NPC helmet attachment failed: display=", displayId, " path=", path);
            } else {
                LOG_DEBUG("Attached NPC helmet: display=", displayId, " path=", path);
            }
        }
    }

}

void EntitySpawner::spawnOnlineCreature(uint64_t guid, uint32_t displayId, float x, float y, float z, float orientation, float scale) {
    if (!renderer_ || !renderer_->getCharacterRenderer() || !assetManager_) return;

    // Skip if lookups not yet built (asset manager not ready)
    if (!creatureLookupsBuilt_) return;

    // Skip if already spawned
    if (creatureInstances_.count(guid)) return;
    if (nonRenderableCreatureDisplayIds_.count(displayId)) {
        removeReplacedCreatureInstance(guid);
        creaturePermanentFailureGuids_.insert(guid);
        return;
    }

    // Get model path from displayId
    std::string m2Path = getModelPathForDisplayId(displayId);
    if (m2Path.empty()) {
        removeReplacedCreatureInstance(guid);
        nonRenderableCreatureDisplayIds_.insert(displayId);
        creaturePermanentFailureGuids_.insert(guid);
        return;
    }

    auto* charRenderer = renderer_->getCharacterRenderer();

    // The model must already be in the cache: callers spawn only once the async load
    // has published it. Reading and parsing the M2 here instead would put file I/O on
    // the main thread mid-frame, so treat a miss as "not ready" and let the caller
    // re-queue the spawn through the async path.
    auto cacheIt = displayIdModelCache_.find(displayId);
    if (cacheIt == displayIdModelCache_.end()) {
        LOG_WARNING("spawnOnlineCreature: model not loaded yet for displayId=", displayId,
                    " - deferring to async load");
        return;
    }
    const uint32_t modelId = cacheIt->second;

    // Native TBC model size includes display/model scale and a family-level
    // lower bound. OBJECT_FIELD_SCALE_X multiplies that resolved model size.
    const float dispScale = creatureDisplayScale(displayId);
    const float serverScale = scale;
    auto entity = gameHandler_->getEntityManager().getEntity(guid);
    assert(entity && entity->getType() == game::ObjectType::UNIT);
    const auto& unit = static_cast<const game::Unit&>(*entity);
    scale = creatureRenderScale(unit, displayId, serverScale);

    {
        static std::set<uint32_t> saidDisplay;
        if (saidDisplay.size() < 60 && saidDisplay.insert(displayId).second) {
            std::string path = "?";
            uint32_t entry = 0;
            std::string name;
            if (auto disp = displayDataMap_.find(displayId); disp != displayDataMap_.end()) {
                if (auto pi = modelIdToPath_.find(disp->second.modelId); pi != modelIdToPath_.end()) {
                    path = pi->second;
                }
            }
            if (gameHandler_) {
                if (auto e = gameHandler_->getEntityManager().getEntity(guid)) {
                    if (e->getType() == game::ObjectType::UNIT) {
                        auto u = std::static_pointer_cast<game::Unit>(e);
                        entry = u->getEntry();
                        name = u->getName();
                    }
                }
            }
            // What every creature question needs first - a bat's size, a
            // goblin's portrait, an elemental's skin and an elemental's
            // geometry each began by working out which model a creature name
            // draws. At debug: sixty displays once each is most of a session's
            // log, and WOWEE_LOG_LEVEL=debug brings it back for the question.
            LOG_DEBUG("Creature display ", displayId, " (", (name.empty() ? "?" : name),
                        ", entry ", entry, ") draws ", path,
                        " at ", scale, " (server ", serverScale,
                        "; display ", dispScale, " model ", creatureModelScale(displayId),
                        " level ", unit.getLevel(), ")");
        }
    }

    // Apply skin textures from CreatureDisplayInfo.dbc (only once per displayId model).
    // Track separately from model cache because async loading may upload the model
    // before textures are applied.
    auto itDisplayData = displayDataMap_.find(displayId);
    bool needsTextures = (displayIdTexturesApplied_.find(displayId) == displayIdTexturesApplied_.end());
    if (needsTextures && itDisplayData != displayDataMap_.end()) {
        applyCreatureDisplayTextures(displayId, modelId, itDisplayData->second);
    }

    // Use the entity's latest server-authoritative position rather than the stale spawn
    // position. Movement packets (SMSG_MONSTER_MOVE) can arrive while a creature is still
    // queued in pendingCreatureSpawns_ and get silently dropped. getLatestX/Y/Z returns
    // the movement destination if the entity is mid-move, which is always up-to-date
    // regardless of distance culling (unlike getX/Y/Z which requires updateMovement).
    if (gameHandler_) {
        if (auto entity = gameHandler_->getEntityManager().getEntity(guid)) {
            x = entity->getLatestX();
            y = entity->getLatestY();
            z = entity->getLatestZ();
            orientation = entity->getOrientation();
        }
    }

    // Convert canonical → render coordinates
    glm::vec3 renderPos = core::coords::canonicalToRender(glm::vec3(x, y, z));

    // Keep authoritative server Z for online creature spawns.
    // Terrain-based lifting can incorrectly move elevated NPCs (e.g. flight masters on
    // Stormwind ramparts) to bad heights relative to WMO geometry.

    // Convert canonical WoW orientation (0=north) -> render yaw (0=west)
    float renderYaw = orientation + glm::radians(90.0f);

    // Create instance (apply server-provided scale from OBJECT_FIELD_SCALE_X)
    uint32_t instanceId = charRenderer->createInstance(modelId, renderPos,
        glm::vec3(0.0f, 0.0f, renderYaw), scale);

    if (instanceId == 0) {
        LOG_WARNING("Failed to create creature instance for guid 0x", std::hex, guid, std::dec);
        return;
    }
    LOG_DEBUG("Creature instance ", instanceId, " for guid 0x", std::hex, guid, std::dec, " display ", displayId);

    applyCreatureInstanceAppearance(instanceId, modelId, displayId);

    // Try attaching NPC held weapons; if update fields are not ready yet,
    // IN_GAME retry loop will attempt again shortly.
    bool weaponsAttachedNow = tryAttachCreatureVirtualWeapons(guid, instanceId);

    // Start the creature in the pose the server says it is already in: dead,
    // mid-emote, or newly arrived.
    playCreatureSpawnPose(guid, instanceId);

    if (const auto replaced = replacedCreatureInstances_.find(guid); replaced != replacedCreatureInstances_.end()) {
        if (auto* visuals = renderer_->getSpellVisualSystem())
            visuals->transferUnitVisuals(replaced->second, instanceId);
        if (replaced->second != 0)
            charRenderer->removeInstance(replaced->second);
        replacedCreatureInstances_.erase(replaced);
    }

    // Track instance
    creatureInstances_[guid] = instanceId;
    creatureModelIds_[guid] = modelId;
    creatureDisplayIds_[guid] = displayId;
    creatureRenderPosCache_[guid] = renderPos;
    // A display change rebuilds the rider and drops its mount with the old
    // instance; the retained mount field puts it back (.npc setmodel on a
    // mounted Wind Reaver left it on foot).
    if (unit.getMountDisplayId() != 0) setRemotePlayerMountDisplayId(guid, unit.getMountDisplayId());
    if (weaponsAttachedNow) {
        creatureWeaponsAttached_.insert(guid);
        creatureWeaponAttachAttempts_.erase(guid);
    } else {
        creatureWeaponsAttached_.erase(guid);
        creatureWeaponAttachAttempts_[guid] = 1;
    }
    LOG_DEBUG("Spawned creature: guid=0x", std::hex, guid, std::dec,
             " displayId=", displayId, " at (", x, ", ", y, ", ", z, ")");
}

} // namespace core
} // namespace wowee
