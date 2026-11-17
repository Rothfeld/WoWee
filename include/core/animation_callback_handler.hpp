#pragma once

#include <unordered_map>

#include <cstdint>
#include <glm/glm.hpp>

namespace wowee {

namespace rendering { class Renderer; }
namespace game { class GameHandler; }
namespace core { class AppearanceComposer; class EntitySpawner; }

namespace core {

/// Handles animation callbacks: death, respawn, swing, hit reaction, spell cast, emote,
/// stun, stealth, health, ghost, stand state, loot, sprint, vehicle, charge.
/// Owns charge rush state (interpolated in update).
class AnimationCallbackHandler {
public:
    AnimationCallbackHandler(EntitySpawner& entitySpawner,
                             rendering::Renderer& renderer,
                             game::GameHandler& gameHandler,
                             AppearanceComposer& appearanceComposer);

    void setupCallbacks();

    /// Called each frame from Application::update() to drive charge interpolation.
    /// Returns true if charge is active (player is externally driven).
    bool updateCharge(float deltaTime);

    // Charge state queries (used by Application::update for externallyDrivenMotion)
    [[nodiscard]] bool isCharging() const { return chargeActive_; }

    // Reset charge state (logout/disconnect)
    void resetChargeState();

private:
    /// UnitBlood combat spurt on a wounded unit, at ChestBloodFront (15) when
    /// the attacker stands in front of it, ChestBloodBack (16) behind it.
    void playCombatBlood(uint64_t attackerGuid, uint64_t victimGuid, bool critical);

    EntitySpawner& entitySpawner_;
    rendering::Renderer& renderer_;
    game::GameHandler& gameHandler_;
    AppearanceComposer& appearanceComposer_;

    // Charge rush state (moved from Application)
    bool chargeActive_ = false;
    float chargeTimer_ = 0.0f;
    float chargeDuration_ = 0.0f;
    glm::vec3 chargeStartPos_{0.0f};  // Render coordinates
    glm::vec3 chargeEndPos_{0.0f};    // Render coordinates
    uint64_t chargeTargetGuid_ = 0;

    // Cast-kit animation started by SMSG_SPELL_GO, per caster: a channel of
    // the same spell starting right after holds its channel animation once
    // this one has played (Fishing: FishingCast, then FishingLoop).
    struct CastOneShot { uint32_t spellId; uint32_t animation; };
    std::unordered_map<uint64_t, CastOneShot> castOneShots_;
    // Precast or channel animation a caster holds. Only a held cast returns
    // to idle when it ends; anything else the unit is doing (walking,
    // fighting) is left alone. A channel's per-tick SMSG_SPELL_GO of another
    // spell (Arcane Missile 7268 under Arcane Missiles 5143) plays its cast
    // kit once and returns to the channel's hold.
    struct CastHold { uint32_t spellId; uint32_t animation; bool channel; };
    std::unordered_map<uint64_t, CastHold> castHolds_;
};

} // namespace core
} // namespace wowee
