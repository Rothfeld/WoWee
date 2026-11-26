#pragma once

#include "rendering/animation/spell_cast_anim_policy.hpp"

#include <unordered_map>

#include <cstdint>
#include <glm/glm.hpp>

namespace wowee {

namespace rendering { class Renderer; }
namespace game { class GameHandler; }
namespace core { class AppearanceComposer; class EntitySpawner; }

namespace core {

/// Resolve the active CharacterRenderer instance ID for a unit GUID.
///
/// Transformed/morphed players (e.g. Red Riding Hood 30753 -> GnomeFemale.m2,
/// Druid forms, Orb of Deception, Polymorph) render via a separate morph
/// instance while their native race instance stays hidden. Combat blood
/// spurts, hit reactions, spell visuals, swings, emotes, and death/respawn
/// animations must target `drawnInstanceId` whenever present so attachments
/// (ChestBloodFront 15 / ChestBloodBack 16) and skeletal poses match the
/// rendered model rather than the hidden native race model.
inline constexpr uint32_t resolveUnitRenderInstance(uint32_t drawnInstanceId,
                                                    bool isLocalPlayer,
                                                    uint32_t localCharacterInstanceId,
                                                    uint32_t creatureInstanceId,
                                                    uint32_t remotePlayerInstanceId) {
    if (drawnInstanceId != 0) return drawnInstanceId;
    if (isLocalPlayer) return localCharacterInstanceId;
    return creatureInstanceId != 0 ? creatureInstanceId : remotePlayerInstanceId;
}

/// Handles animation callbacks: death, respawn, swing, hit reaction, spell cast, emote,
/// stun, stealth, health, ghost, stand state, loot, sprint, vehicle.
class AnimationCallbackHandler {
public:
    AnimationCallbackHandler(EntitySpawner& entitySpawner,
                             rendering::Renderer& renderer,
                             game::GameHandler& gameHandler,
                             AppearanceComposer& appearanceComposer);

    void setupCallbacks();

private:
    /// Resolve the currently drawn CharacterRenderer instance for `guid`
    /// (preferring an active morph/shapeshift instance over the hidden base instance).
    uint32_t resolveUnitInstanceId(uint64_t guid) const;

    /// Reverse-lookup a remote unit GUID from its rendered CharacterRenderer instance ID.
    uint64_t resolveRemoteGuidForInstance(uint32_t instanceId) const;

    /// Shared TBC 2.4.3 wound-animation eligibility check for remote creatures and
    /// remote players across SMSG_ATTACKERSTATEUPDATE, SMSG_SPELLNONMELEEDAMAGELOG,
    /// SMSG_EMOTE (EMOTE_ONESHOT_WOUND 33 / EMOTE_ONESHOT_WOUNDCRITICAL 34), and
    /// SpellVisualKit ImpactKit wound animations (e.g. Fireball / Frostbolt / Fire Blast).
    bool canPlayRemoteWoundAnimation(uint64_t guid, uint32_t instanceId,
                                     uint32_t animId, bool isServerEmote) const;

    /// UnitBlood combat spurt on a wounded unit, at ChestBloodFront (15) when
    /// the attacker stands in front of it, ChestBloodBack (16) behind it.
    void playCombatBlood(uint64_t attackerGuid, uint64_t victimGuid, bool critical);

    EntitySpawner& entitySpawner_;
    rendering::Renderer& renderer_;
    game::GameHandler& gameHandler_;
    AppearanceComposer& appearanceComposer_;

    // Per-caster spell cast animation hold & active one-shot state (see
    // spell_cast_anim_policy.hpp for the TBC 2.4.3 completion & transition rules).
    std::unordered_map<uint64_t, rendering::SpellCastAnimState> castAnimStates_;
};

} // namespace core
} // namespace wowee
