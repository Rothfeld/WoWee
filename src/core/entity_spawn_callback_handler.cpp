#include "core/entity_spawn_callback_handler.hpp"
#include "rendering/animation/spell_cast_anim_policy.hpp"
#include "core/entity_spawner.hpp"
#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include "game/game_object_interaction.hpp"
#include "rendering/renderer.hpp"
#include "rendering/placement_transform.hpp"
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

    // Native 2.4.3 rears the other player's mount (MountSpecial) once and
    // returns it to what it was doing; the rider stays seated.
    gameHandler_.setMountSpecialAnimCallback([this](uint64_t guid) {
        const auto* mount = entitySpawner_.getRemotePlayerMount(guid);
        auto* cr = renderer_.getCharacterRenderer();
        if (!mount || mount->instanceId == 0 || !cr) return;
        uint32_t current = rendering::anim::STAND;
        float t = 0.0f, duration = 0.0f;
        cr->getAnimationState(mount->instanceId, current, t, duration);
        cr->playAnimation(mount->instanceId, rendering::anim::MOUNT_SPECIAL, false, current);
    });

    // Creature despawn callback (online mode) - remove creature models
    gameHandler_.setCreatureDespawnCallback([this](uint64_t guid) {
        entitySpawner_.despawnCreatureFromServer(guid);
    });

    // Mirror image appearance data (SMSG_MIRRORIMAGE_DATA)
    gameHandler_.setMirrorImageDataCallback([this](uint64_t guid, const game::MirrorImageData& data) {
        entitySpawner_.applyCreatureMirrorImage(guid, data);
    });

    gameHandler_.setPlayerDespawnCallback([this](uint64_t guid) {
        entitySpawner_.despawnPlayer(guid);
    });

    // GameObject spawn callback (online mode) - spawn static models (mailboxes, etc.)
    gameHandler_.setGameObjectSpawnCallback([this](uint64_t guid, uint32_t entry, uint32_t displayId, float x, float y, float z, float orientation, float scale, const glm::quat& rotation) {
        entitySpawner_.queueGameObjectSpawn(guid, entry, displayId, x, y, z, orientation, scale, rotation);
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
        entitySpawner_.playGameObjectCustomSound(guid, animId);
        if (!m2r->hasAnimation(info.instanceId, *sequence)) {
            LOG_DEBUG("GO custom animation ", *sequence, " absent from model: guid=0x", std::hex, guid);
            return;
        }
        m2r->setInstanceAnimation(info.instanceId, *sequence, false);
    });

    // SMSG_GAMEOBJECT_SPAWN_ANIM_OBSOLETE (0x214): sent by CMaNGOS Totem::Summon
    // (Totem.cpp:104) and GO spawn triggers with the 8-byte GUID to play the
    // model's Birth (127) or Spawn (148) one-shot animation.
    gameHandler_.setGameObjectSpawnAnimCallback([this](uint64_t guid) {
        auto& goInstances = entitySpawner_.getGameObjectInstances();
        if (auto it = goInstances.find(guid); it != goInstances.end()) {
            const auto& info = it->second;
            if (info.isWmo) return;
            auto* m2r = renderer_.getM2Renderer();
            if (!m2r) return;
            if (m2r->hasAnimation(info.instanceId, rendering::anim::BIRTH)) {
                m2r->setInstanceAnimation(info.instanceId, rendering::anim::BIRTH, false);
            } else if (m2r->hasAnimation(info.instanceId, rendering::anim::SPAWN)) {
                m2r->setInstanceAnimation(info.instanceId, rendering::anim::SPAWN, false);
            }
            return;
        }
        const uint32_t instanceId = entitySpawner_.getCreatureInstanceId(guid);
        auto* cr = renderer_.getCharacterRenderer();
        if (!instanceId || !cr || cr->isInstanceDead(instanceId)) return;
        uint32_t curAnim = 0; float curT = 0.0f, curDur = 0.0f;
        if (cr->getAnimationState(instanceId, curAnim, curT, curDur) &&
            (curAnim == rendering::anim::BIRTH || curAnim == rendering::anim::SPAWN)) {
            return;
        }
        if (cr->hasAnimation(instanceId, rendering::anim::BIRTH)) {
            cr->playAnimation(instanceId, rendering::anim::BIRTH, false,
                              entitySpawner_.remoteIdleAnimation(guid));
        } else if (cr->hasAnimation(instanceId, rendering::anim::SPAWN)) {
            cr->playAnimation(instanceId, rendering::anim::SPAWN, false,
                              entitySpawner_.remoteIdleAnimation(guid));
        }
    });

    // GameObject state change callback: a door, a chest, a destructible is a
    // pose the server describes, and the spawner holds it - including the one
    // that arrives before the model does.
    gameHandler_.setGameObjectStateCallback([this](uint64_t guid, uint8_t goState) {
        entitySpawner_.applyGameObjectState(guid, goState);
    });
    gameHandler_.setGameObjectAnimProgressCallback([this](uint64_t guid, uint8_t animProgress) {
        entitySpawner_.applyGameObjectAnimProgress(guid, animProgress);
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
            const auto ent = gameHandler_.getEntityManager().getEntity(guid);
            const float speed = ent ? ent->getLocomotionSpeed() : 0.0f;
            if (rendering::shouldStartCreatureLocomotion(durationMs, speed)) {
                // Player animation is managed by the local renderer state machine -
                // don't reset it here or every server movement packet restarts the
                // run cycle from frame 0, causing visible stutter.
                if (!isPlayer) {
                    // Mounted NPCs share the per-frame mount/rider state machine.
                    // Mutating wasMoving here would consume its transition and
                    // leave the rider walking instead of seated on the mount.
                    if (entitySpawner_.getRemotePlayerMount(guid)) return;
                    const auto unit = std::dynamic_pointer_cast<game::Unit>(ent);
                    const auto standState = ent
                        ? static_cast<uint8_t>(ent->getField(game::fieldIndex(game::UF::UNIT_FIELD_BYTES_1)))
                        : uint8_t{0};
                    const bool deadOrCorpse = entitySpawner_.isCreatureDead(guid) ||
                        (unit && game::isUnitDeadPose(unit->getHealth(), unit->getMaxHealth(),
                                                      unit->getDynamicFlags(), standState,
                                                      unit->getNpcEmoteState()));
                    // Do not play locomotion or mark wasMoving while the server still
                    // holds the unit in a dead/corpse pose; once the server clears
                    // UNIT_STAND_STATE_DEAD / UNIT_DYNFLAG_DEAD, the render sync loop
                    // will see the false->true moving transition and start locomotion.
                    if (deadOrCorpse) return;
                    uint32_t curAnimId = 0; float curT = 0.0f, curDur = 0.0f;
                    auto* cr = renderer_.getCharacterRenderer();
                    bool gotState = cr->getAnimationState(instanceId, curAnimId, curT, curDur);
                    const bool walking = entitySpawner_.remoteGaitWalks(guid, instanceId);
                    const bool swimming = ent
                        ? entitySpawner_.isCreatureEffectivelySwimming(
                              guid, core::coords::canonicalToRender(
                                        glm::vec3(ent->getLatestX(), ent->getLatestY(), ent->getLatestZ())))
                        : (entitySpawner_.getCreatureSwimmingState().count(guid) > 0);
                    const bool flying = entitySpawner_.getCreatureFlyingState().count(guid) > 0;
                    const bool stealthed = unit && unit->hasCreepVisibility();
                    const uint32_t auraLocomotion = entitySpawner_.remoteAuraLocomotion(guid);
                    const uint32_t targetAnim = auraLocomotion != 0
                        ? auraLocomotion
                        : rendering::creatureMovingAnimation(flying, swimming, walking, stealthed);
                    // Only restart when the selected locomotion animation changed.
                    if (!gotState || curAnimId != targetAnim) {
                        cr->playAnimation(instanceId, targetAnim, /*loop=*/true);
                    }
                    uint32_t actualAnim = 0; float actualTime = 0.0f, actualDuration = 0.0f;
                    cr->getAnimationState(instanceId, actualAnim, actualTime, actualDuration);
                    LOG_DEBUG("Creature locomotion: guid=0x", std::hex, guid, std::dec,
                              " speed=", speed,
                              " walking=", walking, " requested=", targetAnim,
                              " actual=", actualAnim, " duration=", actualDuration);
                    entitySpawner_.getCreatureWasMoving()[guid] = true;
                }
            }
        }
    });

    gameHandler_.setGameObjectMoveCallback([this](uint64_t guid, float x, float y, float z, float /*orientation*/, const glm::quat& rotation) {
        auto& goInstMap = entitySpawner_.getGameObjectInstances();
        auto it = goInstMap.find(guid);
        if (it == goInstMap.end()) {
            return;
        }
        glm::vec3 renderPos = core::coords::canonicalToRender(glm::vec3(x, y, z));
        auto& info = it->second;
        if (info.isWmo) {
            if (auto* wr = renderer_.getWMORenderer()) {
                const glm::quat wmoRot = rotation * glm::angleAxis(-glm::half_pi<float>(), glm::vec3(0.0f, 0.0f, 1.0f));
                const glm::mat4 transform = rendering::placementModelMatrix(renderPos, wmoRot, info.scale);
                wr->setInstanceTransform(info.instanceId, transform);
            }
        } else {
            if (auto* mr = renderer_.getM2Renderer()) {
                const glm::mat4 transform = rendering::placementModelMatrix(renderPos, rotation, info.scale);
                mr->setInstanceTransform(info.instanceId, transform);
            }
        }
    });
}

}} // namespace wowee::core
