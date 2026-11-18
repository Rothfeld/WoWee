#include "core/entity_spawn_callback_handler.hpp"
#include "core/entity_spawner.hpp"
#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include "game/game_object_interaction.hpp"
#include "rendering/renderer.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "rendering/animation/creature_locomotion.hpp"
#include "game/game_handler.hpp"

namespace wowee { namespace core {

EntitySpawnCallbackHandler::EntitySpawnCallbackHandler(
    EntitySpawner& entitySpawner,
    rendering::Renderer& renderer,
    game::GameHandler& gameHandler,
    std::function<bool(uint64_t)> isLocalPlayerGuid)
    : entitySpawner_(entitySpawner)
    , renderer_(renderer)
    , gameHandler_(gameHandler)
    , isLocalPlayerGuid_(std::move(isLocalPlayerGuid))
{
}

void EntitySpawnCallbackHandler::setupCallbacks() {
    // Creature spawn callback (online mode) - spawn creature models
    gameHandler_.setCreatureSpawnCallback([this](uint64_t guid, uint32_t displayId, float x, float y, float z, float orientation, float scale) {
        // Queue spawns to avoid hanging when many creatures appear at once.
        // A VALUES update can change a creature's display in place (Westfall
        // lumberjacks alternate between the normal and log-carrying models).
        // Replace the old render instance instead of retaining stale geometry.
        if (entitySpawner_.isCreatureSpawned(guid)) {
            const auto& displayIds = entitySpawner_.getCreatureDisplayIds();
            auto it = displayIds.find(guid);
            if (it != displayIds.end() && it->second == displayId) return;
            entitySpawner_.removeCreatureForNewDisplay(guid);
        }
        entitySpawner_.queueCreatureSpawn(guid, displayId, x, y, z, orientation, scale);
    });

    // Player spawn callback (online mode) - spawn player models with correct textures
    gameHandler_.setPlayerSpawnCallback([this](uint64_t guid,
                                              uint32_t /*displayId*/,
                                              uint8_t raceId,
                                              uint8_t genderId,
                                              uint32_t appearanceBytes,
                                              uint8_t facialFeatures,
                                              float x, float y, float z, float orientation) {
        LOG_DEBUG("playerSpawnCallback: guid=0x", std::hex, guid, std::dec,
                    " race=", static_cast<int>(raceId), " gender=", static_cast<int>(genderId),
                    " pos=(", x, ",", y, ",", z, ")");
        // Skip local player - already spawned as the main character
        if (isLocalPlayerGuid_(guid)) return;
        if (entitySpawner_.isPlayerSpawned(guid)) return;
        if (entitySpawner_.isPlayerPending(guid)) return;
        entitySpawner_.queuePlayerSpawn(guid, raceId, genderId, appearanceBytes, facialFeatures, x, y, z, orientation);
    });

    // Online player equipment callback - apply armor geosets/skin overlays per player instance.
    gameHandler_.setPlayerEquipmentCallback([this](uint64_t guid,
                                                  const std::array<uint32_t, 19>& displayInfoIds,
                                                  const std::array<uint8_t, 19>& inventoryTypes) {
        // Queue equipment compositing instead of doing it immediately -
        // compositeWithRegions is expensive (file I/O + CPU blit + GPU upload)
        // and causes frame stutters if multiple players update at once.
        entitySpawner_.queuePlayerEquipment(guid, displayInfoIds, inventoryTypes);
    });

    gameHandler_.setOtherPlayerMountCallback([this](uint64_t guid, uint32_t mountDisplayId) {
        if (isLocalPlayerGuid_(guid)) return;
        entitySpawner_.setRemotePlayerMountDisplayId(guid, mountDisplayId);
    });

    // Creature despawn callback (online mode) - remove creature models
    gameHandler_.setCreatureDespawnCallback([this](uint64_t guid) {
        entitySpawner_.despawnCreatureFromServer(guid);
    });

    gameHandler_.setPlayerDespawnCallback([this](uint64_t guid) {
        entitySpawner_.despawnPlayer(guid);
    });

    // GameObject spawn callback (online mode) - spawn static models (mailboxes, etc.)
    gameHandler_.setGameObjectSpawnCallback([this](uint64_t guid, uint32_t entry, uint32_t displayId, float x, float y, float z, float orientation, float scale) {
        entitySpawner_.queueGameObjectSpawn(guid, entry, displayId, x, y, z, orientation, scale);
    });

    // GameObject despawn callback (online mode) - remove static models
    gameHandler_.setGameObjectDespawnCallback([this](uint64_t guid) {
        entitySpawner_.despawnGameObjectFromServer(guid);
    });

    // GameObject metadata arrival - settles the animation policy for models that
    // spawned before their type was known.
    gameHandler_.setGameObjectInfoCallback([this](uint32_t entry) {
        entitySpawner_.onGameObjectInfoReceived(entry);
    });

    // SMSG_GAMEOBJECT_CUSTOM_ANIM names custom animation N (0-3), which is
    // AnimationData Custom0-Custom3 (153-156), not animation id N: CMaNGOS
    // sends it for GameObjectActions ANIMATE_CUSTOM_0-3 (SPELL_EFFECT_
    // ACTIVATE_OBJECT). Zul'Aman's Strange Gong (G_GongTroll01, Stand 0 and
    // Custom0 153) swings on each use natively (Zul'Aman Object Visual 45222,
    // misc 1 = Custom0); playing animation 0 left it still.
    gameHandler_.setGameObjectCustomAnimCallback([this](uint64_t guid, uint32_t animId) {
        auto& goInstances = entitySpawner_.getGameObjectInstances();
        auto it = goInstances.find(guid);
        if (it == goInstances.end()) return;
        auto& info = it->second;
        if (info.isWmo) return;
        auto* m2r = renderer_.getM2Renderer();
        if (!m2r) return;
        const auto sequence = game::gameObjectCustomAnimation(animId);
        if (!sequence) {
            LOG_WARNING("GO custom animation ", animId, " outside Custom0-3: guid=0x", std::hex, guid);
            return;
        }
        if (!m2r->hasAnimation(info.instanceId, *sequence)) {
            LOG_DEBUG("GO custom animation ", *sequence, " absent from model: guid=0x", std::hex, guid);
            return;
        }
        m2r->setInstanceAnimation(info.instanceId, *sequence, false);
    });

    // GameObject state change callback: a door, a chest, a destructible is a
    // pose the server describes, and the spawner holds it - including the one
    // that arrives before the model does.
    gameHandler_.setGameObjectStateCallback([this](uint64_t guid, uint8_t goState) {
        entitySpawner_.applyGameObjectState(guid, goState);
    });

    // Creature move callback (online mode) - update creature positions
    gameHandler_.setCreatureMoveCallback([this](uint64_t guid, float, float, float, uint32_t durationMs) {
        if (!renderer_.getCharacterRenderer()) return;
        uint32_t instanceId = 0;
        bool isPlayer = false;
        instanceId = entitySpawner_.getPlayerInstanceId(guid);
        if (instanceId != 0) { isPlayer = true; }
        else {
            instanceId = entitySpawner_.getCreatureInstanceId(guid);
        }
        if (instanceId != 0) {
            // Entity evaluates positions for both players and NPCs. The per-frame
            // render sync copies that result; a renderer interpolation here would
            // compete with it and restart on every movement packet.
            // Play the server-selected ground locomotion animation for the
            // duration of the spline move: Walk or Run from the spline speed
            // the entity took from this SMSG_MONSTER_MOVE before the callback.
            // WoW M2 animation IDs: 4=Walk, 5=Run.
            // Don't override Death animation (1). The per-frame sync loop will return to
            // Stand when movement stops.
            if (durationMs > 0) {
                // Player animation is managed by the local renderer state machine -
                // don't reset it here or every server movement packet restarts the
                // run cycle from frame 0, causing visible stutter.
                if (!isPlayer) {
                    // Mounted NPCs share the per-frame mount/rider state machine.
                    // Mutating wasMoving here would consume its transition and
                    // leave the rider walking instead of seated on the mount.
                    if (entitySpawner_.getRemotePlayerMount(guid)) return;
                    uint32_t curAnimId = 0; float curT = 0.0f, curDur = 0.0f;
                    auto* cr = renderer_.getCharacterRenderer();
                    bool gotState = cr->getAnimationState(instanceId, curAnimId, curT, curDur);
                    const bool walking = entitySpawner_.remoteGaitWalks(guid, instanceId);
                    const bool swimming = entitySpawner_.getCreatureSwimmingState().count(guid) > 0;
                    const bool flying = entitySpawner_.getCreatureFlyingState().count(guid) > 0;
                    const uint32_t targetAnim = rendering::creatureMovingAnimation(flying, swimming, walking);
                    // Only restart when the selected locomotion animation changed.
                    if (!gotState || (!rendering::anim::isCorpsePose(curAnimId) && curAnimId != targetAnim)) {
                        cr->playAnimation(instanceId, targetAnim, /*loop=*/true);
                    }
                    uint32_t actualAnim = 0; float actualTime = 0.0f, actualDuration = 0.0f;
                    cr->getAnimationState(instanceId, actualAnim, actualTime, actualDuration);
                    LOG_DEBUG("Creature locomotion: guid=0x", std::hex, guid, std::dec,
                              " speed=", gameHandler_.getEntityManager().getEntity(guid)->getLocomotionSpeed(),
                              " walking=", walking, " requested=", targetAnim,
                              " actual=", actualAnim, " duration=", actualDuration);
                    entitySpawner_.getCreatureWasMoving()[guid] = true;
                }
            }
        }
    });

    gameHandler_.setGameObjectMoveCallback([this](uint64_t guid, float x, float y, float z, float orientation) {
        auto& goInstMap = entitySpawner_.getGameObjectInstances();
        auto it = goInstMap.find(guid);
        if (it == goInstMap.end()) {
            return;
        }
        glm::vec3 renderPos = core::coords::canonicalToRender(glm::vec3(x, y, z));
        auto& info = it->second;
        if (info.isWmo) {
            if (auto* wr = renderer_.getWMORenderer()) {
                glm::mat4 transform(1.0f);
                transform = glm::translate(transform, renderPos);
                transform = glm::rotate(transform, orientation, glm::vec3(0, 0, 1));
                wr->setInstanceTransform(info.instanceId, transform);
            }
        } else {
            if (auto* mr = renderer_.getM2Renderer()) {
                glm::mat4 transform(1.0f);
                transform = glm::translate(transform, renderPos);
                mr->setInstanceTransform(info.instanceId, transform);
            }
        }
    });
}

}} // namespace wowee::core
