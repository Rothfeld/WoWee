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
