#include "ui/scene_pick.hpp"

#include "core/application.hpp"
#include "core/logger.hpp"
#include "core/coordinates.hpp"
#include "core/entity_spawner.hpp"
#include "rendering/character_renderer.hpp"
#include "game/game_handler.hpp"
#include "game/entity.hpp"
#include "game/game_object_interaction.hpp"
#include <cassert>
#include "rendering/camera.hpp"
#include "rendering/renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/terrain_manager.hpp"

#include <algorithm>
#include <cmath>

namespace wowee {
namespace ui {

namespace {

/// CGUnit_C 0x60e870: a dead unit the player can neither loot
/// (UNIT_DYNFLAG_LOOTABLE, which the server sets for the looter) nor skin
/// (UNIT_FLAG_SKINNABLE) is picked only when nothing else is under the cursor.
bool deferredCorpse(const std::shared_ptr<game::Entity>& entity) {
    auto unit = std::dynamic_pointer_cast<game::Unit>(entity);
    if (!unit || unit->getHealth() != 0 || unit->getMaxHealth() == 0) return false;
    const bool lootable = (unit->getDynamicFlags() & game::UNIT_DYNFLAG_LOOTABLE) != 0;
    const bool skinnable = (unit->getUnitFlags() & game::UNIT_FLAG_SKINNABLE) != 0;
    return !lootable && !skinnable;
}

bool interactiveCorpse(const std::shared_ptr<game::Entity>& entity) {
    auto unit = std::dynamic_pointer_cast<game::Unit>(entity);
    if (!unit || unit->getHealth() != 0 || unit->getMaxHealth() == 0) return false;
    const bool lootable = (unit->getDynamicFlags() & game::UNIT_DYNFLAG_LOOTABLE) != 0;
    const bool skinnable = (unit->getUnitFlags() & game::UNIT_FLAG_SKINNABLE) != 0;
    return lootable || skinnable;
}

} // namespace

ScenePick pickScene(game::GameHandler& gameHandler,
                    const rendering::Ray& ray,
                    const ScenePickParams& params,
                    const ScenePickHit& onHit) {
    ScenePick pick;
    const uint64_t myGuid = gameHandler.getPlayerGuid();
    uint64_t fallbackCorpseGuid = 0;
    float fallbackCorpseT = 1e30f;
    float fallbackCorpseCenterT = 0.0f;
    std::shared_ptr<game::Entity> fallbackCorpseEntity;

    for (const auto& [guid, entity] : gameHandler.getEntityManager().getEntities()) {
        if (!entity) continue;
        const auto type = entity->getType();
        if (type != game::ObjectType::UNIT &&
            type != game::ObjectType::PLAYER &&
            type != game::ObjectType::GAMEOBJECT) continue;
        if (!isScenePickCandidate(guid, myGuid, params.includeLocalPlayer)) continue;

        // Nor the scenery. A trigger is an ordinary unit on the wire - it has
        // health, a name and a model - and UNIT_FLAG_NOT_SELECTABLE is the
        // server saying it is not there to be clicked.
        if (type == game::ObjectType::UNIT) {
            auto unit = std::static_pointer_cast<game::Unit>(entity);
            if (unit->getUnitFlags() & game::UNIT_FLAG_NOT_SELECTABLE) continue;
        }

        const game::GameObjectQueryResponseData* goInfo = nullptr;
        bool highlightOnly = false;
        bool highlightable = false;
        if (type == game::ObjectType::GAMEOBJECT) {
            auto go = std::static_pointer_cast<game::GameObject>(entity);
            // Neither cursor nor name for an object the player may not use:
            // flagged off, or of a faction that regards the player as worse
            // than neutral (CGUnit_C::CanInteract).
            const bool enabled = gameHandler.canInteractWithGameObject(*go);
            if (params.trace) {
                glm::vec3 center;
                float radius = 0.0f, distance = 0.0f;
                const bool bounds = core::Application::getInstance().getRenderBoundsForGuid(
                    guid, center, radius);
                if (bounds && raySphereIntersect(ray, center, radius, distance)) {
                    LOG_DEBUG("Scene pick object candidate guid=", guid,
                        " entry=", go->getEntry(),
                        " flags=", go->getField(game::fieldIndex(game::UF::GAMEOBJECT_FLAGS)),
                        " dynamic=", go->getField(game::fieldIndex(game::UF::GAMEOBJECT_DYNAMIC)),
                        " enabled=", enabled, " boundsDistance=", distance);
                }
            }
            goInfo = gameHandler.getCachedGameObjectInfo(go->getEntry());
            // Until the template answers, the type is unknown and the object
            // offers nothing; it passes the pick through like scenery.
            if (!goInfo) continue;
            // The per-type table (game_object_interaction.hpp): an object
            // with neither a use nor a name does not take the mouse at all,
            // so it cannot hide what stands behind it. It may still light up
            // under the pointer (highlightObjectGuid).
            highlightOnly = !game::gameObjectAffordance(goInfo->type, goInfo->data[0], goInfo->data[1],
                                                        enabled).hoverable();
            // Flagged off or hostile (not enabled): nothing, as before.
            highlightable = enabled && game::gameObjectHighlights(goInfo->type, goInfo->data);
            if (highlightOnly && !highlightable) continue;
        }

        glm::vec3 hitCenter;
        float hitRadius = 0.0f;
        const bool hasBounds =
            core::Application::getInstance().getRenderBoundsForGuid(guid, hitCenter, hitRadius);
        if (!hasBounds) {
            // Match the hover-cursor sizes, so the reticle agrees with the
            // cursor affordance - otherwise NPCs feel hard to click.
            float heightOffset = params.unitHeightOffset;
            hitRadius = params.unitHitRadius;
            if (type == game::ObjectType::UNIT) {
                auto unit = std::static_pointer_cast<game::Unit>(entity);
                // Critters have very low max health.
                if (unit->getMaxHealth() > 0 && unit->getMaxHealth() < 100) {
                    hitRadius = 0.5f;
                    heightOffset = 0.3f;
                }
            } else if (type == game::ObjectType::GAMEOBJECT) {
                hitRadius = 2.5f;
                heightOffset = 1.2f;
            }
            hitCenter = core::coords::canonicalToRender(
                glm::vec3(entity->getX(), entity->getY(), entity->getZ()));
            hitCenter.z += heightOffset;
        } else if (type == game::ObjectType::GAMEOBJECT) {
            // Model-space sphere transformed without rotation-dependent inflation.
        } else {
            hitRadius = std::max(hitRadius * 1.25f, 1.0f);
        }

        float hitT = 0.0f;
        if (type == game::ObjectType::UNIT || type == game::ObjectType::PLAYER) {
            auto& app = core::Application::getInstance();
            auto* spawner = app.getEntitySpawner();
            auto* renderer = app.getRenderer();
            if (!spawner || !renderer || !renderer->getCharacterRenderer()) continue;
            // A unit not drawn because its room is not seen cannot be pointed at.
            if (renderer->getCharacterRenderer()->isInstanceRoomHidden(spawner->characterInstanceIdForGuid(guid))) continue;
            const uint32_t drawnInst = spawner->drawnInstanceIdForGuid(guid);
            if (!renderer->getCharacterRenderer()->intersectInstance(
                    drawnInst, ray.origin, ray.direction, hitT)) {
                // Pass-2 fallback for lootable/skinnable corpses whose death
                // animation fades out their body meshes (e.g. Entropius /
                // VoidGod.m2 leaving only a small floating orb). Used only when
                // Pass 1 finds no drawn surface under the cursor.
                if (type == game::ObjectType::UNIT && interactiveCorpse(entity)) {
                    glm::vec3 corpseOrigin = core::coords::canonicalToRender(
                        glm::vec3(entity->getX(), entity->getY(), entity->getZ()));
                    app.getRenderPositionForGuid(guid, corpseOrigin);
                    float selRadius = 0.0f;
                    app.getSelectionRadiusForGuid(guid, selRadius);
                    float boundRadius = 0.0f;
                    renderer->getCharacterRenderer()->getInstanceCollisionRadius(drawnInst, boundRadius);
                    const auto sphere = corpseFallbackPickSphere(corpseOrigin, selRadius, boundRadius);
                    float fbT = 0.0f;
                    if (raySphereIntersect(ray, sphere.center, sphere.radius, fbT) &&
                        fbT < fallbackCorpseT) {
                        bool blocked = false;
                        if (auto* wmo = renderer->getWMORenderer(); wmo &&
                            wmo->raycastVisibleGeometry(ray.origin, ray.direction, fbT) < fbT) {
                            blocked = true;
                        }
                        if (!blocked) {
                            if (auto* m2 = renderer->getM2Renderer(); m2 &&
                                m2->raycastVisibleGeometry(ray.origin, ray.direction, fbT, 0) < fbT) {
                                blocked = true;
                            }
                        }
                        if (!blocked) {
                            fallbackCorpseGuid = guid;
                            fallbackCorpseT = fbT;
                            fallbackCorpseCenterT = glm::dot(sphere.center - ray.origin, ray.direction);
                            fallbackCorpseEntity = entity;
                        }
                    }
                }
                if (params.trace) LOG_DEBUG("Scene pick unit volume miss guid=", guid);
                continue;
            }
        } else if (!raySphereIntersect(ray, hitCenter, hitRadius, hitT)) continue;
        uint32_t pickedM2 = 0;
        if (type == game::ObjectType::GAMEOBJECT) {
            auto& app = core::Application::getInstance();
            auto* spawner = app.getEntitySpawner();
            auto* renderer = app.getRenderer();
            if (!spawner || !renderer) continue;
            const auto& objects = spawner->getGameObjectInstances();
            const auto found = objects.find(guid);
            if (found == objects.end()) continue;
            if (!found->second.isWmo) {
                pickedM2 = found->second.instanceId;
                // Bounds are broad phase, not the surface occlusion must test.
                // When the camera is inside a large object's sphere its far
                // exit can lie behind terrain despite the mesh being visible.
                const float limit = hitT + 2.0f * hitRadius;
                auto* m2 = renderer->getM2Renderer();
                if (!m2) continue;
                hitT = m2->raycastVisibleGeometry(
                    ray.origin, ray.direction, limit, 0, pickedM2);
                if (hitT >= limit) continue;
                if (params.trace) LOG_DEBUG("Scene pick object mesh guid=", guid,
                    " instance=", pickedM2, " hit=", hitT);
            }
        }
        // Exclude the picked mesh from its own occlusion query: its collision
        // hull can protrude beyond the visible surface. Buildings hide what
        // they draw, not their invisible collision hulls.
        if (auto* renderer = core::Application::getInstance().getRenderer()) {
            if (auto* wmo = renderer->getWMORenderer(); wmo &&
                wmo->raycastVisibleGeometry(ray.origin, ray.direction, hitT) < hitT) {
                if (params.trace) LOG_DEBUG("Scene pick WMO blocked guid=", guid, " hit=", hitT,
                    " blocker=", wmo->raycastVisibleGeometry(ray.origin, ray.direction, hitT));
                continue;
            }
            if (auto* m2 = renderer->getM2Renderer(); m2 &&
                m2->raycastVisibleGeometry(ray.origin, ray.direction, hitT, pickedM2) < hitT) {
                if (params.trace) LOG_DEBUG("Scene pick M2 blocked guid=", guid, " hit=", hitT,
                    " blocker=", m2->raycastVisibleGeometry(ray.origin, ray.direction, hitT, pickedM2));
                continue;
            }
            if (auto* terrain = renderer->getTerrainManager(); terrain &&
                terrainOccludesSceneRay(ray, hitT, [terrain](float x, float y) {
                    return terrain->isHoleAt(x, y) ? std::optional<float>{}
                                                  : terrain->getHeightAt(x, y);
                })) {
                if (params.trace) LOG_DEBUG("Scene pick terrain blocked guid=", guid, " hit=", hitT,
                    " origin=", ray.origin.x, ",", ray.origin.y, ",", ray.origin.z);
                continue;
            }
        }
        const float centerT = glm::dot(hitCenter - ray.origin, ray.direction);

        if (type == game::ObjectType::UNIT || type == game::ObjectType::PLAYER) {
            if (deferredCorpse(entity)) {
                if (hitT < pick.deferredT) {
                    pick.deferredT = hitT;
                    pick.deferredGuid = guid;
                }
            } else if (hitT < pick.closestT) {
                pick.closestT = hitT;
                pick.closestGuid = guid;
                pick.closestInteractive = true;
            }
        } else if (type == game::ObjectType::GAMEOBJECT) {
            assert(goInfo);
            if (highlightable && hitT < pick.highlightObjectT) {
                pick.highlightObjectT = hitT;
                pick.highlightObjectGuid = guid;
            }
            if (highlightOnly) continue;
            const bool interactive = game::gameObjectTypeRule(goInfo->type).use;
            if (hitT < pick.closestT) {
                pick.closestT = hitT;
                pick.closestGuid = guid;
                pick.closestInteractive = interactive;
            }
            if (hitT < pick.namedObjectT) {
                pick.namedObjectT = hitT;
                pick.namedObjectGuid = guid;
            }
        }

        if (onHit) onHit(guid, entity, hitT, centerT);
    }

    if (pick.closestGuid == 0 && fallbackCorpseGuid != 0) {
        pick.closestGuid = fallbackCorpseGuid;
        pick.closestT = fallbackCorpseT;
        pick.closestInteractive = true;
        if (onHit) onHit(fallbackCorpseGuid, fallbackCorpseEntity, fallbackCorpseT, fallbackCorpseCenterT);
    }

    return pick;
}

} // namespace ui
} // namespace wowee
